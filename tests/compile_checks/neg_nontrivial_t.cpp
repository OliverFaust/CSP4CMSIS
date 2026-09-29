// EXPECT-ERROR: T must be trivially copyable
#include "csp/csp4cmsis.h"
struct NonTrivial { NonTrivial() {} NonTrivial(const NonTrivial&) {} };
csp::internal::BufferedChannel<NonTrivial, 4> c;
