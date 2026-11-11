/*
 * 实验 02：水平触发(LT) vs 边缘触发(ET) —— 一次"故意读不完"的实验
 *
 * 原理一句话：
 *   LT：只要缓冲区里还有数据，每轮 epoll_wait 都会提醒你 —— "电平"还在；
 *   ET：只在"状态翻转"（空→非空）的那一次提醒，之后你不读完，它不再吭声。
 *
 * 这个区别决定了上层代码的写法：
 *   LT 允许"一次事件读一次"，读不完下轮还会再报（不容易丢数据、代码简单）；
 *   ET 强制"一次事件必须循环读到 EAGAIN"，否则会卡死（唤醒次数少、适合超高性能场景）。
 *
 * 对应 muduo：
 *   Buffer.cc 的 readFd 上方注释写着 "Poller工作在LT模式"。
 *   muduo 每次收到 EPOLLIN 只 readv 一次 —— 在 LT 下是安全的；
 *   如果换成 ET，这一行不改代码就会丢数据。
 *
 * 编译: gcc -std=gnu11 -O2 -o 02_lt_vs_et src/02_lt_vs_et.c
 * 运行: ./bin/02_lt_vs_et
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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

/* 场景：一次性发来 15 字节（模拟 3 条粘在一起的 TCP 消息），
 * 我们故意每轮只读 4 字节，数一数 epoll_wait 提醒几次。 */
static void run(int useET)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) { perror("socketpair"); exit(1); }
    setNonblock(sp[0]);

    int epfd = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | (useET ? EPOLLET : 0);   /* ← 本实验唯一的区别 */
    ev.data.fd = sp[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, sp[0], &ev);

    ssize_t t;
    t = write(sp[1], "AAAAA", 5);
    t = write(sp[1], "BBBBB", 5);
    t = write(sp[1], "CCCCC", 5);                  /* 共 15 字节 */
    (void)t;

    int wakes = 0, got = 0;
    for (;;) {
        struct epoll_event rev;
        int n = epoll_wait(epfd, &rev, 1, 100);
        if (n <= 0) break;              /* 100ms 超时：epoll 认为"现在没有事件" */
        wakes++;
        char buf[8];
        int r = (int)read(sp[0], buf, 4);   /* 故意每次只读 4 字节 */
        if (r > 0) got += r;
        if (got >= 15) break;
    }
    int left = 15 - got;
    printf("%-16s => epoll_wait 共提醒 %d 次；停止提醒时缓冲区还剩 %d 字节\n",
           useET ? "[ET 边缘触发]" : "[LT 水平触发]", wakes, left);

    if (useET && left > 0) {
        /* 抢救 ET：再来一个新数据 → 制造新"边沿" → 通知才恢复 */
        ssize_t w2 = write(sp[1], "D", 1);
        (void)w2;
        struct epoll_event rev;
        int n = epoll_wait(epfd, &rev, 1, 100);
        printf("  ↳ 再写 1 个字节，epoll_wait %s —— 新数据制造了新边沿\n",
               n > 0 ? "又醒了(这就是ET的'边沿'语义)" : "还是没醒");
        char drain[64];
        int total = 0;
        for (;;) {                       /* ET 标准写法：循环读到 EAGAIN */
            int r = (int)read(sp[0], drain, sizeof drain);
            if (r <= 0) break;
            total += r;
        }
        printf("  ↳ 这次一次读到 %d 字节，直到 EAGAIN 才停\n", total);
    }
    close(sp[0]); close(sp[1]); close(epfd);
}

int main(void)
{
    printf("== 场景：发来 15 字节，每次只读 4 字节 ==\n");
    run(0);   /* LT */
    run(1);   /* ET */
    printf(
"\n结论：\n"
"  1) LT：数据没读完就一直提醒 —— 不会'丢事件'，所以 muduo 一次事件只 read 一次也安全；\n"
"  2) ET：只在'空→非空'翻转时提醒一次。还剩 11 字节它就不吭声了 —— 必须读到 EAGAIN；\n"
"  3) 为什么 muduo 选 LT？看 Buffer.cc:readFd：一次 readv + append 完事，代码简单；\n"
"     残留数据靠下一轮 epoll_wait 的 LT 再报，天然续上了；\n"
"  4) 对你的 RpcCodec::onData(Buffer*)：LT 之下，一次 onData 可能拆不出完整帧，\n"
"     下一轮 onData 还会再被调用 —— 所以 pendingBuffer_ 的'缓存半截帧'逻辑必须可重入。\n");
    return 0;
}
