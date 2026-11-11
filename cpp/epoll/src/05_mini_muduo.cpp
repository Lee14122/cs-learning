/*
 * 实验 05：迷你版 muduo —— 用 ~300 行把前 4 个实验"零件"装成一台服务器
 *
 *   MiniPoller  ↔ EPollPoller.cc   epoll 三件套封装 + kNew/kAdded/kDeleted 状态机
 *   MiniChannel ↔ Channel.cc       fd + 感兴趣事件 + 事件回调
 *   MiniLoop    ↔ EventLoop.cc     事件循环 + eventfd wakeup + 跨线程 queueInLoop
 *
 * 程序流程：
 *   [主线程]    跑事件循环，提供一个 127.0.0.1 的 echo 服务（纯单线程，零 IO 锁）
 *   [客户端线程] 连接、发 3 条消息、收 echo —— 你看到 EPOLLIN→回调→读写 的完整链路
 *   ★ 压轴测量：客户端线程调用 loop.queueInLoop() 排入一个回调，
 *     而此刻主线程正阻塞在 5 秒超时的 epoll_wait 上。
 *     从"入队"到"回调在主线程被执行"的延迟是微秒级 —— 靠的就是
 *     write(wakeupFd_) 让 epoll_wait 提前返回（EventLoop.cc:168 那 6 行代码）。
 *     没有 eventfd，这个回调要干等 5 秒 —— 这就是 muduo 设计 wakeupFd_ 的原因，
 *     也是你以后 RpcClient 线程安全接口的地基。
 *
 * 编译: g++ -std=c++17 -O2 -pthread -o 05_mini_muduo src/05_mini_muduo.cpp
 * 运行: ./bin/05_mini_muduo   （约 1 秒自动结束）
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

/* ---- 状态常量：照搬 EPollPoller.cc:9-11 ---- */
const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;

class MiniLoop;

/* ================= MiniChannel：fd + events + 回调（↔ Channel.h/.cc） ================= */
class MiniChannel {
public:
    using CallBack = std::function<void()>;

    MiniChannel(MiniLoop *loop, int fd) : loop_(loop), fd_(fd) {}

    void setReadCallback(CallBack cb) { readCallback_ = std::move(cb); }
    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int r) { revents_ = r; }
    int index() const { return index_; }
    void set_index(int i) { index_ = i; }

    void enableReading();   // ↔ Channel::enableReading()：改 events_ 后让 loop 去 epoll_ctl
    void remove();          // ↔ Channel::remove()
    void handleEvent();     // ↔ Channel::handleEvent()：按 revents_ 分发回调

private:
    MiniLoop *loop_;
    int fd_;
    int events_ = 0;
    int revents_ = 0;
    int index_ = kNew;
    CallBack readCallback_;
};

/* ================= MiniPoller：epoll 三个接口的面向对象包装 ================= */
class MiniPoller {
public:
    explicit MiniPoller(MiniLoop *loop)
        : loop_(loop)
        , epollfd_(::epoll_create1(EPOLL_CLOEXEC))   // ↔ EPollPoller.cc:15
        , events_(16)                                // ↔ kInitEventListSize
    {}
    ~MiniPoller() { ::close(epollfd_); }

    // ↔ EPollPoller::poll + fillActiveChannels（EPollPoller.cc:29-60,113-121）
    std::vector<MiniChannel *> poll(int timeoutMs) {
        std::vector<MiniChannel *> active;
        int n = ::epoll_wait(epollfd_, &*events_.begin(),
                             static_cast<int>(events_.size()), timeoutMs);
        if (n < 0 && errno != EINTR) { perror("epoll_wait"); return active; }
        for (int i = 0; i < n; ++i) {
            MiniChannel *ch = static_cast<MiniChannel *>(events_[i].data.ptr); // ← 往返
            ch->set_revents(events_[i].events);
            active.push_back(ch);
        }
        if (n == static_cast<int>(events_.size()))
            events_.resize(n * 2);                   // ↔ EPollPoller.cc:42 扩容
        return active;
    }

    // ↔ EPollPoller::updateChannel（EPollPoller.cc:63-94）：状态机 → ADD/MOD/DEL
    void updateChannel(MiniChannel *ch) {
        if (ch->index() == kNew || ch->index() == kDeleted) {
            if (ch->index() == kNew) channels_[ch->fd()] = ch;
            ch->set_index(kAdded);
            update(EPOLL_CTL_ADD, ch);
        } else {
            if (ch->events() == 0) {
                update(EPOLL_CTL_DEL, ch);
                ch->set_index(kDeleted);
            } else {
                update(EPOLL_CTL_MOD, ch);
            }
        }
    }
    // ↔ EPollPoller::removeChannel（EPollPoller.cc:97-110）
    void removeChannel(MiniChannel *ch) {
        channels_.erase(ch->fd());
        if (ch->index() == kAdded) update(EPOLL_CTL_DEL, ch);
        ch->set_index(kNew);
    }

private:
    void update(int op, MiniChannel *ch) {           // ↔ EPollPoller::update（EPollPoller.cc:124-147）
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof ev);
        ev.events = ch->events();
        ev.data.ptr = ch;   // ★ 把 Channel* 塞进 data.ptr，事件回来时"认领"就靠它
        if (::epoll_ctl(epollfd_, op, ch->fd(), &ev) < 0)
            perror("epoll_ctl");
    }

    MiniLoop *loop_;
    int epollfd_;
    std::vector<struct epoll_event> events_;
    std::unordered_map<int, MiniChannel *> channels_;   // fd → Channel（↔ muduo 同名成员）
};

