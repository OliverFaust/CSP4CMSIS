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
//   0x00  ProcessStart     Op     thread                    process object
//   0x01  ProcessEnd       Op     thread                    process object
//   0x02  ProcessName      Op     (data record: the process's name(), after ProcessStart)
//   0x10  RendezvousDone   Op     second arriver's thread   rendezvous core (channel + 4)
//   0x20  AltSelected      Op     thread                    guard-list index
//   0x30  BufferedPut      Op     thread                    channel
//   0x31  BufferedGet      Op     thread                    channel
//   0x32  PutFromISR       Op     0 (interrupt)             channel
//   0x40  SpawnFailed      Error  spawning thread           process object
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
#define CSP4CMSIS_EVT_SPAWN_FAILED       0x40U

#if defined(CSP4CMSIS_TRACE)
  #include <stdint.h>
  #include <string.h>
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

  /// The calling thread, or 0 in an interrupt handler. Needs the device
  /// header (csp_critical.h) for __get_IPSR().
  #define CSP4CMSIS_TRACE_SELF() \
      ((__get_IPSR() != 0U) ? 0U : (uint32_t)(uintptr_t)osThreadGetId())
#else
  #define CSP4CMSIS_TRACE_EVENT(msg, a, b)   ((void)0)
  #define CSP4CMSIS_TRACE_STRING(msg, s)     ((void)0)
#endif

#endif // CSP4CMSIS_TRACE_H
