# CSP4CMSIS 2.0 (in development): changes and migration notes

Branch `buffered-channel-v2`. The version is **not** bumped yet: the pdsc still says 1.0.0 and the pack
has not been rebuilt. Background and evidence are in `BUFFERED_CHANNEL_ANALYSIS.md`; the regression suite
is in `tests/fvp_sse300/`.

## Behaviour changes

| Area | 1.0.0 | 2.0 |
|---|---|---|
| `SamplingBufferedChannel<T, SIZE, P>` storage | RTOS message queue, allocated from the RTOS heap | `SIZE` elements **inside the object** (static), plus two counting semaphores with static control blocks |
| KeepNewest when full | drop, then put: two queue operations; a concurrent writer or ISR could lose the newest value | the oldest element is overwritten **in place, atomically**, for task and ISR writers alike |
| ALT on a buffered channel | lost wakeups possible; an ALT read did not wake an ALTing writer | readiness is the **semaphore count** (items/spaces), checked after registering; partners release the token before they snapshot the registration and signal, so there is no lost wakeup and no busy retry; an ALT read frees space and wakes the writer |
| ALT wakeups | per-`Alternative` event-flags object | CMSIS-RTOS2 **thread flags** of the selecting thread (reserved bits, see below); no RTOS object per `Alternative` |
| `select()` | used every set bit as the choice; disabled all guards | clears its bits per round; disables only the guards it enabled; re-verifies each wakeup (`Guard::confirm`); if `activate()` loses a race (only possible when another reader or writer took the token), starts a new round; a failed wait is fatal instead of being used as a bit mask |
| RTOS calls inside CSP critical sections | `_notifyReader()`/`_notifyWriter()` called `osEventFlagsSet()` with BASEPRI raised; rendezvous `putFromISR()` too | never: state is snapshotted inside the section, and the RTOS is called after leaving it |
| `RelTimeoutGuard` / `TimerGuard` | `osTimerNew()` from the RTOS heap on every construction | static control block under `CSP4CMSIS_STATIC_ALLOCATION`; creation checked |
| CSP critical section entry | `MSR BASEPRI_MAX` only | `MSR BASEPRI_MAX; DSB; ISB` (as the FreeRTOS Cortex-M ports); Cortex-M7 r0p1 (erratum 837070) additionally needs `CPSID i`/`CPSIE i` around the MSR, which is not done (no known target uses that core) |
| Failed RTOS object creation | silently ignored (e.g. a dead channel whose `input()` returned without data) | `csp4cmsis_fatal_error()` |
| ALT guard state | one guard object per channel direction, shared by all processes | one guard per channel-end **handle** (`Chanin`/`Chanout`), for rendezvous and buffered channels |

## Rules for applications (new or now enforced)

- **At most one process ALTing on each end of a `BufferedChannel`** (one ALTing reader, one ALTing
  writer). A second, different thread is a fatal error. Any number of *blocking* readers and writers is
  allowed.
- **Do not share a `Chanin`/`Chanout` handle object between processes.** Each process keeps its own copy,
  as returned by `reader()`/`writer()`, because the handle now holds that process's guard state.
- **`T` must be trivially copyable** (`static_assert`), and **`SIZE > 0`** (`static_assert`).
- **Thread flags reserved by CSP4CMSIS** (per thread):

  | Bits | Use |
  |---|---|
  | 0 | `RENDEZVOUS_FLAG` |
  | 8–23 | ALT, guard *i* → bit 8+*i* (`altFlag(i)`, `CSP4CMSIS_ALT_FLAG_SHIFT`) |
  | 1–7, 24–30 | free for the application |
  | 31 | invalid in CMSIS-RTOS2 |

- **FreeRTOS backend:** the CMSIS-RTOS2 adapter implements thread flags with the **task notification at
  index 0**. Native FreeRTOS code that uses index-0 notifications on a CSP process's thread
  (`xTaskNotifyGive`, `ulTaskNotifyTake`, stream/message buffers, …) collides with CSP4CMSIS. Keep native
  notifications off CSP threads, or move them to another index
  (`configTASK_NOTIFICATION_ARRAY_ENTRIES > 1`).
- **`putFromISR()`** is only for ISRs at or below `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` (unchanged).
  The element copy runs with BASEPRI raised, so interrupt latency grows with `sizeof(T)`.
  **New:** `Chanout<T>::putFromISR()` has a `static_assert(sizeof(T) <= CSP4CMSIS_ISR_MAX_ELEMENT_SIZE)`
  (default 64 bytes; override with `-D`). Only code that calls `putFromISR()` is affected. For larger
  payloads, send an index into a static pool (pattern in `buffered_channel.h`).
- Override `csp4cmsis_fatal_error(const char*)` (weak) to log or reset. It must not return.
- **Where channels may be constructed** (namespace scope, function-local static; never in an ISR) and
  the RTX5 start-up-hook rule: `Documentation/CSP4CMSIS_Configuration.md`, section 5.

## Source/API changes (internal API: only code using `csp::internal` is affected)

- `csp::internal::BufferedChannel<T, P>(size_t capacity)` → `BufferedChannel<T, SIZE, P>` (default
  constructor). `CSP4CMSIS_BUFFERED_CHANNEL_API` is defined as `2`.
- `BaseAltChan<T>::getInputGuard(T&)` / `getOutputGuard(const T&)` →
  `getInputGuard(GuardSlot&, T&)` / `getOutputGuard(GuardSlot&, const T&)`.
  `Chanin/Chanout::getGuard()` are unchanged.
- `internal::Guard`: `activate()` returns `bool`; new virtual `confirm(bool)` (default `true`).
- `internal::AltScheduler`: event-flags object removed (`initForCurrentTask()` and `getEventGroupHandle()`
  are gone); new `ownerThread()`. `wakeUp(flag)` now sets a thread flag and must not be called inside a
  CSP critical section.
- Non-copyable: `TimerGuard`, `RelTimeoutGuard`, `BufferedChannel`, `SamplingBufferedChannel`,
  `AltScheduler` (and therefore `Alternative`).
- `Alternative`'s variadic constructor takes its bindings **by reference**, so a `RelTimeoutGuard` is never
  copied.
- New header `csp_fatal.h` (in the pdsc). `csp_rtos_static.h` adds `csp_static_timer_storage_t`.
- `OverwritingChannel` was removed: use `SamplingBufferedChannel<T, SIZE, BufferPolicy::KeepNewest>`.

## Not changed / still using the RTOS heap

These are outside the BufferedChannel work, and remain even with `CSP4CMSIS_STATIC_ALLOCATION`:
- the rendezvous `AltChanSyncBase` mutex (`osMutexNew(NULL)`);
- `SyncChannel` mutex and semaphores;
- `Barrier`;
- `run.h`'s thread control blocks are static only when `CSP4CMSIS_STATIC_ALLOCATION` is set (unchanged).

Rendezvous and signal-channel ALT guards still trust a wakeup (no re-verification; same as 1.0.0), and
still have a single ALT registration slot per direction. Only the buffered channel enforces the
one-ALTing-process rule.
