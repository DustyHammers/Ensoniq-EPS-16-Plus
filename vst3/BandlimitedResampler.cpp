#include "BandlimitedResampler.h"

/* BandlimitedResamplerT is a header-only template (see BandlimitedResampler.h
   for why: it needs to be instantiated at two different OversampleFactor
   values, 1 and 8, with provably-equal latency between them). This
   translation unit exists only so CMake's existing source lists for this
   file keep compiling unchanged. */
