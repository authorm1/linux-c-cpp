本项目使用 C 语言实现了两种经典的高性能网络服务器模型：

1.  **单 Reactor + 多线程 (Worker Pool)**
2.  **主从 Reactor + 多线程 (Worker Pool)**

`thread_pool.c` 和 `thread_pool.h` 是一个独立的线程池实现，用于解耦 I/O 线程和业务逻辑线程。

## 1. 单 Reactor + 多线程

此模型使用一个单独的 Reactor 线程（主线程）处理所有的 I/O 事件（`accept`, `recv`, `send`），并将业务逻辑分发给一个 Worker 线程池。

**编译与运行:**

```c
cd src/
gcc -o single_reactor single_reactor.c thread_pool.c -lpthread
./single_reactor 8888

```

### 模型架构：

*   **单 Reactor (I/O 线程)**：利用 `epoll` 管理所有 I/O，只负责处理 `accept`、`recv`、`send` 等 I/O 事件。
*   **多 Worker (工作线程)**：由线程池管理，只负责业务逻辑处理（模拟 10ms 耗时），完全不接触 I/O 操作。

### 流程：

1.  I/O 线程在 `main` 循环中 `epoll_wait`。对于 `EPOLLIN` 事件，如果是 `listenfd`，调用 `accept_callback` 将新的 `clientfd` 注册到 `epoll` 上；如果是 `clientfd`，调用 `recv_callback`。
2.  `recv_callback` 读取数据，将请求数据打包为 `task` 并推入线程池的任务队列。
3.  工作线程从任务队列中取出任务，执行 `task_callback`，调用 `protocol_handle` (模拟耗时 10ms 的业务)。
4.  业务处理完毕后，将响应数据打包为 `result_t` 推入结果队列 `result_queue` 中（使用互斥锁保证线程安全），随后利用 `eventfd` 通知 I/O 线程。
5.  I/O 线程的 `epoll_wait` 被 `eventfd` 唤醒。
6.  I/O 线程调用 `handle_pending_results`，从 `result_queue` 中取出所有响应，并将结果数据挂载到 `clientfd` 绑定的 `w_buffer` 上，然后修改 `epoll`，关注该 `clientfd` 的 `EPOLLOUT` (写) 事件。
7.  当 `clientfd` 的 TCP 发送缓冲区可用时，触发 `EPOLLOUT`。Reactor 调用 `send_callback`，将 `w_buffer` 中的数据发送出去，然后重新关注 `EPOLLIN` (读) 事件。

### 流程图：
[!single_reactor](./docs/images/single_reactor.png)

***

## 2. 主从 Reactor + 多线程

此模型将 I/O 事件进一步拆分：Main Reactor 只负责 `accept`，Sub-Reactor 负责 `recv` 和 `send`。业务逻辑依然交给 Worker 线程池。

**编译与运行:**

```c
cd src/
gcc -o multi_reactor multi_reactor.c thread_pool.c -lpthread
./multi_reactor 8888

```

### 模型架构：

*   **Main Reactor (主 I/O 线程)**：1 个线程，只负责 `accept` 新的网络连接。
*   **Sub-Reactors (从 I/O 线程)**：N 个线程，每个线程维护一个独立的 `epoll`。它们负责处理已连接 `fd` 上的 `recv` 和 `send` 事件。
*   **多 Worker (工作线程)**：M 个线程（线程池），只负责业务逻辑处理。

### 流程：

1.  Main Reactor 在 `epoll_wait` 中等待 `listenfd` 的 `EPOLLIN` 事件。
2.  新连接到来时，Main Reactor 调用 `accept_callback`，`accept` 得到 `clientfd`。
3.  Main Reactor 通过轮询（或哈希）算法 (`clientfd % SUBREACTOR_THREAD_NUM`) 将 `clientfd` 分配给一个 Sub-Reactor。
4.  Main Reactor 将 `clientfd` 放入该 Sub-Reactor 专属的 `fd_queue`，并通过 `eventfd` 唤醒该 Sub-Reactor 线程。
5.  Sub-Reactor 被 `fdqueue_eventfd` 唤醒，从 `fd_queue` 中取出 `clientfd`，并将其注册到**自己**的 `epoll` 实例上，监听 `EPOLLIN`。
6.  当 `clientfd` 数据可读时，**Sub-Reactor** 的 `epoll_wait` 触发 `EPOLLIN`。
7.  Sub-Reactor 调用 `recv_callback` 读取数据，将请求打包为 `task`，并推入**全局**的 `g_worker_pool` 任务队列。
8.  Worker 线程从任务队列中取出任务，执行 `task_callback` (处理 10ms 业务)。
9.  Worker 处理完毕后，将 `result_t` (响应数据) 推入**来源 Sub-Reactor** 的 `result_queue` 中。
10. Worker 通过该 Sub-Reactor 专属的 `resqueue_eventfd` 唤醒它。
11. Sub-Reactor 被 `resqueue_eventfd` 唤醒，调用 `handle_pending_results` 取出结果，挂载到 `w_buffer`，并注册 `EPOLLOUT`。
12. 当 `fd` 可写时，Sub-Reactor 的 `epoll_wait` 触发 `EPOLLOUT`，调用 `send_callback` 发送数据，然后重新关注 `EPOLLIN`。

### 流程图：
[!single_reactor](./docs/images/multi_reactor.png)

***

## 3. 压力测试程序

项目包含一个简单的 C 语言 `testcase` 用于压力测试。

**编译:**

```c
gcc -o testcase testcase.c -lpthread
```

**运行:**

```c
cd testcase/
./testcase 127.0.0.1 8888 10000 10
```

**参数说明:** `./testcase [IP] [Port] [Total Requests] [Concurrency]`

*   `IP`: 服务器 IP 地址 (例如: 127.0.0.1)
*   `Port`: 服务器端口 (例如: 8888)
*   `Total Requests`: 总共要发送的请求数量 (例如: 10000)
*   `Concurrency`: 并发线程数 (例如: 10)

