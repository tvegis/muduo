#include <functional>
#include <string>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sys/sendfile.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close

#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null!\n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpConnection::TcpConnection(EventLoop *loop,
                             const std::string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CheckLoopNotNull(loop))
    , name_(nameArg)
    , state_(kConnecting)
    , reading_(true)
    , socket_(new Socket(sockfd))
    , channel_(new Channel(loop, sockfd))
    , localAddr_(localAddr)
    , peerAddr_(peerAddr)
    , highWaterMark_(64 * 1024 * 1024) // 64M
    , sendingFileFd_(-1)
    , sendingFileOffset_(0)
    , sendingFileRemaining_(0)
{
    // 下面给channel设置相应的回调函数 poller给channel通知感兴趣的事件发生了 channel会回调相应的回调函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::ctor[%s] at fd=%d\n", name_.c_str(), sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at fd=%d state=%d\n", name_.c_str(), channel_->fd(), (int)state_);
}

void TcpConnection::send(const std::string &buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread()) // 这种是对于单个reactor的情况 用户调用conn->send时 loop_即为当前线程
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(
                std::bind(&TcpConnection::sendInLoop, this, buf.c_str(), buf.size()));
        }
    }
}

/**
 * 发送数据 应用写的快 而内核发送数据慢 需要把待发送数据写入缓冲区，而且设置了水位回调
 **/
void TcpConnection::sendInLoop(const void *data, size_t len)
{
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    if (state_ == kDisconnected) // 之前调用过该connection的shutdown 不能再进行发送了
    {
        LOG_ERROR("disconnected, give up writing");
    }

    // 表示channel_第一次开始写数据或者缓冲区没有待发送数据
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)
            {
                // 既然在这里数据全部发送完成，就不用再给channel设置epollout事件了
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }
        else // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK) // EWOULDBLOCK表示非阻塞情况下没有数据后的正常返回 等同于EAGAIN
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if (errno == EPIPE || errno == ECONNRESET) // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }
    /**
     * 说明当前这一次write并没有把数据全部发送出去 剩余的数据需要保存到缓冲区当中
     * 然后给channel注册EPOLLOUT事件，Poller发现tcp的发送缓冲区有空间后会通知
     * 相应的sock->channel，调用channel对应注册的writeCallback_回调方法，
     * channel的writeCallback_实际上就是TcpConnection设置的handleWrite回调，
     * 把发送缓冲区outputBuffer_的内容全部发送完成
     **/
    if (!faultError && remaining > 0)
    {
        // 目前发送缓冲区剩余的待发送的数据的长度
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
        }
        outputBuffer_.append((char *)data + nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting(); // 这里一定要注册channel的写事件 否则poller不会给channel通知epollout
        }
    }
}

void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kDisconnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, this));
    }
}

void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明当前outputBuffer_的数据全部向外发送完成
    {
        socket_->shutdownWrite();
    }
}

// 连接建立
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading(); // 向poller注册channel的EPOLLIN读事件

    // 新连接建立 执行回调
    connectionCallback_(shared_from_this());
}
// 连接销毁
void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件从poller中删除掉
        connectionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中删除掉
}

// 读是相对服务器而言的 当对端客户端有数据到达 服务器端检测到EPOLLIN 就会触发该fd上的回调 handleRead取读走对端发来的数据
void TcpConnection::handleRead(Timestamp receiveTime)
{
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0) // 有数据到达
    {
        // 已建立连接的用户有可读事件发生了 调用用户传入的回调操作onMessage shared_from_this就是获取了TcpConnection的智能指针
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0) // 客户端断开
    {
        handleClose();
    }
    else // 出错了
    {
        errno = savedErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}

