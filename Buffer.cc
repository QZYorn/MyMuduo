#include "Buffer.h"

#include <sys/uio.h>
#include <unistd.h>

/*
 * 从fd上读取数据，Poller工作在LT模式
 * Buffer缓冲区是有大小的！但是从fd上读数据的时候，却不知道tcp数据最终的大小
 * 若读取数据过多，使用数组在栈上临时存放超额的数据，并追加缓冲区大小直至足以存放
**/
ssize_t Buffer::readFd(int fd, int* saveErrno)
{
    char extrabuf[65536];   // 栈上的内存空间 64K
    struct iovec vec[2];

    // 这是Buffer底层缓冲区剩余的可写空间大小
    const size_t writable = writableBytes();

    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;

    const  int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
    const ssize_t n = readv(fd, vec, iovcnt);
    if (n < 0)
    {
        *saveErrno = errno;
    }
    else if (n <= writable) // Buffer的可写缓冲区 已经够存储读出来的数据了
    {
        writerIndex_ += n;
    }
    else
    {
        writerIndex_ = buffer_.size();
        append(extrabuf, n - writable); // writerIndex_开始写 n - writable大小的数据
    }

    return n;
    
}

// 向fd上写数据，source:缓冲区可读区域的所有数据，dest:fd
ssize_t Buffer::writeFd(int fd, int* saveErrno)
{
    ssize_t n = ::write(fd, peek(), readableBytes());
    if (n < 0)
    {
        *saveErrno = errno;
    }
    return n;
}