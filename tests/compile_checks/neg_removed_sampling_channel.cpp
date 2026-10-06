// EXPECT-ERROR: SamplingChannel
// No longer public in 3.0: Channel<T> is the rendezvous channel.
#include "csp/csp4cmsis.h"
csp::SamplingChannel<int> ch;
