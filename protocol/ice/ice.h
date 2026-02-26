#ifndef _ICE_H_
#define _ICE_H_

#include <cstddef>
#include <cstdint>

namespace ice
{
/* ---------- implementation constants (not wire-format) ---------- */
struct Limits final 
{
  /* Foundation tag used to group candidates. */
  static constexpr std::size_t kFoundationMax = 48;

  static constexpr std::size_t kFoundationBuf = kFoundationMax + 1;

  /* Pair tag usually concatenates local + separator + remote (+ '\0') */
  static constexpr std::size_t kPairTagBuf = (kFoundationMax * 2) + 2;

  /* Cap different foundation groups to avoid checklist explosion. */
  static constexpr std::size_t kFoundationGroupCap = 16;

  /* Pacing: max checks per scheduler tick. */
  static constexpr std::size_t kCheckBurstPerTick = 8;

  /* Internal IDs (for logs/debug only; arbitrary) */
  static constexpr std::uint16_t kPairIdBase = 10;
  static constexpr std::uint16_t kCheckIdBase = 1000;
};


enum class ComponentId : std::uint8_t 
{
   Rtp  = 1,
   Rtcp = 2,
};


constexpr bool is_valid(ComponentId c) noexcept 
{
   return c == ComponentId::Rtp || c == ComponentId::Rtcp;
}


} // namespace ice



#endif /* _ICE_H_ */