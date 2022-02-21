#include "Acceptor.h"
#include "InetAddress.h"
#include "Logger.h"

#include <unistd.h>

static int createNonblocking()
{
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0)
    {
            LOG_FATAL("%s:%s:%d listen sockfd create err:%d \n", __FILE__, __FUNCTION__, __LINE__, errno);
    }
    return sockfd;
}

// socket.bind()，并绑定对应channel的ReadCallback，在accept时回调
Acceptor::Acceptor(EventLoop *loop, const InetAddress &listenAddr, bool reuseport)
    : loop_(loop)
    , acceptSocket_(createNonblocking()) // socket
    , acceptChannel_(loop, acceptSocket_.fd())
    , listenning_(false)
{
    acceptSocket_.setReuseAddr(true);
    acceptSocket_.setReusePort(true);
    acceptSocket_.bindAddress(listenAddr); // bind
    // TcpServer::start() Acceptor.listen   有新用户的连接，要执行一个回调(connfd => channel => subloop)
    // baseLoop => acceptChannel_(listenfd) =>
    acceptChannel_.setReadCallback(std::bind(&Acceptor::handleRead, this));
}
Acceptor::~Acceptor()
{
    acceptChannel_.disableAll();
    acceptChannel_.remove();
}

// 开启socket.listen，并开启对应channel的read状态
void Acceptor::listen()
{
    listenning_ = true;
    acceptSocket_.listen();         // listen
    acceptChannel_.enableReading(); // acceptChannel_ => Poller
}

// 阻塞等待accept返回值，listenfd有事件发生了，就是有新用户连接了
void Acceptor::handleRead()
{
    InetAddress peerAddr;
    int connfd = acceptSocket_.accept(&peerAddr);   // accept
    if (connfd >= 0)
    {
        if (newConnectionCallback_)
        {
            // 轮询找到subLoop，唤醒，分发当前的新客户端的Channel
            newConnectionCallback_(connfd, peerAddr);
        }
        else
        {
            ::close(connfd);
        }
    }
    else
    {
        LOG_ERROR("%s:%s:%d accept err:%d \n", __FILE__, __FUNCTION__, __LINE__, errno);
        // fd reached max，can't open new fd
        if (errno == EMFILE)
        {
            LOG_ERROR("%s:%s:%d sockfd readched limit! \n", __FILE__, __FUNCTION__, __LINE__);
        }
    }
}