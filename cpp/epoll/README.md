# epoll 五连实验（配合 muduo-core 源码食用）

> 目标：把 epoll 从"背八股"变成"亲手摸过"。
> 每个实验都是一个独立小程序，几秒跑完，输出会直接告诉你原理。
> 实验里标注了 muduo-core 源码的具体文件和行号，跑完实验立刻去读对应源码，效果翻倍。

## 快速开始

```bash
cd ~/projects/cs-learning/cpp/epoll
bash build.sh          # 编译全部 5 个实验到 bin/
bash run_all.sh        # 依次运行（约 10 秒）
```

单独运行：`./bin/01_epoll_basics` 等。想观察系统调用：`strace -c ./bin/01_epoll_basics`。

## 学习路线（建议按序做，每个 30 分钟内）

| # | 实验 | 回答的问题 | 对应 muduo-core 源码 |
|---|------|-----------|---------------------|
| 01 | `01_epoll_basics.c` | epoll 三件套怎么用？epoll 等的到底是什么？ | `EPollPoller.cc:15,34,134`、`EventLoop.cc:34`（eventfd） |
| 02 | `02_lt_vs_et.c` | LT/ET 差在哪？为什么 muduo 敢"一次事件只读一次"？ | `Buffer.cc`（readFd 注释"Poller工作在LT模式"） |
| 03 | `03_epoll_ctl.c` | ADD/MOD/DEL 什么语义？EPOLLONESHOT 干嘛的？muduo 为什么不用它？ | `EPollPoller.cc:9-11,63-110`（kNew/kAdded/kDeleted 状态机） |
| 04 | `04_scale_epoll_vs_poll.c` | 为什么 select/poll 撑不起高并发，epoll 凭什么 O(1)？ | `DefaultPoller.cc`、`EPollPoller::poll` |
| 05 | `05_mini_muduo.cpp` | 把前四个零件装起来：一个 ~300 行的迷你 muduo（Echo 服务器 + 跨线程唤醒实测） | `EventLoop.cc` 全文、`Channel.cc`、`EPollPoller.cc` |

## 每个实验的"预期现象"（先猜再跑）

### 01 epoll 三件套（不用 socket！）

- 注册 timerfd（每 200ms 到期）+ eventfd（预先写过一次）。
- **先猜**：第一次 `epoll_wait` 会返回几个事件？
- 实际：第 1 轮只有 eventfd（timer 还没到期），第 2~4 轮每轮一个 timerfd 事件。
- 要点：epoll 管理的是"内核就绪列表"，**不关心 fd 是不是 socket**；`data.ptr` 原样往返（muduo 塞的是 `Channel*`）。
- 伏笔：muduo-core 精简版**没有 TimerQueue**，你的 RPC 框架要做超时/心跳，标准解法就是这里的 timerfd。

### 02 LT vs ET：故意"读不完"

- 一次性发 15 字节，每轮只读 4 字节，数 `epoll_wait` 提醒几次。
- **先猜**：LT 提醒几次？ET 呢？
- 实际：LT 提醒 4 次直到读干净（4+4+4+3）；**ET 只提醒 1 次**，剩下 11 字节它装死，直到你再写 1 字节制造"新边沿"。
- 要点：muduo 用 LT，所以 `Buffer::readFd` 一次 `readv` 就够了——读不完下一轮 LT 还会再报；ET 则必须循环读到 `EAGAIN`，否则丢事件。
- 对你的 RPC 框架：`RpcCodec::onData()` 在 LT 下会被反复调用，**半截帧的缓存逻辑必须可重入**。

### 03 epoll_ctl 状态机 + EPOLLONESHOT

- Part A：`ADD` 后写 1 字节 → 报 `EPOLLIN`；读干净再问 → 不报（电平下去了）；`MOD` 加 `EPOLLOUT` → 立即报（管道永远可写）；`DEL` 后再写 → **装死**；重新 `ADD` → 旧的没读数据立刻又报出来。
- Part B：`EPOLLIN|EPOLLONESHOT` 注册，第 1 次写入 → 报；处理完第 2 次写入 → **不报**（自动熄火）；`MOD` 重新武装 → 又报。
- 要点：这三态流转就是 `EPollPoller.cc` 里 `kNew/kAdded/kDeleted` 状态机的实物版；`Channel::enableReading/disableAll/remove` 全走这条路。
- 面试题"muduo 为什么不用 EPOLLONESHOT"：**one loop per thread**，每个 fd 只归属一个 loop 线程，不存在多线程抢同一 fd；ONESHOT 是给"多线程共享一个 epollfd"的模型准备的。

### 04 规模实验：5 万连接，处理一个事件谁贵？

- 5 万条空闲连接，每轮往其中 1 条写 1 字节，分别测 poll 和 epoll 找出并处理这个事件的耗时。
- **先猜**：差多少倍？
- 实际（本机实测：poll 14.8ms/事件 vs epoll 5µs/事件，**差约 2800 倍**；机器不同数字不同，量级一致）：poll 每事件要付 O(N) 的扫描成本（内核拷贝+扫描 5 万个 pollfd，用户态再遍历一遍），epoll 每事件只收割就绪链表。**连接数越大差得越多**。
- 要点：poll 的成本 = O(N) 与"有没有事件"无关；epoll 的成本 = O(发生的事件数)。epoll 的 O(1) 来自内核的**兴趣列表（红黑树）+ 就绪链表**：数据到达 → fd 的回调把自己挂上就绪链表 → `epoll_wait` 只收割就绪链表。
- 顺带：`FD_SETSIZE=1024`，select 连参赛资格都没有。