/* ================= MiniLoop（↔ EventLoop.cc 全文） ================= */
// ↔ EventLoop.cc:13 "防止一个线程创建多个 EventLoop"
__thread MiniLoop *t_loopInThisThread = nullptr;

class MiniLoop {
public:
    using Functor = std::function<void()>;

    MiniLoop()
        : threadId_(static_cast<int>(::syscall(SYS_gettid)))
        , poller_(this)
        , wakeupFd_(createEventfd())               // ↔ EventLoop.cc:50
        , wakeupChannel_(this, wakeupFd_)          // ↔ EventLoop.cc:51
    {
        if (t_loopInThisThread) {
            fprintf(stderr, "FATAL: Another MiniLoop already exists in this thread %d\n", threadId_);
            std::abort();
        }
        t_loopInThisThread = this;
        wakeupChannel_.setReadCallback([this] { handleRead(); });
        wakeupChannel_.enableReading();            // ↔ EventLoop.cc:66：每个 loop 都监听自己的 wakeupFd
    }

    // ↔ EventLoop::loop()（EventLoop.cc:77-103）：muduo 的心脏，就 10 行
    void loop() {
        looping_ = true;
        quit_ = false;
        printf("[loop] 主线程 eventLoop 启动 tid=%d（epoll_wait 超时 5s）\n", threadId_);
        while (!quit_) {
            auto activeChannels = poller_.poll(5000);       // ① 阻塞等事件
            for (MiniChannel *ch : activeChannels)
                ch->handleEvent();                          // ② 分发（谁的事件调谁的回调）
            doPendingFunctors();                            // ③ 处理跨线程排进来的任务
        }
        looping_ = false;
        printf("[loop] 退出事件循环\n");
    }

    // ↔ EventLoop::quit（EventLoop.cc:115-123）：别的线程调它，必须唤醒
    void quit() {
        quit_ = true;
        if (!isInLoopThread()) wakeup();
    }

    // ↔ EventLoop::runInLoop / queueInLoop（EventLoop.cc:126-155）
    void runInLoop(Functor cb) {
        if (isInLoopThread()) cb();
        else queueInLoop(std::move(cb));
    }
    void queueInLoop(Functor cb) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingFunctors_.emplace_back(std::move(cb));
        }
        // 不是本线程排的，或 loop 正忙着执行上一批回调 → 必须再戳它一下
        if (!isInLoopThread() || callingPendingFunctors_)
            wakeup();
    }

    void updateChannel(MiniChannel *ch) { poller_.updateChannel(ch); }
    void removeChannel(MiniChannel *ch) { poller_.removeChannel(ch); }
    bool isInLoopThread() const { return threadId_ == static_cast<int>(::syscall(SYS_gettid)); }

private:
    static int createEventfd() {                    // ↔ EventLoop.cc:34-42
        int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) { perror("eventfd"); std::abort(); }
        return fd;
    }
    void handleRead() {                             // ↔ EventLoop::handleRead（:157）
        uint64_t one = 1;
        ssize_t n = ::read(wakeupFd_, &one, sizeof one);
        (void)n;                                    // 读 8 字节 = 把"电平"拉下去（实验 02 的知识！）
    }
    void wakeup() {                                 // ↔ EventLoop::wakeup（:168）
        uint64_t one = 1;
        ssize_t n = ::write(wakeupFd_, &one, sizeof one);
        (void)n;
    }
    void doPendingFunctors() {                      // ↔ EventLoop.cc:194-211
        std::vector<Functor> functors;
        callingPendingFunctors_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            functors.swap(pendingFunctors_);        // swap 缩小锁的临界区 + 避免回调里再 queueInLoop 死锁
        }
        for (const Functor &f : functors) f();
        callingPendingFunctors_ = false;
    }

    bool looping_ = false;
    std::atomic<bool> quit_{false};
    std::atomic<bool> callingPendingFunctors_{false};
    int threadId_;
    MiniPoller poller_;
    int wakeupFd_;
    MiniChannel wakeupChannel_;
    std::vector<Functor> pendingFunctors_;
    std::mutex mutex_;
};

