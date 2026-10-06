// EXPECT-ERROR: WORDS
// 3.0: CSProcessStatic<N> needs N >= CSP_MIN_STACK_WORDS (64 words); N is in words, not bytes.
#include "csp/csp4cmsis.h"
struct P : csp::CSProcessStatic<16> { void run() override {} };
P p;
