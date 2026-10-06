// EXPECT-ERROR: SamplingBufferedChannel
// Removed in 3.0: BufferedChannel<T, SIZE, P> is the one buffered channel.
#include "csp/csp4cmsis.h"
csp::SamplingBufferedChannel<int, 4, csp::BufferPolicy::KeepNewest> ch;
