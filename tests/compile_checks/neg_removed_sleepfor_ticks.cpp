// EXPECT-ERROR: could not convert || no matching function || no viable conversion
// 3.0: SleepFor() takes a Time only (Ticks(n), Milliseconds(ms), Seconds(s), Forever).
#include "csp/csp4cmsis.h"
void f() { csp::SleepFor(10); }
