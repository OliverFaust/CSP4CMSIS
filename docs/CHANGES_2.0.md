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
| `select()` | used every set bit as the choice; disabled all guards | one-winner protocol with re-verification (**OWRV**): per-ALT state word (ENABLING/WAITING/CLAIMED); clears its bits per round; disables only the guards it enabled; **every wakeup is re-verified** (`disable()` reports readiness again). A rendezvous partner's claim wins; otherwise the first ready guard in fairness order; nothing ready → new round. If `activate()` fails (the partner went away or a competitor took the token), a new round starts. A failed wait is fatal instead of being used as a bit mask |
| RTOS calls inside CSP critical sections | `_notifyReader()`/`_notifyWriter()` called `osEventFlagsSet()` with BASEPRI raised; rendezvous `putFromISR()` too (removed in 2.0) | never: state is snapshotted inside the section, and the RTOS is called after leaving it |
| `RelTimeoutGuard` / `TimerGuard` | `osTimerNew()` from the RTOS heap on every construction | static control block under `CSP4CMSIS_STATIC_ALLOCATION`; creation checked |
| CSP critical section entry | `MSR BASEPRI_MAX` only | `MSR BASEPRI_MAX; DSB; ISB` (as the FreeRTOS Cortex-M ports); Cortex-M7 r0p1 (erratum 837070) additionally needs `CPSID i`/`CPSIE i` around the MSR, which is not done (no known target uses that core) |
| Failed RTOS object creation | silently ignored (e.g. a dead channel whose `input()` returned without data) | `csp4cmsis_fatal_error()` |
| ALT guard state | one guard object per channel direction, shared by all processes | one guard per channel-end **handle** (`Chanin`/`Chanout`), for rendezvous and buffered channels |
| Rendezvous channel | mutex-protected state; the ALT-vs-ALT partner copied the data and then woke the ALT, which trusted the wakeup (PHANTOM/SILENT, T15) | **OWRV** (`alt_channel_sync.{h,cpp}`): state under CSP critical sections, no mutex. A partner never completes a communication for an ALT: plain partners stay pending until the other side copies and releases them; ALT-vs-ALT pairs are claimed atomically (both state words in one section) and the reader side copies. Element copies run outside critical sections. Checked in `docs/formal/alt_owrv_extended.csp` |
| Signal channel | `SyncChannel` (own protocol; lost signals, T15s) | a **data-less rendezvous** (`csp::Signal`) on the same OWRV core; `SignalChannel::reader()` / `writer()` |

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
- **ISR writes only into buffered channels (breaking):** `Chanout<T>::putFromISR()` is gone. Take an
  ISR writer end from a buffered channel, `auto isr_out = chan.isrWriter();` (`IsrChanout<T>`), and
  call `isr_out.putFromISR(v)` in the ISR. Rendezvous and signal channels have no ISR path: an
  interrupt cannot wait for a partner. Migrate `Channel<T>` + `putFromISR()` to
  `BufferedChannel<T, 1>` (the reader code is unchanged, and an event is no longer lost when the reader
  is not yet waiting) or `SamplingBufferedChannel<T, 1, KeepNewest>` for a latest value.
  - Only for ISRs at or below `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` (unchanged). The element copy
    runs with BASEPRI raised, so `IsrChanout<T>` has `static_assert(sizeof(T) <=
    CSP4CMSIS_ISR_MAX_ELEMENT_SIZE)` (default 64 bytes; override with `-D`). For larger payloads, send
    an index into a static pool (pattern in `buffered_channel.h`).
- **Sampling policies only on buffered channels (breaking):** `SamplingChannel<T, KeepNewest|KeepOldest>`
  and `SignalChannel<KeepNewest|KeepOldest>` are compile-time errors; use
  `SamplingBufferedChannel<T, 1, P>`. (A rendezvous writer that must not block cannot hand a value to
  an ALT reader, which only takes it in its own `activate()`.)
- Override `csp4cmsis_fatal_error(const char*)` (weak) to log or reset. It must not return.
- **Where channels may be constructed** (namespace scope, function-local static; never in an ISR) and
  the RTX5 start-up-hook rule: `Documentation/CSP4CMSIS_Configuration.md`, section 5.

## Source/API changes (internal API: only code using `csp::internal` is affected)

- `csp::internal::BufferedChannel<T, P>(size_t capacity)` → `BufferedChannel<T, SIZE, P>` (default
  constructor). `CSP4CMSIS_BUFFERED_CHANNEL_API` is defined as `2`.
- `BaseAltChan<T>::getInputGuard(T&)` / `getOutputGuard(const T&)` →
  `getInputGuard(GuardSlot&, T&)` / `getOutputGuard(GuardSlot&, const T&)`.
  `Chanin/Chanout::getGuard()` are unchanged.
- `internal::Guard`: `activate()` returns `bool` (false = new round). `confirm()` does not exist: every
  wakeup is re-verified through `disable()`.
- `internal::AltScheduler`: one-winner state word (`claimableLocked()`, `claimLocked()`, …; used only
  inside CSP critical sections).
- `SignalChannel`: `getInternal()` and `SyncChannel` are gone; use `reader()`/`writer()` with
  `csp::Signal` (`out << csp::Signal{}`, `in >> s`, `in | s`).
- Rendezvous channels: `T` must be trivially copyable (`static_assert`); at most one ALTing process per
  channel end (fatal); any number of plain writers/readers, serialised per end.
- New header `csp_semaphore.h` (in the pdsc); `sync_channel.{h,cpp}` removed.
- `BaseAltChan<T>::putFromISR()` removed; buffered channels implement `internal::IsrSink<T>`.
  `CSP4CMSIS_ISR_WRITER_API` and `CSP4CMSIS_ALT_PROTOCOL_OWRV` are defined in 2.0 (feature tests).
- `internal::AltScheduler`: event-flags object removed (`initForCurrentTask()` and `getEventGroupHandle()`
  are gone); new `ownerThread()`. `wakeUp(flag)` now sets a thread flag and must not be called inside a
  CSP critical section.
- Non-copyable: `TimerGuard`, `RelTimeoutGuard`, `BufferedChannel`, `SamplingBufferedChannel`,
  `AltScheduler` (and therefore `Alternative`).
- `Alternative`'s variadic constructor takes its bindings **by reference**, so a `RelTimeoutGuard` is never
  copied.
- New header `csp_fatal.h` (in the pdsc). `csp_rtos_static.h` adds `csp_static_timer_storage_t`.
- `OverwritingChannel` was removed: use `SamplingBufferedChannel<T, SIZE, BufferPolicy::KeepNewest>`.

## RTOS heap

- `run.h`'s thread control blocks are static only when `CSP4CMSIS_STATIC_ALLOCATION` is set (unchanged).
- `Barrier` (2.0): count and phase under a CSP critical section, two semaphores alternating by phase,
  static control blocks, so no RTOS heap. This also fixes two 1.x bugs: the last arrival released N
  tokens for N - 1 waiters (the next phase's first arrival passed straight through), and it reset the
  count outside the lock. FVP test T18 fails on 1.0.0 and passes on 2.0.
- **Under `CSP4CMSIS_STATIC_ALLOCATION` CSP4CMSIS makes no dynamic RTOS allocation at all.** The harness
  proves it with builds that have RTOS dynamic allocation disabled (`tests/fvp_sse300/README.md`,
  "Heap-free proof").

Rendezvous and signal channels use no RTOS heap under `CSP4CMSIS_STATIC_ALLOCATION`: their only RTOS
objects are two counting semaphores per channel (one per end, serialising plain operations), with
static control blocks.
