#include "Pip.h"

Pip::Pip()
{
}

bool Pip::Create()
{
    if (pipe2(pipe_fd_, O_NONBLOCK | O_CLOEXEC) < 0) 
    {
		return false;
	}
    return true;
}

int Pip::Write(void *buf, int len)
{
    return write(pipe_fd_[1], buf, len);
}

int Pip::Read(void *buf, int len)
{
    return ::read(pipe_fd_[0], buf, len);
}

void Pip::Close()
{
    close(pipe_fd_[0]);
    close(pipe_fd_[1]);
}