void TcpConnection::handleWrite()
{
    if (!channel_->isWriting())
    {
        LOG_ERROR("TcpConnection fd=%d is down, no more writing", channel_->fd());
        return;
    }

    // 优先处理零拷贝文件传输的继续（由 sendFileInLoop 注册的 EPOLLOUT 触发）
    if (sendingFileFd_ >= 0)
    {
        int fd = sendingFileFd_;
        off_t offset = sendingFileOffset_;
        size_t remaining = sendingFileRemaining_;
        // 重置状态，sendFileInLoop 会在未完成时重新设置
        sendingFileFd_ = -1;
        sendFileInLoop(fd, offset, remaining);
        return;
    }

    // 普通的 Buffer 写操作
    int savedErrno = 0;
    ssize_t n = outputBuffer_.writeFd(channel_->fd(), &savedErrno);
    if (n > 0)
    {
        outputBuffer_.retrieve(n);//从缓冲区读取reable区域的数据移动readindex下标
        if (outputBuffer_.readableBytes() == 0)
        {
            channel_->disableWriting();
            if (writeCompleteCallback_)
            {
                // TcpConnection对象在其所在的subloop中 向pendingFunctors_中加入回调
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this()));
            }
            if (state_ == kDisconnecting)
            {
                shutdownInLoop(); // 在当前所属的loop中把TcpConnection删除掉
            }
        }
    }
    else
    {
        LOG_ERROR("TcpConnection::handleWrite");
    }
}

void TcpConnection::handleClose()
{
    LOG_INFO("TcpConnection::handleClose fd=%d state=%d\n", channel_->fd(), (int)state_);
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr connPtr(shared_from_this());
    connectionCallback_(connPtr); // 连接回调
    closeCallback_(connPtr);      // 执行关闭连接的回调 执行的是TcpServer::removeConnection回调方法   // must be the last line
}

void TcpConnection::handleError()
{
    int optval;
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0)
    {
        err = errno;
    }
    else
    {
        err = optval;
    }
    LOG_ERROR("TcpConnection::handleError name:%s - SO_ERROR:%d\n", name_.c_str(), err);
}

// 新增的零拷贝发送函数
void TcpConnection::sendFile(int fileDescriptor, off_t offset, size_t count) {
    if (connected()) {
        if (loop_->isInLoopThread()) { // 判断当前线程是否是loop循环的线程
            sendFileInLoop(fileDescriptor, offset, count);
        }else{ // 如果不是，则唤醒运行这个TcpConnection的线程执行Loop循环
            loop_->runInLoop(
                std::bind(&TcpConnection::sendFileInLoop, shared_from_this(), fileDescriptor, offset, count));
        }
    } else {
        LOG_ERROR("TcpConnection::sendFile - not connected");
    }
}

// 在事件循环中执行sendfile（零拷贝文件传输）
//  对齐 sendInLoop 模式：先尝试直接发送，若内核缓冲区满则注册 EPOLLOUT，
//  等待 handleWrite 回调时继续发送，而非递归 queueInLoop 形成忙等。
void TcpConnection::sendFileInLoop(int fileDescriptor, off_t offset, size_t count) {
    if (state_ == kDisconnecting) {
        LOG_ERROR("disconnected, give up writing");
        return;
    }

    // 必须在 outputBuffer_ 为空时才能发送文件（保证数据顺序）
    if (outputBuffer_.readableBytes() > 0) {
        LOG_ERROR("sendFileInLoop: outputBuffer not empty, data ordering would break");
        return;
    }

    ssize_t bytesSent = ::sendfile(socket_->fd(), fileDescriptor, &offset, count);
    if (bytesSent >= 0) {
        size_t remaining = count - static_cast<size_t>(bytesSent);
        if (remaining == 0) {
            // 全部发送完成
            sendingFileFd_ = -1;
            if (writeCompleteCallback_) {
                loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
        } else {
            // 部分发送，保存状态，注册 EPOLLOUT 等待内核缓冲区可写
            sendingFileFd_ = fileDescriptor;
            sendingFileOffset_ = offset;
            sendingFileRemaining_ = remaining;
            if (!channel_->isWriting()) {
                channel_->enableWriting();
            }
        }
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 内核发送缓冲区满，保存状态，注册 EPOLLOUT
            sendingFileFd_ = fileDescriptor;
            sendingFileOffset_ = offset;
            sendingFileRemaining_ = count;
            if (!channel_->isWriting()) {
                channel_->enableWriting();
            }
        } else if (errno == EPIPE || errno == ECONNRESET) {
            LOG_ERROR("TcpConnection::sendFileInLoop: connection reset");
            sendingFileFd_ = -1;
        } else {
            LOG_ERROR("TcpConnection::sendFileInLoop: sendfile error %d", errno);
            sendingFileFd_ = -1;
        }
    }
}