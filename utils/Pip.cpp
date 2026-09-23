#include "Pip.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
#if !defined(__linux) && !defined(__linux__)
int Fcntl(int fd, int command, int argument = 0)
{
    int result;
    do { result = ::fcntl(fd, command, argument); }
    while (result < 0 && errno == EINTR);
    return result;
}

bool ConfigurePipeEnd(int fd)
{
    const int status = Fcntl(fd, F_GETFL);
    if (status < 0 || Fcntl(fd, F_SETFL, status | O_NONBLOCK) < 0) return false;
    const int descriptor = Fcntl(fd, F_GETFD);
    return descriptor >= 0 && Fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) == 0;
}
#endif
}

Pip::Pip()
{
}

bool Pip::Create()
{
    int descriptors[2]{-1, -1};
    int result;
    do
    {
#if defined(__linux) || defined(__linux__)
        result = ::pipe2(descriptors, O_NONBLOCK | O_CLOEXEC);
#else
        result = ::pipe(descriptors);
#endif
    }
    while (result < 0 && errno == EINTR);
    if (result < 0) return false;

#if !defined(__linux) && !defined(__linux__)
    if (!ConfigurePipeEnd(descriptors[0]) || !ConfigurePipeEnd(descriptors[1])
#ifdef F_SETNOSIGPIPE
        || Fcntl(descriptors[1], F_SETNOSIGPIPE, 1) < 0
#endif
       )
    {
        const int error = errno;
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        errno = error;
        return false;
    }
#endif
    Close();
    pipe_fd_[0] = descriptors[0];
    pipe_fd_[1] = descriptors[1];
    return true;
}

int Pip::Write(void *buf, int len)
{
    if (len < 0) { errno = EINVAL; return -1; }
    ssize_t result;
    do { result = ::write(pipe_fd_[1], buf, static_cast<size_t>(len)); }
    while (result < 0 && errno == EINTR);
    return static_cast<int>(result);
}

int Pip::Read(void *buf, int len)
{
    if (len < 0) { errno = EINVAL; return -1; }
    ssize_t result;
    do { result = ::read(pipe_fd_[0], buf, static_cast<size_t>(len)); }
    while (result < 0 && errno == EINTR);
    return static_cast<int>(result);
}

void Pip::Close()
{
    if (pipe_fd_[0] >= 0)
    {
        ::close(pipe_fd_[0]);
        pipe_fd_[0] = -1;
    }

    if (pipe_fd_[1] >= 0)
    {
        ::close(pipe_fd_[1]);
        pipe_fd_[1] = -1;
    }
}

int Pip::ReadFd() const
{
    return pipe_fd_[0];
}

int Pip::WriteFd() const
{
    return pipe_fd_[1];
}
