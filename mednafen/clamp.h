#ifndef __MDFN_CLAMP_H
#define __MDFN_CLAMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// Uses intmax_t instead of non-standard ssize_t

static INLINE void clamp(int32_t *val, intmax_t min, intmax_t max)
{
   if(*val < min)
      *val = min;
   if(*val > max)
      *val = max;
}

#ifdef __cplusplus
}
#endif

#endif
