#pragma once

#include "noncopyable.h"
#include <arpa/inet.h>

class InetAddress;

// 类本身并不创建socket，而是管理传进来的socketfd，并辅以对应系统bind listen accept函数
class Socket : noncopyable
{
public:
    explicit Socket(int sockfd)
        : sockfd_(sockfd)
    {}
    ~Socket();

    int fd() const { return sockfd_; }
    void bindAddress(const InetAddress &Localaddr);
    void listen();
    int accept(InetAddress *peeraddr);

    void shutdownWrite();

    void setTcpNoDelay(bool on);
    void setReuseAddr(bool on);
    void setReusePort(bool on);
    void setKeepAlive(bool on);

    static int getSocketError(int sockfd);
    static sockaddr_in getLocalAddr(int sockfd);

private:
    const int sockfd_;
};