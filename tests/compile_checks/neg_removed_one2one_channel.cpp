// EXPECT-ERROR: One2OneChannel
// Removed in 3.0: use Channel<T>.
#include "csp/csp4cmsis.h"
csp::One2OneChannel<int> ch;
