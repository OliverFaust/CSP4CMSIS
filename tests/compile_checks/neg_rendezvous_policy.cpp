// EXPECT-ERROR: KeepNewest/KeepOldest need a buffer
// Replaces FVP test T16n: sampling policies only on buffered channels.
#include "csp/csp4cmsis.h"
csp::SamplingChannel<unsigned, csp::BufferPolicy::KeepNewest> ch;
