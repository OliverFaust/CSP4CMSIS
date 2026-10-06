// EXPECT-ERROR: template argument
// Replaces FVP test T16n: sampling policies only on buffered channels; Channel<T> has none.
#include "csp/csp4cmsis.h"
csp::Channel<unsigned, csp::BufferPolicy::KeepNewest> ch;
