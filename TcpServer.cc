#include "TcpServer.h"
#include "Logger.h"
#include "Socket.h"
#include "TcpConnection.h"

#include <functional>

static EventLoop* CheckLoopNotNull(EventLoop *loop)
{
    if (loop == nullptr)
    {
        LOG_FATAL("%s:%s:%d mainLoop is null! \n", __FILE__, __FUNCTION__, __LINE__);
    }
    return loop;
}

TcpServer::TcpServer(EventLoop *loop,
              const InetAddress &listenAddr,
              const std::string &nameArg,
              Option option)
              : loop_(CheckLoopNotNull(loop))
              , ipPort_(listenAddr.toIpPort())
              , name_(nameArg)
              , acceptor_(new Acceptor(loop, listenAddr, option == kReusePort))
              , threadPool_(new EventLoopThreadPool(loop, name_))
              , connetionCallback_()
              , messageCallback_()
              , nextConnId_(1)
              , started_(0)
{
    // 当有新用户连接时，会执行TcpServer::newConnection回调
    acceptor_->setNewConnectionCallback(std::bind(&TcpServer::newConnection, this,
                                    std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer()
{
    for (auto &item : connections_)
    {
        // 这个局部的shared_ptr智能指针对象，出右括号，可以自动释放new出来的TcpConnection对象资源了
        TcpConnectionPtr conn(item.second);
        item.second.reset();

        // 销毁连接     需要使用TcpConnection的回调，因此回调结束前TcpConnection对象仍需存活
        conn->getLoop()->runInLoop(
            std::bind(&TcpConnection::connectDestroyed, conn)
        );
    }
}

// 设置底层subLoop的个数
void TcpServer::setThreadNum(int numThreads)
{
    threadPool_->setThreadNum(numThreads);
}

// 开启服务器监听   loop.loop()
void TcpServer::start()
{
    // 防止一个TcpServer对象被start多次
    if (started_++ == 0)
    {
        
        threadPool_->start(threadInitCallback_);    // 启动底层的loop线程池
        loop_->runInLoop(std::bind(&Acceptor::listen, acceptor_.get()));
    }
}

// 有一个新的客户端连接，acceptor会执行这个回调操作
void TcpServer::newConnection(int sockfd, const InetAddress &peerAddr)
{

    // 轮询算法，选择一个subLoop，来管理channel
    EventLoop* ioLoop = threadPool_->getNextLoop();
    char buf[64] = {0};
    snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
    ++nextConnId_;  // 由于限制了只存在一个loop运行TcpServer，所以不会出现多线程问题，不需要使用原子变量
    std::string connName = name_ + buf;

    LOG_INFO("TcpServer::newConnection [%s] - new connection [%s] from %s"
            , name_.c_str(), connName.c_str(), peerAddr.toIpPort().c_str());
    
    InetAddress localAddr(Socket::getLocalAddr(sockfd));

    // 根据连接成功的sockfd，创建TcpConnection连接对象
    TcpConnectionPtr conn(new TcpConnection(
                            ioLoop,
                            connName,
                            sockfd,     // Socket Channel
                            localAddr,
                            peerAddr
    ));
    connections_[connName] = conn;
    // 下面的回调都是用户调用给TcpServer => TcpConnection => Channel => Poller => notify    channel调用回调
    conn->setConnectionCallback(connetionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);

    // 设置了如何关闭连接的回调     conn->shutDown()
    conn->setCloseCallback(
        std::bind(&TcpServer::removeConnection, this, std::placeholders::_1)
    );

    // 直接调用TcpConnection::connectEstablished
    ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}
void TcpServer::removeConnection(const TcpConnectionPtr &conn)
{
    loop_->runInLoop(
        std::bind(&TcpServer::removeConnectionInLoop, this, conn)
    );
}
void TcpServer::removeConnectionInLoop(const TcpConnectionPtr &conn)
{
    LOG_INFO("TcpServer::removeConnectionInLoop [%s] - connection %s\n",
            name_.c_str(), conn->name().c_str());
    connections_.erase(conn->name());
    EventLoop *ioLoop = conn->getLoop();
    ioLoop->queueInLoop(
        std::bind(&TcpConnection::connectDestroyed, conn)
    );
}