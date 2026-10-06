// EXPECT-ERROR: not a template || expected unqualified-id
// 3.0: SignalChannel is a plain class (Block only), no longer a template.
#include "csp/csp4cmsis.h"
csp::SignalChannel<csp::BufferPolicy::Block> sig;
