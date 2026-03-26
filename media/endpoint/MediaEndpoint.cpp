#include "MediaEndpoint.h"

namespace media 
{
void MediaEndpoint::OnRtp(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleRtpPacket(pkt);
}

void MediaEndpoint::OnRtcp(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleRtcpPacket(pkt);
}

void MediaEndpoint::OnStun(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleStunPacket(pkt);
}

void MediaEndpoint::OnDtls(WorkJob& job)
{
    auto* pkt = static_cast<Packet*>(job.pkt);
    if (!pkt || !IsRunning()) 
    {
        return;
    }
    HandleDtlsPacket(pkt);
}

}