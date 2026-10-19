// Trace tests: one static pool for the processes of the tests that
// trace_tests.cpp runs one after the other (the trace builds of small
// targets have little RAM to spare). A test constructs its processes in the
// pool, runs them, then calls trace_pool_release(), which waits until their
// threads are gone (FreeRTOS deletes a task that exited in its idle task)
// before the next test reuses the memory. trace_tests.cpp owns the pool.
#ifndef CSP4CMSIS_TRACE_POOL_H
#define CSP4CMSIS_TRACE_POOL_H
#include <cstddef>
#include <new>
#include <utility>

extern "C" void* csp4cmsis_trace_pool(size_t bytes);   // the pool, if bytes fit (else fatal)
extern "C" void csp4cmsis_trace_pool_release(void);

// Constructs a T at offset `off` of the pool (aligned) and advances `off`.
template <typename T, typename... A>
T& trace_pool_new(size_t& off, A&&... args) {
    off = (off + alignof(T) - 1u) & ~(alignof(T) - 1u);
    unsigned char* base = static_cast<unsigned char*>(csp4cmsis_trace_pool(off + sizeof(T)));
    T* p = new (base + off) T(std::forward<A>(args)...);
    off += sizeof(T);
    return *p;
}

#endif
