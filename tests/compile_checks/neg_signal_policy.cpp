// EXPECT-ERROR: KeepNewest/KeepOldest need a buffer
// Signal channels are Block-only too.
#include "csp/csp4cmsis.h"
csp::SignalChannel<csp::BufferPolicy::KeepOldest> sig;
