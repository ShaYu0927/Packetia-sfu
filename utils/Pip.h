#ifndef _PIP_H_
#define _PIP_H_



class Pip
{
public:

    Pip();
    bool  Create();
	int   Write(void *buf, int len);
	int   Read(void *buf, int len);
	void  Close();

    int Read() const { return pipe_fd_[0]; }
	int Write() const { return pipe_fd_[1]; }

    int ReadFd() const;
    int WriteFd() const;

private:
    int pipe_fd_[2];
};

#endif