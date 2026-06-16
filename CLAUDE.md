# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在此仓库中工作提供指导。

## 构建命令

```bash
# 从仓库根目录或 muduo-core/ 目录执行
cd muduo-core
mkdir -p build && cd build
cmake .. && cmake --build .
```

构建产物：
- `lib/libmuduo_core.so`（Windows 上为 `.dll`）—— 共享库
- `example/testserver` —— echo 服务器（监听 8080 端口）

依赖：CMake >= 3.0、C++11 编译器、pthread、Linux 头文件（`<sys/epoll.h>`、`<arpa/inet.h>` 等）

**注意：** 本项目依赖 Linux 专有 API（epoll、eventfd、syscall），在 Windows 上无法编译，必须在 Linux 环境下构建。

## 架构：Multi-Reactor TCP 服务器

本项目是陈硕 muduo 网络库的精简实现，基于 **Reactor 模式**，采用 epoll I/O 复用和 **One Loop Per Thread** 并发模型。

### 核心三元组（Reactor 基础）

以下三个类构成了事件循环的心脏：

- **`EventLoop`** —— 事件循环。持有一个 `Poller` 和活跃 `Channel` 列表。`loop()` 反复调用 `poller_->poll()`，然后将每个活跃 Channel 的回调分发出去。每个线程最多一个 EventLoop（`one loop per thread`）。提供 `runInLoop()`/`queueInLoop()` 实现跨线程任务提交，通过 eventfd（`wakeupFd_`）唤醒。

- **`Poller`** / **`EPollPoller`** —— 抽象 I/O 复用器；具体 epoll 实现。封装 `epoll_create`/`epoll_ctl`/`epoll_wait`。`newDefaultPoller()` 工厂方法创建平台相关实例。

- **`Channel`** —— 封装单个文件描述符及其关注的事件（EPOLLIN、EPOLLOUT）。注册读/写/关闭/错误回调。Channel 通过 `EventLoop::updateChannel()` 注册到 Poller 中。

### 并发模型：One Loop Per Thread

- **`Thread`** —— `std::thread` 的轻量封装，附带 tid 缓存。
- **`EventLoopThread`** —— 唯一职责是运行一个 EventLoop 的线程。`startLoop()` 启动线程并返回 loop 指针。
- **`EventLoopThreadPool`** —— 上述线程的池。`getNextLoop()` 轮询分配 subLoop 以分发新连接。

### 服务器生命周期（自顶向下）

```
TcpServer.start()
  → Acceptor.listen()               // 在 mainLoop 上 bind + listen
  → EventLoopThreadPool.start()     // 启动 subLoop 线程
```

新连接到达时：
```
Acceptor::handleRead()              // 在 mainLoop 上 accept 新 fd
  → TcpServer::newConnection()       // 通过 getNextLoop() 选择 subLoop
    → TcpConnection::connectEstablished()  // 创建 Channel，注册回调
```

### 核心类

- **`TcpServer`** —— 面向用户的服务器类。用户设置回调（连接、消息、写完成），配置线程数，然后调用 `start()`。内部创建 `Acceptor` 和 `EventLoopThreadPool`。

- **`Acceptor`** —— 在 mainLoop 中监听服务端 socket。有新连接时调用注册的 `NewConnectionCallback`（`TcpServer` 将其绑定到 `newConnection()`）。

- **`TcpConnection`** —— 表示一条 TCP 连接。持有一个 `Socket`、一个 `Channel` 和两个 `Buffer`（输入/输出）。回调由 `TcpServer` 设置并转发到底层 `Channel`。通过 `enable_shared_from_this` + `tie()` + `weak_ptr` 实现安全的生命周期管理——防止 Channel 回调执行时 TcpConnection 已被析构。

- **`Socket`** —— socket fd 的 RAII 封装（`sockfd_`）。提供 `bindAddress()`、`listen()`、`accept()`、`shutdownWrite()` 以及 socket 选项设置（NoDelay、ReuseAddr、ReusePort、KeepAlive）。

- **`Buffer`** —— 基于 `std::vector<char>` 的可前置缓冲区。维护 reader/writer 两个索引。头部预留 8 字节 prependable 空间，方便协议头添加。`readFd()`/`writeFd()` 使用 `readv()`/`write()` 进行分散/聚集 I/O。

- **`InetAddress`** —— 封装 `sockaddr_in`，表示 IP:端口。

### 工具类

- **`noncopyable`** —— 禁止拷贝和赋值的基类（删除拷贝构造和赋值运算符），派生类继承后自动不可拷贝。
- **`Logger`** —— 单例日志器，支持 INFO/ERROR/FATAL/DEBUG 四个级别。`LOG_DEBUG` 仅在定义 `MUDEBUG` 宏时启用。使用宏 `LOG_INFO`、`LOG_ERROR`、`LOG_FATAL`、`LOG_DEBUG` 输出日志。
- **`CurrentThread`** —— 通过 `syscall(SYS_gettid)` 获取线程 tid 并缓存在 `__thread` 变量中，避免重复系统调用。
- **`Timestamp`** —— 微秒精度的时间戳类。
- **`Callbacks.h`** —— 统一定义 `TcpServer` 和 `TcpConnection` 使用的所有回调函数类型别名。

### 回调传递链

用户 → `TcpServer` → `TcpConnection` → `Channel` → 由 `EventLoop::loop()` 在 `Poller::poll()` 返回后触发。

```
TcpServer.setConnectionCallback(cb)
  → 存入 TcpServer::connectionCallback_
    → 在 connectEstablished() 时传给 TcpConnection::connectionCallback_
      → 注册为 Channel::readCallback_ / writeCallback_ / closeCallback_
```

### 示例：Echo 服务器（`example/testserver.cc`）

创建 EventLoop，绑定 8080 端口，在 TcpServer 上设置连接/消息回调，开启 3 个 subLoop 线程。收到消息后从输入缓冲区读取全部数据并原样发回，实现 echo 功能。
