#include <iostream>
#include "./log/KXY_logger.h"
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#define NUM 10000
int main() {
    CatLog::Delete();
    CatLog::Instance();
    std::thread thread_test_0([]{
        for(int i = 0; i < NUM; i++)
        {
            std::cout << "Attempting to write log: ./kxyLog" << std::endl;
            CatLog::__Write_Log("/home/roots/CLionProjects/FFmpegAAc/kxyLog",__DEBUG_LOG("log: " + std::to_string(i)));
        }
    });
    thread_test_0.join();
    CatLog::Delete();

    return 0;
}


