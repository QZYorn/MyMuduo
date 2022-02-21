#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

#include <functional>

static EventLoop *CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null! \n", __FILE__, __FUNCTION__, __LINE__);
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
    , highWaterMark_(64 * 1024 * 1024)
{
    // 下面给channel设置相应的回调函数，poller给channel通知感兴趣的事件发生了，channel会回调相应的操作函数
    channel_->setReadCallback(
        std::bind(&TcpConnection::handleRead, this, std::placeholders::_1));
    channel_->setWriteCallback(
        std::bind(&TcpConnection::handleWrite, this));
    channel_->setCloseCallback(
        std::bind(&TcpConnection::handleClose, this));
    channel_->setErrorCallback(
        std::bind(&TcpConnection::handleError, this));

    LOG_INFO("TcpConnection::ctor[%s] at %p fd=%d", name_.c_str(), this, sockfd);
    socket_->setKeepAlive(true);
}

TcpConnection::~TcpConnection()
{
    LOG_INFO("TcpConnection::dtor[%s] at %p fd=%d state=%s", 
        name_.c_str(), this, channel_->fd(), stateToString());
}




 // 发送数据
void TcpConnection::send(const void* data, int len)
{
    send(std::string(static_cast<const char*>(data) , len));
}
// 发送数据
void TcpConnection::send(const std::string &buf)
{
    if (state_ == kConnected)
    {
        if (loop_->isInLoopThread())
        {
            sendInLoop(buf.c_str(), buf.size());
        }
        else
        {
            loop_->runInLoop(std::bind(
                &TcpConnection::sendInLoop,
                this,
                buf.c_str(),
                buf.size()
            ));
        }
    }
}

// 发送数据  应用写的快，而内核发动数据慢，需要把待发送数据写入缓冲区，而且设置了水位回调
void TcpConnection::sendInLoop(const void* data, size_t len)
{
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    // 之前调用过该connection的shutdown，不能再进行发送了
    if (state_ == kDisconnected)
    {
        LOG_ERROR("disconnected, give up writeing!");
        return;
    }

    // 表示channel_第一次开始写数据，而且缓冲区没有待发送数据
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
    {
        nwrote = ::write(channel_->fd(), data, len);
        if (nwrote >= 0)
        {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_)
            {
                // 既然在这路数据全部发送完成， 就不用再给channel设置epollout事件了
                loop_->queueInLoop(
                    std::bind(writeCompleteCallback_, shared_from_this())
                );
            }
        }
        else    // nwrote < 0
        {
            nwrote = 0;
            if (errno != EWOULDBLOCK)
            {
                LOG_ERROR("TcpConnection::sendInLoop");
                if (errno == EPIPE || errno == ECONNRESET)  // SIGPIPE RESET
                {
                    faultError = true;
                }
            }
        }
    }

    /*
     * 说明当前这一次write，并没有把数据全部发送出去，剩余的数据需要保存到缓冲区中，然后给channel
     * 注册epollout事件，poller发现tcp的发送缓冲区有空间，会通知相应的sock-channel，调用writeCallback_回调方法
     * 也就是调用TcpConnection::handlewrite方法，把发送缓冲区中的数据全部发送完成
    **/
    if (!faultError && remaining > 0)
    {
        // 目前发送缓冲区剩余的待发送数据的长度
        size_t oldlen = outputBuffer_.readableBytes();
        // 若超水位，则需水位回调
        if (oldlen + remaining >= highWaterMark_    // 目前剩余数据量需要水位回调
            && oldlen < highWaterMark_              // 上次剩余数据量无需水位回调
            && highWaterMarkCallback_)
        {
            loop_->queueInLoop(
                std::bind(highWaterMarkCallback_, shared_from_this(), oldlen + remaining)
            );
        }
        
        outputBuffer_.append((char*)data + nwrote, remaining);
        if (!channel_->isWriting())
        {
            channel_->enableWriting();  // 这里一定要注册channel的写事件，否则poller不会给channel通知epollout
        }
    }
}   



// 关闭连接
void TcpConnection::shutdown()
{
    if (state_ == kConnected)
    {
        setState(kConnecting);
        loop_->runInLoop(
            std::bind(&TcpConnection::shutdownInLoop, this)
        );
    }
}
void TcpConnection::shutdownInLoop()
{
    if (!channel_->isWriting()) // 说明outputBuffer中的数据已经全部发送完成
    {
        socket_->shutdownWrite();   // 关闭写端
    }
}


// 连接建立
void TcpConnection::connectEstablished()
{
    setState(kConnected);
    channel_->tie(shared_from_this());
    channel_->enableReading();  // 向poller注册channel的epollin事件

    // 新连接建立，执行回调
    connetionCallback_(shared_from_this());
}
// 连接销毁
void TcpConnection::connectDestroyed()
{
    if (state_ == kConnected)
    {
        setState(kDisconnected);
        channel_->disableAll(); // 把channel的所有感兴趣的事件，从poller中del掉
        connetionCallback_(shared_from_this());
    }
    channel_->remove(); // 把channel从poller中删除掉
}


void TcpConnection::handleRead(Timestamp receiveTime)
{
    int saveErrno = 0;
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &saveErrno);
    if (n > 0)
    {
        // 已建立连接的用户，有可读事件发生了，调用用户传入的回调操作onMessage
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0)
    {
        handleClose();
    }
    else
    {
        errno = saveErrno;
        LOG_ERROR("TcpConnection::handleRead");
        handleError();
    }
}
// 发送缓冲区未发送完。写到一半，没写完，仍需继续写时，回调
void TcpConnection::handleWrite()
{
    if (channel_->isWriting())
    {
        int saveErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &saveErrno);
        if (n > 0)
        {
            outputBuffer_.retrieve(n);
            if (outputBuffer_.readableBytes() == 0) // 若已写完，则关闭写监听writeEvent
            {
                channel_->disableWriting();

                if (writeCompleteCallback_)
                {
                    // 唤醒loop, 对应的thread线程，执行写完回调
                    loop_->queueInLoop(
                        std::bind(writeCompleteCallback_, this->shared_from_this())
                    );
                }
                if (state_ == kDisconnecting)
                {
                    shutdownInLoop();
                }
            }// 若未写完，下次仍会回调当前函数TcpConnection::handleWrite()
        }
        else
        {
            LOG_ERROR("TcpConnection::handleWrite");
        }
    }
    else
    {
        LOG_ERROR("Connection fd=%d is down, no more writing \n", channel_->fd());
    }
}
// poller => channel::closeCallback => TcpConnection::handleClose()
void TcpConnection::handleClose()
{
    LOG_INFO("fd = %d state = %s", channel_->fd(), stateToString());
    setState(kDisconnected);
    channel_->disableAll();

    TcpConnectionPtr guardThis(this->shared_from_this());
    connetionCallback_(guardThis);  // 执行连接关闭的回调
    closeCallback_(guardThis);      // 关闭连接的回调   执行的是TcpServer::removeConnection()
}
void TcpConnection::handleError()
{
    int err = Socket::getSocketError(channel_->fd());
    LOG_ERROR("TcpConnection::handleError [%s] - SO_ERROR = %d \n",name_.c_str(), err);
}


const char *TcpConnection::stateToString() const
{
    switch (state_)
    {
    case kDisconnected:
        return "kDisconnected";
    case kConnected:
        return "kConnected";
    case kConnecting:
        return "kConnecting";
    case kDisconnecting:
        return "kDisconnecting";

    default:
        return "unknown state";
    }
}