/* MiniChannel 成员函数定义放在 MiniLoop 之后（需要调用 loop 的完整接口） */
void MiniChannel::enableReading() { events_ |= EPOLLIN; loop_->updateChannel(this); }
void MiniChannel::remove()        { loop_->removeChannel(this); }
void MiniChannel::handleEvent() {
    if (revents_ & EPOLLIN) {
        if (readCallback_) readCallback_();
    }
    // 真实 Channel 还要分 EPOLLOUT/EPOLLERR/EPOLLHUP，见 Channel.cc
}

static void setNonblock(int fd) {
    int fl = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ============================ 主程序 ============================ */
int main() {
    std::signal(SIGPIPE, SIG_IGN);

    /* 1. 监听 socket：bind 端口 0 让内核挑空闲端口，避免冲突 */
    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    setNonblock(listenFd);
    int on = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listenFd, (sockaddr *)&addr, sizeof addr);
    socklen_t alen = sizeof addr;
    ::getsockname(listenFd, (sockaddr *)&addr, &alen);
    ::listen(listenFd, 128);
    printf("[main] echo 服务: 127.0.0.1:%d (listenFd=%d)\n",
           ntohs(addr.sin_port), listenFd);

    /* 2. 事件循环 + 监听 Channel（↔ muduo: TcpServer 里也有一个 Acceptor 的 Channel） */
    MiniLoop loop;                                            // 主线程独占 —— one loop per thread
    std::unordered_map<int, std::shared_ptr<MiniChannel>> conns;  // ↔ TcpServer::connections_

    MiniChannel listenChannel(&loop, listenFd);
    listenChannel.setReadCallback([&] {
        // 监听 fd 上的 EPOLLIN = 有新的 TCP 连接完成三次握手（↔ Acceptor::handleRead）
        while (true) {
            int cfd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK);
            if (cfd < 0) break;                              // EAGAIN： pending 连接收完了
            printf("[loop] 新连接 fd=%d → 建 Channel 并 enableReading(epoll_ctl ADD)\n", cfd);
            auto conn = std::make_shared<MiniChannel>(&loop, cfd);
            conn->setReadCallback([conn, &conns] {
                char buf[128];
                ssize_t n = ::read(conn->fd(), buf, sizeof buf);
                if (n > 0) {
                    printf("[loop] fd=%d EPOLLIN 读到 %zd 字节: %.*s",
                           conn->fd(), n, (int)n, buf);
                    ssize_t off = 0;
                    while (off < n) {
                        ssize_t w = ::write(conn->fd(), buf + off, n - off);
                        if (w <= 0) break;
                        off += w;
                    }
                } else if (n == 0) {                         // 对端 FIN
                    printf("[loop] fd=%d 对端关闭 → removeChannel(epoll_ctl DEL)\n", conn->fd());
                    conn->remove();
                    ::close(conn->fd());
                    conns.erase(conn->fd());                 // shared_ptr 还活在回调里，安全
                }
            });
            conns[cfd] = conn;
            conn->enableReading();
        }
    });
    listenChannel.enableReading();

    /* 3. 客户端线程：走一遍完整链路 + 压轴测 wakeup 延迟 */
    std::thread client([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 等主线程进入 epoll_wait
        int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
        ::connect(cfd, (sockaddr *)&addr, sizeof addr);

        char msg[64];
        for (int i = 0; i < 3; i++) {
            int len = std::snprintf(msg, sizeof msg, "hello mini-muduo #%d\n", i);
            ::send(cfd, msg, len, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        int nl = 0, total = 0;
        while (nl < 3) {
            ssize_t r = ::recv(cfd, msg, sizeof msg, 0);
            if (r <= 0) break;
            total += (int)r;
            for (ssize_t i = 0; i < r; i++) if (msg[i] == '\n') nl++;
        }
        printf("[client] 收到 echo 数据共 %d 字节 —— 完整链路: 内核→epoll_wait→Channel→回调→write\n", total);

        /* ★ 压轴：主线程此刻正阻塞在 5 秒超时的 epoll_wait。
         *   本线程 queueInLoop 排一个回调，靠 write(eventfd) 让 epoll_wait 立刻返回。
         *   实测延迟是微秒级 —— 没有 wakeupFd_，这个回调要等 5 秒才能执行。 */
        auto t0 = std::make_shared<std::chrono::steady_clock::time_point>(
                      std::chrono::steady_clock::now());
        loop.queueInLoop([&loop, t0] {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - *t0).count();
            printf("[loop] 跨线程回调执行，从入队到这里只花了 %lld us ——"
                   " 就是 eventfd write 把阻塞的 epoll_wait 拽回来的（EventLoop.cc:168）\n",
                   (long long)us);
            loop.quit();     // 本回调在 loop 线程里执行 → quit 无需 wakeup（EventLoop.cc:119）
        });
        ::close(cfd);
    });

    loop.loop();          // 阻塞运行，直到 quit_ 生效
    client.join();
    ::close(listenFd);
    printf("[main] 演示结束。对照阅读 muduo-core/src/{EventLoop,EPollPoller,Channel}.cc\n");
    return 0;
}
