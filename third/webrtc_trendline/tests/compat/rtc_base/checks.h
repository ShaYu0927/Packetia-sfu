#pragma once
#include <cassert>
#define RTC_DCHECK(condition) assert(condition)
#define RTC_DCHECK_EQ(a, b) assert((a) == (b))
#define RTC_DCHECK_GE(a, b) assert((a) >= (b))
#define RTC_DCHECK_LT(a, b) assert((a) < (b))
