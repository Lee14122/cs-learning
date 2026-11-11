/*
 * 实验 01：epoll 三件套 —— 偏偏不用 socket
 *
 * 常见误解："epoll 是用来监听网络套接字的"。
 * 真相：epoll 管理的是"内核就绪列表"，任何能用 fd 表达"可读"的内核对象
 *       都可以被 epoll 等待 —— timerfd(定时器)、eventfd(线程信令)、
 *       signalfd(信号)、inotify(文件变更)，socket 只是其中之一。
 *
 * muduo 的 wakeupFd_ 就是 eventfd（EventLoop.cc:34-42），不是 socket。
 * 看懂这一点，你才算真正理解"epoll 等待的是什么"。
 *
 * 观察重点：
 *   1) epoll_create1 → epoll_ctl(ADD) → epoll_wait 三步；
 *   2) epoll_wait 每次只返回"真正发生了事件"的 fd，而不是全部注册列表；
 *   3) ev.data.ptr 原样往返 —— 这就是 EPollPoller.cc:134 那行代码的全部秘密；
 *   4) 读出 timerfd 的"超时次数"，感受它作为定时器的用法（你的 RPC 超时/心跳要用它）。
 *
 * 编译: gcc -std=gnu11 -O2 -o 01_epoll_basics src/01_epoll_basics.c
 * 运行: ./bin/01_epoll_basics   （约 0.6 秒自动结束）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

/* epoll 帮你"捎带"的上下文对象 —— muduo 用的是 Channel，这里用 Tagged */
typedef struct { const char *name; int fd; } Tagged;

int main(void)
{
    /* ---- 1. 两个"非 socket"的内核对象 ---- */

    /* timerfd：每到期一次，内核就往这个 fd 写一个 8 字节计数（到期周期数） */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_interval.tv_nsec = 200 * 1000000L;  /* 之后每 200ms 到期一次 */
    its.it_value.tv_nsec    = 100 * 1000000L;  /* 第一次在 100ms 后 */
    timerfd_settime(tfd, 0, &its, NULL);

    /* eventfd：一个 64 位计数器，write() 之后它就"可读" */
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t v = 42;
    ssize_t w = write(efd, &v, sizeof v);
    (void)w;

    Tagged timerObj = { "timerfd", tfd };
    Tagged eventObj = { "eventfd", efd };

    /* ---- 2. 注册 ---- */
    /* 对应 muduo：EPollPoller 构造里 ::epoll_create1(EPOLL_CLOEXEC) (EPollPoller.cc:15)
     *            update(EPOLL_CTL_ADD, channel)                (EPollPoller.cc:136) */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.ptr = &timerObj;                    /* 关键：把"我自己的指针"交给内核 */
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.ptr = &eventObj;
    epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev);

    /* ---- 3. 等待事件 ---- */
    /* 对应 muduo：EPollPoller::poll() 里的 ::epoll_wait (EPollPoller.cc:34) */
    printf("注册了 2 个 fd（timerfd + eventfd），开始 epoll_wait：\n");
    struct epoll_event revs[4];
    for (int round = 1; round <= 4; round++) {
        int n = epoll_wait(epfd, revs, 4, 1000);
        if (n < 0) { perror("epoll_wait"); return 1; }
        printf("  第 %d 次 epoll_wait 返回 %d 个事件\n", round, n);
        for (int k = 0; k < n; k++) {
            /* 注册时塞进去的指针，原封不动地回来了 —— 不用查表就知道是谁的事件 */
            Tagged *obj = (Tagged *)revs[k].data.ptr;
            uint64_t val = 0;
            ssize_t r = read(obj->fd, &val, sizeof val);
            (void)r;
            if (obj->fd == tfd)
                printf("    [%s] 到期（内核累计到期次数 = %llu）\n",
                       obj->name, (unsigned long long)val);
            else
                printf("    [%s] 可读（计数器值 = %llu，read 后清零）\n",
                       obj->name, (unsigned long long)val);
        }
    }

    printf("\n结论：\n"
           "  1) epoll 不关心 fd 是不是 socket，它等的是'内核就绪列表'——timerfd/eventfd 同样能等；\n"
           "  2) epoll_wait 只返回'发生了的'事件，不是把你注册的 fd 全扫一遍（对比 select/poll）；\n"
           "  3) ev.data.ptr 往返：muduo 把 Channel* 塞进去（EPollPoller.cc:134），\n"
           "     事件回来时再 static_cast 拿出来（EPollPoller.cc:117），零查表分发；\n"
           "  4) timerfd 是实现定时器的标准积木 —— muduo-core 精简版没带 TimerQueue，\n"
           "     你的 RPC 超时/心跳就得靠它 + eventfd 这套组合自己搭。\n");
    return 0;
}
