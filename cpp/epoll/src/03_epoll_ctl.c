/*
 * 实验 03：epoll_ctl 的 ADD/MOD/DEL 状态流转 + EPOLLONESHOT
 *
 * 对应 muduo：
 *   EPollPoller.cc 顶部的三个状态常量：
 *       kNew(-1) 未注册   kAdded(1) 已注册   kDeleted(2) 已删除
 *   updateChannel()（EPollPoller.cc:63-94）就是一台把"Channel 状态"
 *   翻译成"epoll_ctl 调用"的状态机。Channel::enableReading/remove
 *   走的正是这个路径。
 *
 * EPOLLONESHOT 对应一个经典面试题："muduo 为什么不处理 EPOLLONESHOT？"
 *   —— 因为 muduo 用 one loop per thread：每个 fd 只归属一个 loop 线程，
 *      根本不存在两个线程抢同一个 fd 的场景，不需要 ONESHOT 保护。
 *      多线程共享一个 epollfd 的框架（很多 Go 式 worker-pool 思路）才需要它。
 *
 * 编译: gcc -std=gnu11 -O2 -o 03_epoll_ctl src/03_epoll_ctl.c
 * 运行: ./bin/03_epoll_ctl
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>

static void setNonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 跑一轮 epoll_wait，把结果翻译成人话 */
static void waitAndShow(int epfd, int timeoutMs, const char *scene)
{
    struct epoll_event rev;
    memset(&rev, 0, sizeof rev);
    int n = epoll_wait(epfd, &rev, 1, timeoutMs);
    printf("  %-38s => ", scene);
    if (n == 0)     printf("无事件（100ms 超时）\n");
    else if (n < 0) printf("错误: %s\n", strerror(errno));
    else            printf("%s%s%s\n",
                           (rev.events & EPOLLIN)  ? "EPOLLIN "  : "",
                           (rev.events & EPOLLOUT) ? "EPOLLOUT " : "",
                           (rev.events & EPOLLERR) ? "EPOLLERR"  : "");
}

int main(void)
{
    char buf[16];
    ssize_t r;
    struct epoll_event ev;

    /* ================= Part A：ADD / MOD / DEL ================= */
    printf("== A. ADD / MOD / DEL（对应 EPollPoller.cc 的 kNew/kAdded/kDeleted 状态机）==\n");
    /* 注意用 socketpair 而不是 pipe：pipe 的读端永远不可写，演示不了 EPOLLOUT；
     * socket 两端都可读可写，缓冲区有空间时 EPOLLOUT 就是"高电平" */
    int p[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) < 0) { perror("socketpair"); return 1; }
    setNonblock(p[0]);
    int epfd = epoll_create1(0);

    /* ADD：相当于 Channel::enableReading() —— index_: kNew → kAdded */
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);
    r = write(p[1], "x", 1); (void)r;
    waitAndShow(epfd, 100, "ADD 后写 1 字节");
    (void)!read(p[0], buf, 1);                  /* 读干净 */
    waitAndShow(epfd, 100, "LT 下数据读干净后再问一次");   /* 没事件：电平降下去了 */

    /* MOD：改感兴趣事件。socket 发送缓冲区有空间 → EPOLLOUT 立即成为高电平 */
    ev.events = EPOLLIN | EPOLLOUT;
    epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev);
    waitAndShow(epfd, 100, "MOD 加上 EPOLLOUT（没写任何数据）");

    /* DEL：相当于 Channel::remove() —— epoll 从此"看不见"这个 fd */
    epoll_ctl(epfd, EPOLL_CTL_DEL, p[0], NULL);
    r = write(p[1], "z", 1); (void)r;
    waitAndShow(epfd, 100, "DEL 后写 1 字节（数据仍在缓冲区）"); /* 报不了：压根没注册 */

    /* 重新 ADD：LT 会把"旧的没读的数据"再报一次 */
    ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev);
    waitAndShow(epfd, 100, "重新 ADD");                          /* EPOLLIN 立刻回来 */
    (void)!read(p[0], buf, 1);
    close(epfd); close(p[0]); close(p[1]);

    /* ================= Part B：EPOLLONESHOT ================= */
    printf("\n== B. EPOLLONESHOT：事件发生一次后该 fd 自动'熄火' ==\n");
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) { perror("socketpair"); return 1; }
    setNonblock(sp[0]);
    int ep2 = epoll_create1(0);

    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = sp[0];
    epoll_ctl(ep2, EPOLL_CTL_ADD, sp[0], &ev);

    r = write(sp[1], "A", 1); (void)r;
    waitAndShow(ep2, 100, "第 1 次写入");                         /* 报 */
    (void)!read(sp[0], buf, 1);

    r = write(sp[1], "B", 1); (void)r;
    waitAndShow(ep2, 100, "第 2 次写入（处理完后 ONESHOT 自动禁用）"); /* 不报！ */

    /* 手动 re-arm —— 多线程框架里"处理完一个事件必须重新注册"的由来 */
    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep2, EPOLL_CTL_MOD, sp[0], &ev);
    r = write(sp[1], "C", 1); (void)r;
    waitAndShow(ep2, 100, "MOD 重新武装后第 3 次写入");            /* 又报 */

    printf("\n结论：\n"
           "  1) ADD/MOD/DEL 三态对应 muduo Channel 的 kNew/kAdded/kDeleted（EPollPoller.cc:68-92）；\n"
           "     disableAll() → isNoneEvent() → EPOLL_CTL_DEL → kDeleted；再 enableReading() 时又 ADD 回去；\n"
           "  2) DEL 之后 fd 上有数据也不会报 —— epoll 看的是'注册关系+就绪列表'，不是 fd 本身；\n"
           "  3) EPOLLONESHOT 是为'多线程抢同一个 epollfd'准备的防重入开关；\n"
           "     muduo 不需要它：一个 fd 永远只被它的 loop 线程碰 —— 面试答案就是这个。\n");
    close(ep2); close(sp[0]); close(sp[1]);
    return 0;
}
