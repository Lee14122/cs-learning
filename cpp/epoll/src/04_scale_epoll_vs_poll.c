/*
 * 实验 04：规模实验 —— 5 万连接在线，poll 和 epoll 处理一个事件谁贵？
 *
 * 原理预告（跑完自己验证）：
 *   select：O(N) + 每轮重建 fd_set + 全量拷回 + FD_SETSIZE=1024 上限；
 *   poll  ：没有 1024 上限，但内核每轮仍要拷贝并扫描全部 N 个 pollfd —— 依旧 O(N)；
 *   epoll ：注册一次终身有效（红黑树），数据到达时内核把它挂上"就绪链表"，
 *           epoll_wait 只从就绪链表收割 —— O(发生的事件数)，与 N 无关。
 *
 * 这就是"为什么后端都要 epoll"，也是面试必考的 select/poll/epoll 区别 ——
 * 别背八股，跑一遍这个程序，数字会替你说话。
 *
 * 对应 muduo：Poller::newDefaultPoller 优先选 epoll；
 *            EPollPoller::poll 返回的 activeChannels 只有"发生了事件的 Channel"。
 *
 * 编译: gcc -std=gnu11 -O2 -o 04_scale_epoll_vs_poll src/04_scale_epoll_vs_poll.c
 * 运行: ./bin/04_scale_epoll_vs_poll   （约 2~5 秒）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/select.h>   /* FD_SETSIZE */
#include <poll.h>

static long nowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

int main(void)
{
    /* 1. 把 fd 软上限提到能提的最大（先 ulimit -Hn 看看硬上限） */
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = (rl.rlim_max > 131072) ? 131072 : rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) perror("setrlimit");
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    printf("fd 软上限 = %llu\n", (unsigned long long)rl.rlim_cur);

    /* 2. 创建一批"空闲连接"：一个 socketpair 算一条连接。
     *    一半(fd reg)注册进多路复用但从不读它数据之外的东西，另一半(peer)只用来"发数据" */
    int N = 50000;
    int maxByFd = (int)((rl.rlim_cur > 200 ? rl.rlim_cur - 200 : 0) / 2);
    if (N > maxByFd) N = maxByFd;
    if (N < 1000)  { printf("fd 上限太低，实验做不了（ulimit -n）\n"); return 1; }

    int *reg  = malloc(sizeof(int) * N);
    int *peer = malloc(sizeof(int) * N);
    for (int i = 0; i < N; i++) {
        int sp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) { N = i; break; }
        reg[i] = sp[0];
        peer[i] = sp[1];
    }
    printf("创建 %d 条空闲连接（共 %d 个 fd），它们安静地待着，只等'个别'被写入\n\n", N, N * 2);

    /* 3. 给 poll 准备全量数组；给 epoll 逐个注册（注册成本只付一次！） */
    struct pollfd *pfds = calloc(N, sizeof(struct pollfd));
    for (int i = 0; i < N; i++) { pfds[i].fd = reg[i]; pfds[i].events = POLLIN; }

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    for (int i = 0; i < N; i++) {
        ev.data.u32 = (unsigned)i;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, reg[i], &ev) < 0) { perror("epoll_ctl"); return 1; }
    }

    /* 4. 模拟真实服务器：每个"请求事件"= 往某条连接的 peer 端写 1 字节，
     *    然后看 poll / epoll 各自多久能把这个事件找出来并处理掉 */
    const int ROUNDS = 200;
    long tPoll = 0, tEpoll = 0;
    struct epoll_event rev;
    char b = 'x';
    char tmp[8];

    for (int r = 0; r < ROUNDS; r++) {
        /* ---------- poll：系统调用 O(N) + 用户态遍历 O(N) ---------- */
        int idx = (int)(((unsigned long)r * 2654435761UL) % (unsigned)N);
        ssize_t w = write(peer[idx], &b, 1); (void)w;

        long t0 = nowNs();
        poll(pfds, N, 0);                       /* 内核拷贝 N 个 pollfd 并全部扫一遍 */
        for (int i = 0; i < N; i++) {           /* 用户态还得再遍历一遍才知道是谁 ready */
            if (pfds[i].revents & POLLIN) {
                (void)!read(reg[i], tmp, sizeof tmp);  /* 处理掉这个事件 */
                break;
            }
        }
        tPoll += nowNs() - t0;

        /* ---------- epoll：内核就绪链表里只有那 1 个 ---------- */
        idx = (int)(((unsigned long)(r + 97) * 2654435761UL) % (unsigned)N);
        w = write(peer[idx], &b, 1); (void)w;

        t0 = nowNs();
        int n = epoll_wait(epfd, &rev, 1, 0);   /* 直接收割就绪的那个 */
        if (n > 0)
            (void)!read(reg[rev.data.u32], tmp, sizeof tmp);
        tEpoll += nowNs() - t0;
    }

    printf("模拟 %d 个事件，统计'处理一个事件'的平均耗时：\n", ROUNDS);
    printf("  poll : %10.1f us/事件  （每次都要让内核+自己遍历 %d 个 fd）\n",
           tPoll / ROUNDS / 1000.0, N);
    printf("  epoll: %10.1f us/事件  （只返回发生的那 1 个，与 %d 个 fd 无关）\n",
           tEpoll / ROUNDS / 1000.0, N);
    printf("  差距 : poll 比 epoll 慢约 %.0f 倍\n\n", (double)tPoll / (double)tEpoll);

    printf("顺便记住两个知识点：\n"
           "  1) FD_SETSIZE = %d —— select 的位图只认编号小于它的 fd，几万人在线直接判死刑；\n"
           "  2) epoll 的 O(1) 来自内核数据结构：兴趣列表(红黑树) + 就绪列表(链表)。\n"
           "     fd 上的数据到达 → 该 fd 的回调把它挂上就绪链表 → epoll_wait 只收割就绪链表。\n"
           "     muduo/Redis/Nginx 敢挂十万连接，底气就在这两行字。\n", FD_SETSIZE);

    for (int i = 0; i < N; i++) { close(reg[i]); close(peer[i]); }
    free(reg); free(peer); free(pfds); close(epfd);
    return 0;
}
