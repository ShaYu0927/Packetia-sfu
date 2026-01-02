#include "RtpThreadPool.h"

void RtpJobHandler::bind(std::uint64_t key, std::weak_ptr<RtpTrack> track)
{
}

void RtpJobHandler::unbind(std::uint64_t key)
{
}

void RtpJobHandler::handle(WorkJob &&job)
{
}
