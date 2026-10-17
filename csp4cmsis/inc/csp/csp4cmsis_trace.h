#ifndef CSP4CMSIS_TRACE_H
#define CSP4CMSIS_TRACE_H

// Event tracing to CMSIS-View Event Recorder (opt-in).
//
// Without CSP4CMSIS_TRACE (the default) every hook expands to nothing: its
// arguments are not evaluated, EventRecorder.h is not included, and the
// generated code is that of a build without this header.
//
// With CSP4CMSIS_TRACE (define it for the whole project: channel templates
// are compiled in application files) the library records the events below
// as component 0x50. The project provides CMSIS-View:Event Recorder,
// initialises it and enables the component, e.g. after EventRecorderInitialize()
// or the RTOS's own Event Recorder set-up:
//     EventRecorderEnable(EventRecordAll, 0x50U, 0x50U);
// trace/CSP4CMSIS.scvd decodes the events (eventlist, uVision, Keil Studio).
//
//   msg   event            level  val1 (a)                  val2 (b)
//   0x00  ProcessStart     Op     context (its thread)      process object
//   0x01  ProcessEnd       Op     context (its thread)      process object
//   0x02  ProcessName      Op     (data record: the process's name(), after ProcessStart)
//   0x10  RendezvousDone   Op     context (second arriver)  channel
//   0x20  AltSelected      Op     context (its thread)      guard-list index
//   0x30  BufferedPut      Op     context                   channel
//   0x31  BufferedGet      Op     context                   channel
//   0x32  PutFromISR       Op     context (handler mode)    channel
//   0x33  DroppedNew       Op     context                   channel (KeepOldest, full: put rejected)
//   0x34  OverwroteOldest  Op     context                   channel (KeepNewest, full: oldest replaced)
//   0x40  SpawnFailed      Error  context                   process object
//
// Context (a): the thread id (osThreadId_t) in thread mode while the kernel
// runs; 0x80000000 | IPSR (the exception number) in handler mode; 0 in thread
// mode before the kernel runs.
//
// Invariants: every put to a buffered channel produces exactly one record:
// BufferedPut (PutFromISR in handler mode), DroppedNew or OverwroteOldest
// (putFromISR() on a full Block channel returns false and is no put); every
// get one BufferedGet; every rendezvous communication exactly one
// RendezvousDone.
//
// Not traced: blocking and unblocking. They would go before and after the
// waits in RendezvousCore::output()/input() (serial semaphore) and waitDone(),
// BufferedChannel::input()/output() (items_/spaces_ semaphores),
// AltScheduler::select() phase 2 (thread flags), Barrier::sync() and
// ParallelHelper::execute_terminating() (completion semaphore).

#define CSP4CMSIS_TRACE_COMPONENT        0x50U

#define CSP4CMSIS_EVT_PROCESS_START      0x00U
#define CSP4CMSIS_EVT_PROCESS_END        0x01U
#define CSP4CMSIS_EVT_PROCESS_NAME       0x02U
#define CSP4CMSIS_EVT_RENDEZVOUS_DONE    0x10U
#define CSP4CMSIS_EVT_ALT_SELECTED       0x20U
#define CSP4CMSIS_EVT_BUFFERED_PUT       0x30U
#define CSP4CMSIS_EVT_BUFFERED_GET       0x31U
#define CSP4CMSIS_EVT_PUT_FROM_ISR       0x32U
#define CSP4CMSIS_EVT_DROPPED_NEW        0x33U
#define CSP4CMSIS_EVT_OVERWROTE_OLDEST   0x34U
#define CSP4CMSIS_EVT_SPAWN_FAILED       0x40U

#if defined(CSP4CMSIS_TRACE)
  #include <stdint.h>
  #include <string.h>
  #include "cmsis_os2.h"
  #include "csp_critical.h"       // device header: __get_IPSR()
  #include "EventRecorder.h"

  // EventID() does not parenthesise its arguments.
  #define CSP4CMSIS_TRACE_ID(msg) \
      EventID((((msg) == CSP4CMSIS_EVT_SPAWN_FAILED) ? EventLevelError : EventLevelOp), \
              (CSP4CMSIS_TRACE_COMPONENT), (msg))

  /// One event with two values (a, b: integers or pointers).
  #define CSP4CMSIS_TRACE_EVENT(msg, a, b) \
      ((void)EventRecord2(CSP4CMSIS_TRACE_ID(msg), (uint32_t)(uintptr_t)(a), (uint32_t)(uintptr_t)(b)))

  /// One event carrying a NUL-terminated string (without the NUL).
  #define CSP4CMSIS_TRACE_STRING(msg, s) \
      ((void)EventRecordData(CSP4CMSIS_TRACE_ID(msg), (s), (uint32_t)strlen(s)))

  namespace csp::internal {
      /// Context value a (see above). Safe with BASEPRI raised: RTX5 and the
      /// FreeRTOS adapters answer osKernelGetState()/osThreadGetId() directly
      /// when interrupts are masked.
      inline uint32_t traceContext() {
          uint32_t ipsr = __get_IPSR();
          if (ipsr != 0U) return 0x80000000UL | ipsr;
          osKernelState_t k = osKernelGetState();
          if (k != osKernelRunning && k != osKernelLocked) return 0U;
          return (uint32_t)(uintptr_t)osThreadGetId();
      }
  }
  #define CSP4CMSIS_TRACE_SELF() (::csp::internal::traceContext())
#else
  #define CSP4CMSIS_TRACE_EVENT(msg, a, b)   ((void)0)
  #define CSP4CMSIS_TRACE_STRING(msg, s)     ((void)0)
#endif

#endif // CSP4CMSIS_TRACE_H