### 05 迷你 muduo：全部零件组装

- 用 ~300 行 C++17 复刻 `MiniChannel / MiniPoller / MiniLoop`，跑一个真 Echo 服务器，自带客户端线程自测。
- 你会依次看到：
  1. `[loop] 新连接 fd=.. → 建 Channel 并 enableReading`（accept 路径 ↔ Acceptor）
  2. `[loop] fd=.. EPOLLIN 读到 .. 字节`（IO 路径 ↔ TcpConnection::handleRead）
  3. `[client] 收到 echo 数据共 .. 字节`（完整回路）
  4. **压轴**：`[loop] 跨线程回调执行，从入队到这里只花了 N us` —— 主线程正阻塞在 5 秒超时的 `epoll_wait` 里，客户端线程 `queueInLoop()` 之后回调**微秒级**就被执行了。靠的是 `write(wakeupFd_)` 这 8 个字节（↔ `EventLoop.cc:168` 的 `wakeup()`）。
- 读完这个文件再读 `muduo-core/src/EventLoop.cc`，你会发现结构一模一样，只是多了日志和更多事件类型。

## 思考题（答案要点见文末）

1. `epoll_wait` 返回的事件里，`data.ptr` 为什么能原样带回？内核存了什么？
2. LT 模式下，`Buffer::readFd` 一次没读完会怎样？为什么这反而是"安全"的？
3. ET 模式下不循环读到 `EAGAIN`，最坏会发生什么？（提示：想想 02 里那 11 个字节的下场）
4. `EPOLL_CTL_DEL` 之后 fd 上还有数据，为什么 `epoll_wait` 不报？说明 epoll 就绪的判定依据是什么？
5. 为什么 muduo 的每个 EventLoop 都要额外监听一个 eventfd，而不是用 `pthread_cond` 等待任务队列？
6. 你的 RPC 客户端 `syncCall` = "asyncCall + 条件变量等待"。如果调用线程和 IO 线程是同一个（在 loop 线程里调 syncCall），会发生什么？怎么防？
7. 实验 04 里 epoll 的注册（5 万次 `epoll_ctl`）也花了时间，为什么说它仍然比 poll 划算？

## 答案要点

1. `epoll_ctl(ADD)` 时内核把 `epoll_event`（含 data）连同 fd 一起挂进兴趣列表（红黑树）节点；事件就绪时内核把该节点链入就绪链表，`epoll_wait` 只是把就绪链表上的 data 拷贝回用户态。所以 data 是内核替你保管的"随身行李"。
2. 剩余数据让 fd 保持"可读电平"，下一轮 `epoll_wait` 会再次返回该 fd——不会丢，只是延迟到下一轮处理。这就是 muduo 一次 readv + LT 的底气。
3. 数据滞留缓冲区，fd 再也不产生"新边沿"，这条连接就永久卡死（直到下次对端再发数据才可能复活）；服务端表现为"客户端发了请求永远等不到响应"。
4. epoll 的判定依据是"该 fd 在兴趣列表里 + 内核就绪链表里有它"。DEL 把它从兴趣列表摘除，即使缓冲区有数据也不再进入就绪链表。就绪是"注册关系"的属性，不是 fd 本身的属性。
5. `pthread_cond_wait` 会把线程挂起，无法同时等 fd 事件；而 epoll_wait 本身就是一个"等待点"，eventfd 把"有新任务"也变成一个 fd 事件，统一进同一个等待点——**一个 epoll_wait 同时等 IO 和跨线程任务**，这是 Reactor 模型能自洽的关键。
6. 死锁/永久阻塞：syncCall 在 loop 线程里等响应，而解析响应也需要这个 loop 线程跑 epoll_wait——自己等自己。防御：`assertInLoopThread` 反向检查（muduo 有 `assertNotInLoopThread` 类似机制），或者 syncCall 内部检测 `isInLoopThread()` 时直接返回错误/改为内部驱动循环。你的 RpcClient.h 里 `syncCall` 注释写了"基于 asyncCall + 条件变量"，实现时必须处理这个 case。
7. `epoll_ctl` 是一次性成本（连接建立/断开时才调用）；poll 的 O(N) 是**每轮事件循环都要付**的成本。连接生命周期内事件轮数远大于 1，摊销下来 epoll 完胜。muduo 的 `EPollPoller::update` 只在 Channel 状态变化时被调用，正是这个设计。

## 与你 RPC 项目（my-light-framework）的直接联系

- **超时/心跳没有现成轮子**：muduo-core 精简版无 TimerQueue。方案 A：框架层用 `timerfd_create` + 自建 Channel 挂到 loop（优雅，实验 01 就是积木）；方案 B：独立定时线程 + `runInLoop` 投递（简单，实验 05 的 queueInLoop 就是地基）。
- **RpcCodec 的拆包**：LT 之下 `onData` 可重入是天然保证（实验 02），但半截帧必须存 `pendingBuffer_`。
- **syncCall 的线程安全**：实验 05 思考题 6 就是你 Phase 7 会踩的坑，提前想好。
- **面试叙事**：这 5 个实验就是你简历项目"epoll + Reactor"部分的弹药库——每个数字（LT 提醒 4 次、poll 慢 100 倍、wakeup 延迟 µs 级）都是你亲手测出来的。
