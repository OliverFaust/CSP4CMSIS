// EXPECT-ERROR: deleted
#include "csp/csp4cmsis.h"
#include <utility>
void f() { csp::internal::BufferedChannel<int, 4> a; csp::internal::BufferedChannel<int, 4> b(std::move(a)); }
