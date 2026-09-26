# BufferedChannel analysis: correctness issues and a static-allocation design

Branch `buffered-channel-analysis`, based on `main` = `v1.0.0` = `a789d2a`. `main`, the tag and
`origin/main` were all identical when checked (after a fetch) on 2026-09-26. Everything is local and
nothing has been pushed.

**Scope.** Step 1 removes `OverwritingChannel`, and that is the only library change. There is also
one behaviour-neutral warning fix, needed for the required `-Wall -Wextra`-clean build. Steps 2–3
are analysis only. The tests are in `tests/fvp_sse300/`.

| Commit | Content |
|---|---|
| `ca0297b` | Step 1: remove `OverwritingChannel` (header, pdsc `<file>` entry and description, README) |
| `5c6bcbf` | `rendezvous_channel.h`: unused-parameter warning in the empty `beginExtInput()`, pre-existing in v1.0.0; the parameter name is commented out |
| *(this commit)* | This document, `tests/fvp_sse300/` (tests, README, reference output) |

---

## Summary of verdicts

| # | Suspected issue | Verdict | Evidence |
|---|---|---|---|
| 1 | Lost wakeup in ALT `enable()`, input and output guards | **Confirmed** (both) | T1a, T1b: FVP hangs at specific preemption points; reproducible |
| 2 | RTOS call (`wakeUp` → `osEventFlagsSet`) inside the BASEPRI critical section | **Confirmed; the behaviour depends on core and backend** | Source quotes; T2: BASEPRI 0xA0 → 0x00 inside the section, and a context switch inside it |
| 3 | KeepNewest "drop oldest, put newest" is not atomic | **Confirmed** | T3: the victim's newest value is lost, *and* a stale value survives (`{2, 200}` instead of `{100, 200}`) |
| 4 | Single writer registration slot | **Confirmed**; no example relies on it | T4a (last registrant wins), T4b (an unrelated ALT wipes the slot) |
| 5 | `osMessageQueueNew()` failure unchecked | **Confirmed** | T5: blocking `input()` returns at once and leaves `dest` unwritten; `output()` silently drops |
| 6 | `TimerGuard` allocates per construction | **Confirmed** (72 B of heap each, even with `STATIC_ALLOCATION`); no example constructs one per loop iteration | T6; usage survey |
| 7 | Other findings | 7a–7l below; 7a, 7b and 7c are confirmed by tests | T7a, T4b, T4c |

All tests: `tests/fvp_sse300/results/2026-09-26_fvp_run.txt`.

---

## Test setup (details in `tests/fvp_sse300/README.md`)

- **Harness:** `helloworld_sse300`, local branch `csp4cmsis-wt-tests` of the FVP repository. It is never
  pushed, and its `origin` is not ours.
- **Target:** Corstone-300 FVP (Cortex-M55, Armv8.1-M), AC6 6.24, CMSIS-RTOS2 over FreeRTOS 11.3.0
  (`ARM::CMSIS-FreeRTOS`).
- **Configuration:** `CSP4CMSIS_STATIC_ALLOCATION`, `CSP4CMSIS_RTOS2_BACKEND_FREERTOS`,
  `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5` (BASEPRI 0xA0).
- **Build against this working tree:** the harness's `hello.cproject.yml` loads the pack by path,
  `- pack: OliverFaust::CSP4CMSIS` / `path: <relative path to ~/src/CSP4CMSIS>`. csolution reads
  `OliverFaust.CSP4CMSIS.pdsc` straight from this clone, so the `OliverFaust::CSP4CMSIS:Core` sources
  come from the working tree. The installed 1.0.0 pack is **not** used, and nothing is registered with
  `cpackget`, so the same pack version never appears twice. I verified this through
  `compile_commands.json`.
- **Build result:** clean, 0 warnings with `-Wall -Wextra` over all CSP4CMSIS sources, `hello.c` and
  the test file, after the `5c6bcbf` fix. Image with Step 1 applied: byte-for-byte the same section
  sizes as v1.0.0 (Code 52,428 / RO 5,540 / RW 52 / ZI 45,736), as expected for an unused header.
- **Determinism:**
  - Two FVP runs of the same image produce **identical output** and the same instruction count
    (6,371,861,157).
  - Within one run, repeated sweeps shift the exact hit `k` values by a few iterations, because
    per-trial state such as queue ring indices differs. Every sweep hits the window.
- **Timing:** the FVP tick is about 312.5 Hz, not 100 Hz (pre-existing clock mismatch). The tests use
  tick counts only.
- **Harness bug (fixed, recorded for honesty):** one intermediate test build hung in T1a. The runner's
  2 KB stack overflowed once the sweep bookkeeping grew; it is now 8 KB with 6,760 B minimum free. That
  was a test defect, not a library issue.

---

## Step 1: OverwritingChannel removed

`overwriting_channel.h` defined `OverwritingChannel<T> : BufferedChannel<T>` with a runtime capacity:
- It overrode only `output()`, so `space_available()`, `putFromISR()` and the output guard kept Block
  semantics.
- It duplicated `BufferPolicy::KeepNewest`, including the non-atomic drop/put (issue 3).
- **Nothing included it:** not `csp4cmsis.h`, nor any other header, source or doc.

Removed:
- the header;
- its `<file>` entry in `OliverFaust.CSP4CMSIS.pdsc`, with the component description now naming the
  buffered policies;
- the line in `csp4cmsis/README.md`.

The prebuilt `OliverFaust.CSP4CMSIS.1.0.0.pack` is the released artefact and is left unchanged. It
still contains the header.

**Sibling repositories** (read-only; shallow clones of the GitHub repos plus the local copies; not
modified):
- There are **no uses** of the class.
- `CSP4CMSIS-Nucleo`'s *own vendored, older* `public_channel.h` has `#include "overwriting_channel.h"`.
  Its copy is self-contained, so this change does not affect it.
- Several vendored copies of `csp4cmsis/README.md` mention the header in prose.

Repositories covered:
- Alif-DK-E8-CSP4CMSIS
- HimaxWE2-CSP4CMSIS
- CSP4CMSIS-Nucleo
- CSP4CMSIS_for_NUCLEO-G474RE
- CSP4CMSIS-B-L475E-IOT01A
- Static_Process_Networks: there is no standalone GitHub repository, so I used the local copy in
  `oliverfaust.github.io`, plus `The_Way_of_Static_Process_Networks/GithubCode`.

---

## Step 2: issues

### Method for the race tests (T1a, T1b, T3)

The runner is the higher-priority aggressor:
1. It aligns to a tick edge E0, releases a lower-priority victim, and sleeps until the next edge E1.
2. The victim spins `k` iterations and then executes the operation under test.
3. At E1 the aggressor preempts the victim wherever it is and does the conflicting operation.
4. A binary search finds the `k` boundary between "aggressor acted after the victim's operation" and
   "before it". A step-1 sweep of `k` from `kb−1500` to `kb+50` then places the preemption at every
   instruction offset of the victim's operation. One iteration is about 10 cycles: about 320,000 cycles
   per tick divided by `kb` ≈ 31,800.
5. The victim publishes a stage marker. **Every BUG hit below was taken with the victim at stage 2,
   inside the operation under test.**
6. A hang counts as a lost wakeup only if the channel really had data or space when the ACK timed out
   (30 ticks). A single successor operation then rescues the victim. That demonstrates the "final
   message with no successor" hang without blocking the rest of the run.

### 1. Lost wakeup in ALT enable: **CONFIRMED** (input and output guards)

`buffered_channel.h`:

```cpp
bool enable(AltScheduler* alt, uint32_t bit) override {      // BufferedInputGuard, l.186
    if (channel->pending()) return true;                     // (a) check
    channel->registerInputAlt(alt, bit);                     // (b) register
    return false;
}
void _notifyReader() {                                       // l.34 -- called by output()
    uint32_t saved = csp_enter_critical();
    if (alt_reader != nullptr) alt_reader->wakeUp(read_bit); // nullptr between (a) and (b)
    csp_exit_critical(saved);
}
```

A `put` between (a) and (b) finds `alt_reader == nullptr` and sends no wakeup. The ALT then waits in
`osEventFlagsWait(…, osWaitForever)` with data in the queue. `BufferedOutputGuard::enable()` (l.209)
has the same check-then-register shape with `space_available()` and `_notifyWriter()`.

| Test | Sweep 1 | Sweep 2 (same run) | Window |
|---|---|---|---|
| **T1a**: ALT reader vs higher-priority `output()` | 6 BUG trials among 1551, `k`=31782…31788 | 6 BUG trials | ≈ 6 iterations ≈ 60–70 cycles |
| **T1b**: ALT writer on a full channel vs higher-priority `input()` | 7 BUG trials, `k`=31782…31795 | 6 BUG trials | ≈ 60–70 cycles |

Each BUG trial is a real hang: no ACK for 30 ticks, with data queued (T1a) or space free (T1b). One
successor message or read releases it.

**How reliably:**
- **On the FVP, fully reproducibly.** The same image gives the same hits run after run, and every sweep
  hits.
- **On hardware, the window is small but reachable.** It is reached whenever a higher-priority writer
  (or an ISR using `putFromISR()`, which takes the same route through `wakeUp`) becomes ready inside
  these ~60–70 cycles of the reader's `select()`. For a writer that wakes at a random point in each
  tick, that is about 2×10⁻⁴ per ALT entry. That is an estimate, not a measurement.
- The failure is silent. It shows up as a stall that lasts until the *next* message, and forever if no
  further message comes: the last sample of a burst, or a shutdown command.

### 2. RTOS call inside the BASEPRI critical section: **CONFIRMED; the behaviour depends on core and backend**

`_notifyReader()` and `_notifyWriter()` call `wakeUp()`, and so `osEventFlagsSet()`, with BASEPRI
raised to 0xA0. Whether the RTOS takes its ISR path depends on how it detects "interrupts masked".

**ARM::CMSIS-FreeRTOS 11.3.0, `CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.c`:**

```c
#ifndef __ARM_ARCH_8M_MAIN__
  #define __ARM_ARCH_8M_MAIN__    0
#endif
...
#if   ((__ARM_ARCH_7M__      == 1U) || \
       (__ARM_ARCH_7EM__     == 1U) || \
       (__ARM_ARCH_8M_MAIN__ == 1U))
#define IS_IRQ_MASKED()           ((__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U))
#elif  (__ARM_ARCH_6M__      == 1U)
#define IS_IRQ_MASKED()           (__get_PRIMASK() != 0U)
...
#else
#define IS_IRQ_MASKED()           (__get_PRIMASK() != 0U)
#endif

__STATIC_INLINE uint32_t IRQ_Context (void) {   // l.172
  ...
  if (IS_IRQ_MODE()) { irq = 1U; }
  else {
    state = xTaskGetSchedulerState();
    if (state != taskSCHEDULER_NOT_STARTED) {
      if (IS_IRQ_MASKED()) { irq = 1U; }
    }
  }
  return (irq);
}

uint32_t osEventFlagsSet (osEventFlagsId_t ef_id, uint32_t flags) {   // l.1533
  ...
  else if (IRQ_Context() != 0U) {
    ...
    if (xEventGroupSetBitsFromISR (hEventGroup, (EventBits_t)flags, &yield) == pdFAIL) {
      rflags = (uint32_t)osErrorResource;
    } else { ... portYIELD_FROM_ISR (yield); }
  }
  else {
    rflags = xEventGroupSetBits (hEventGroup, (EventBits_t)flags);   // l.1560
  }
```

- **Cortex-M55 (Armv8.1-M): the thread path is taken.** The compiler defines `__ARM_ARCH_8_1M_MAIN__`,
  not `__ARM_ARCH_8M_MAIN__`, so `IS_IRQ_MASKED()` checks PRIMASK only. The linked image confirms it:
  `IRQ_Context` reads `IPSR` and `PRIMASK` and never BASEPRI.
  - `xEventGroupSetBits()` calls `vTaskSuspendAll()` and then `xTaskResumeAll()`.
  - `xTaskResumeAll()` calls `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`, i.e. `vPortExitCritical()`.
    When the nesting count reaches 0, that runs `portENABLE_INTERRUPTS()` = `vClearInterruptMask(0)`,
    which executes `msr basepri, r0` with r0 = 0.
  - **T2**, which reproduces the `_notifyReader()` sequence exactly: BASEPRI is 0xA0 before `wakeUp()`
    and **0x00 after it, still inside the section**. A higher-priority thread waiting on the flags
    **ran before `csp_exit_critical()`**.
  - Consequences: the CSP critical section silently ends early, and the scheduler can switch threads
    inside it. Any code placed after a `wakeUp()` in a critical section, or an enclosing (nested)
    critical section, is unprotected.
- **Cores where the macros match (M3, M4, M7, M33 on FreeRTOS): the ISR path is taken from task
  context.**
  - `xEventGroupSetBitsFromISR()` does not set the bits. It *defers* the operation to the timer daemon:
    `xTimerPendFunctionCallFromISR(vEventGroupSetBitsCallback, …)` → `xQueueSendFromISR(xTimerQueue, …)`
    (`event_groups.c`, `timers.c`).
  - The flags are therefore set only when the daemon runs (`configTIMER_TASK_PRIORITY`).
  - **If the timer command queue is full** (`configTIMER_QUEUE_LENGTH`, 10 in the harness), the call
    returns `pdFAIL`, so `osEventFlagsSet` returns `osErrorResource`. `AltScheduler::wakeUp()` ignores
    the return value, so the wakeup is **lost silently**.
- **CMSIS-RTX 5.9.1** (source read from the Alif pack root; not run here):

  ```c
  __STATIC_INLINE bool_t IsIrqMasked (void) {            // rtx_core_cm.h l.155
  #if   ((defined(__ARM_ARCH_7M__)        && (__ARM_ARCH_7M__        != 0)) || \
         (defined(__ARM_ARCH_7EM__)       && (__ARM_ARCH_7EM__       != 0)) || \
         (defined(__ARM_ARCH_8M_MAIN__)   && (__ARM_ARCH_8M_MAIN__   != 0)) || \
         (defined(__ARM_ARCH_8_1M_MAIN__) && (__ARM_ARCH_8_1M_MAIN__ != 0)))
    return ((__get_PRIMASK() != 0U) || (__get_BASEPRI() != 0U));
  ...
  uint32_t osEventFlagsSet (osEventFlagsId_t ef_id, uint32_t flags) {   // rtx_evflags.c l.675
    ...
    if (IsException() || IsIrqMasked()) {
      event_flags = isrRtxEventFlagsSet(ef_id, flags);
    } else {
      event_flags =  __svcEventFlagsSet(ef_id, flags);
    }
  ```

  - With BASEPRI ≠ 0 on every Mainline core, including the M55, `isrRtxEventFlagsSet()` runs. It sets
    the flags **immediately** and calls `osRtxPostProcess()`, which queues the object in the ISR FIFO
    (`OS_ISR_FIFO_QUEUE`, default 16) and pends PendSV.
  - PendSV is masked until `csp_exit_critical()`. The thread wakeup happens then, in post-processing.
  - On FIFO overflow: `osRtxKernelErrorNotify(osRtxErrorISRQueueOverflow, …)`. The template's weak
    `osRtxErrorNotify()` (`RTX_Config.c` l.39) ends in `for (;;) {}`, i.e. the **system halts**.
  - RTX is the best-behaved backend here (no lost bits), but only by relying on its ISR path.
- **Conclusion.** CSP4CMSIS must not call RTOS APIs inside `csp_enter_critical()` sections. It should
  capture what to wake inside the section and call the RTOS after `csp_exit_critical()`; see the design
  notes below.
- The same rule applies to `rendezvous_channel.h` `putFromISR()`. It is fine from real ISRs, where
  IPSR ≠ 0, but not from task context.

### 3. KeepNewest atomicity: **CONFIRMED**

```cpp
// output(), KeepNewest (buffered_channel.h l.128); putFromISR() l.93 has the same shape
if (osMessageQueuePut(queue_handle, source, 0, 0) != osOK) {
    T dummy;
    osMessageQueueGet(queue_handle, &dummy, NULL, 0); // drop oldest -> one slot free
    osMessageQueuePut(queue_handle, source, 0, 0);    // <- a concurrent writer may already own that slot
}
```

**T3 setup:** capacity 2, full with `{1, 2}`. The victim writes 100 and the higher-priority aggressor
writes 200. Every serial order leaves `{100, 200}` in some order.

**Result:** 22 and 23 BUG trials over the two sweeps (`k`=31772…31795; window ≈ 220 cycles). The test
prints the surviving content for the first six failing trials, and in all six the reader sees
**`2 200`**:
- the victim's newest value is lost;
- the victim had already dropped `1`, so the stale `2` survives instead of the newest data;
- the final `put` fails silently, because its return value is ignored.

**Who can race:**
- **Task–task:** any two writers, e.g. `BufferedAny2OneChannel<…, KeepNewest>`.
  - This pattern is in real use: the Himax KWS apps have several producer processes writing one
    `SamplingBufferedChannel<KwsReportMsg, 8, KeepNewest>`.
- **Task and ISR:** an ISR `putFromISR()` between the task's `Get` and `Put` has the same effect. The
  reverse direction is safe, because a task cannot preempt an ISR.
- **Writer and reader:** if the reader drains between the writer's failed `Put` and its `Get`, the
  writer drops an element although the queue is no longer full. That is an unnecessary loss; the
  newest value is still stored.

**Requirements on `T`:**
- `T dummy;` requires a **default-constructible** `T`.
- It costs `sizeof(T)` of stack in the calling context. For `putFromISR()` that is the **ISR/MSP
  stack**, i.e. 1 KB for a 1 KB message.
- Because RTOS queues copy with `memcpy`, `T` must also be **trivially copyable**. Nothing enforces
  this (no `static_assert`).

### 4. Single writer registration slot: **CONFIRMED**; no existing example relies on it

`alt_writer` and `write_bit` are single fields (l.27–28). `registerOutputAlt()` overwrites them, and
`unregisterOutputAlt()` clears them without checking who registered.

- **T4a:**
  1. Capacity 1, full. ALT writers A (sending 10) and B (sending 20) both block. B registered last.
  2. The reader drains once, and only B is woken.
  3. B's `disable()` then clears the slot.
  4. The reader drains again. **A is never woken, although space is free** (result: `reader got 1 then
     20; writer A done=0`).
- **T4b** is worse than "last registrant wins". A is registered. An unrelated process runs
  `ALT{instant timeout, out | b}`; the timeout guard is ready in phase 1, so the output guard is
  **never enabled**. But `select()` calls `disable()` on **every** guard (`alternative.cpp` l.107),
  which wipes A's registration. A then misses the next free slot.

**Usage survey:**
- No example in this repository or the siblings ALTs on a buffered channel's **output**. In fact no
  example ALTs on a buffered channel at all.
- Every `Alternative` found multiplexes **inputs** of rendezvous `Channel`s.
- The only output-guard ALT (Himax `csp4cmsis_alt_alt_test`) uses a rendezvous `Channel`.

### 5. Unchecked queue handle: **CONFIRMED**

`BufferedChannel(size_t)` (l.52) stores the result of `osMessageQueueNew()` without checking it.

**T5:** capacity 20,000 × 4 B is larger than the 32 KB heap, so the handle is `NULL`. Then:
- `input()` (Block, "wait forever") **returns after 0 ticks with `dest` unwritten** (0xDEADBEEF);
- `output()` returns as if the message were sent;
- `pending()` and `space_available()` both return 0.

The adapter returns `osErrorParameter` for a NULL handle, and `input()`/`output()` only check for
`osOK` before notifying. So the error is swallowed.

With static allocation, failure becomes deterministic (wrong `cb_size`, misalignment on RTX, object
creation before `osKernelInitialize()` on RTX; see open questions), but it still needs a check.

### 6. TimerGuard allocation: **CONFIRMED** heap per construction; typical usage is not per iteration

`TimerGuard::TimerGuard()` calls `osTimerNew(…, NULL)` and `~TimerGuard()` calls `osTimerDelete()`
(`alternative.cpp` l.132–140). No static control block is supported: `csp_rtos_static.h` has no timer
storage type.

**T6:**
- A `RelTimeoutGuard` takes **72 B** from the RTOS heap while alive (`StaticTimer_t` 44 B plus heap_4
  overhead) and returns it on destruction.
- **1000 constructions in a loop means 1000 heap allocations**, with `CSP4CMSIS_STATIC_ALLOCATION`
  defined.
- An `Alternative` itself takes 0 B of heap (its event flags are static).

**Usage in docs and examples:**
- Only Himax `csp4cmsis_alt_alt_test/tests.cpp` uses timeouts: three `RelTimeoutGuard`s as
  **block-scope locals, once per test phase**, each with its own `Alternative`. There is none per loop
  iteration anywhere.
- Every other example constructs its `Alternative` **once, before the loop**. This includes the FVP
  demo, Nucleo, Alif, Himax `alt_test`/`comstime`/`kws_PCA9685_alt`.
- The natural "`RelTimeoutGuard t(ms); Alternative a(in | x, t);` inside the loop" pattern would
  allocate and free a timer on every iteration. That is runtime heap use and fragmentation risk with
  heap_2/heap_4.
- The Himax test also documents a copy hazard: the variadic `Alternative(Bindings...)` takes bindings
  by value. Passing a `RelTimeoutGuard` there copies it, so the copy shares `timer_handle` (a double
  `osTimerDelete`), and its `internal_guard_ptr` still points into the original.

### 7. Other findings (buffered_channel.h, alt.h/alternative.cpp, csp_critical.h)

| # | Finding | Evidence | Effect |
|---|---|---|---|
| 7a | **`BufferedInputGuard::activate()` does not notify writers.** It does `osMessageQueueGet(…, 0)` and never calls `_notifyWriter()`, unlike `input()`. | **T7a**: control (reader uses `input()`): writer woken = 1; reader uses ALT: **writer woken = 0** | An ALT writer blocked on a full channel is never woken by an ALT reader. That is a deterministic hang, not a race. |
| 7b | **`select()` disables guards that were never enabled** (l.107), and `unregister*Alt()` clears registrations unconditionally | **T4b** | One process's ALT can cancel another process's registration on a shared channel end. |
| 7c | **One guard object per channel end.** `getOutputGuard(src)` re-targets the shared `res_out_guard` (`setTarget(&src)`), even while another writer's ALT is blocked on it. | **T4c**: writer A (sending 10) blocked; another writer merely builds its guard with `b`=20; the reader drains and **receives 20 from A's ALT** | Silent data corruption with several writers. The same applies to `res_in_guard` if one channel end is used in two ALTs. |
| 7d | `select()` does not check the result of `osEventFlagsWait()` for errors (`osFlagsError*` = 0xFFFFFFFx) | Code: `fired` is used directly to compute `selected` (l.94–101) | If the event-flags object is invalid (e.g. `osEventFlagsNew` failed without static allocation), `selected` comes from error bits and `guardArray[selected]->activate()` can index **past `amount`**. |
| 7e | `AltScheduler::wakeUp()` ignores `osEventFlagsSet()`'s return value | Code l.126; see item 2 | Lost wakeups on the FreeRTOS ISR path (timer queue full) are silent. |
| 7f | ISR-context detection differs by backend and core (item 2) | Source | The same library code has three different behaviours inside a critical section. |
| 7g | `putFromISR()` reads `alt_reader` without a critical section | Code l.106 | Safe only for ISRs at or below `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` (task-side writes are under BASEPRI). Undocumented. |
| 7h | `BufferedOutputGuard::activate()` calls `output()`, which for Block waits with `osWaitForever` | Code l.221 | If another writer takes the slot between `disable()` and `activate()` (Any2One), the ALT **blocks inside its commit phase**. |
| 7i | `BufferedChannel` / `SamplingBufferedChannel` are **copyable** | Code: implicit copy constructor | A copy double-deletes the queue handle, and the copy's guards point to the original (`res_in_guard(this)`). |
| 7j | `T` requirements are not enforced (see item 3): trivially copyable, and default-constructible for KeepNewest | Code | Undefined behaviour for non-trivial `T`. |
| 7k | `KeepOldest` `output()` calls `_notifyReader()` even when the message was dropped | Code l.136 | Harmless: the queue is full, so the reader has data anyway. It is still an RTOS call inside the critical section (item 2). |
| 7l | Channels as namespace-scope statics call `osMessageQueueNew()` during C++ static initialisation, before `osKernelInitialize()` (Himax KWS apps) | Usage | Works on the FreeRTOS adapter. **Correction (review round 2, T14):** also works on RTX5, where CMSIS-RTX calls `osKernelInitialize()` before C++ static constructors. My original expectation of a NULL return was wrong. |

---

## Step 3: static BufferedChannel, design comparison (analysis only)

The public `SamplingBufferedChannel<T, SIZE, P>` already carries `SIZE`. Only the internal
`BufferedChannel<T, P>(size_t)` takes a runtime capacity, and it would become
`BufferedChannel<T, SIZE, P>`.

**Sizes used below:**
- Compiled on this target: FreeRTOS with this project's `FreeRTOSConfig.h`; RTX5 5.9.1 with its
  template `RTX_Config.h`.
- The v1.0.0 channel object is **48 B** for any `T` (vptr, queue handle, 2 registrations, 2 guards).
- An `Alternative` is 112 B.

| Object | FreeRTOS 11.3.0 | RTX5 5.9.1 |
|---|---|---|
| Message queue control block | `StaticQueue_t` 80 B; `cb_size >=` | `osRtxMessageQueue_t` 52 B; `cb_size` must be **exactly** this |
| Message queue storage | `mq_size >= SIZE*sizeof(T)` (adapter check) | `osRtxMessageQueueMemSize(SIZE, sizeof(T))` = `4*SIZE*(3+⌈sizeof(T)/4⌉)` (12-byte header per message); `mq_mem` 4-byte aligned |
| Semaphore control block | `StaticSemaphore_t` 80 B | `osRtxSemaphore_t` 16 B |
| Mutex / event flags / timer | 80 / 32 / 44 B | 28 / 16 / 32 B |

I verified both sets of sizing rules against the installed sources: adapter `osMessageQueueNew()`
checks `attr->cb_size >= sizeof(StaticQueue_t) && attr->mq_size >= msg_count * msg_size` and then calls
`xQueueCreateStatic`. RTX `rtx_msgqueue.c` computes
`block_size = ((msg_size + 3U) & ~3UL) + sizeof(os_message_t)` and requires
`cb_size == sizeof(os_message_queue_t)` and `mq_size >= msg_count * block_size`.

### Option A: keep `osMessageQueue`, with static `cb_mem`/`mq_mem`

Add backend-specific queue control-block and storage types to `csp_rtos_static.h`. On RTX the storage
size must use the 12-byte-header formula, on FreeRTOS `SIZE*sizeof(T)`.

### Option B: own ring buffer

- Storage: `alignas(T) unsigned char buf[SIZE*sizeof(T)]`, with `head`/`tail`/`count` updated only
  inside `csp_enter_critical()`.
- Blocking: two counting semaphores, "items" (starts at 0) and "slots" (starts at SIZE). They are
  static, using the **existing** `csp_static_semaphore_storage_t`.
- KeepNewest overwrite becomes a single update inside the critical section.
- A variant **B′** blocks with thread flags plus waiter registration instead of semaphores. It needs no
  RTOS objects, but Any2One requires a waiter list, which adds verification effort.

### Fixes that are the same in both options

These are independent of the storage choice:
- **Item 1:** register first, then check (`registerInputAlt(); return pending();`).
  - A put either sees the registration or is seen by the check, so nothing is lost; at worst there is a
    spurious wakeup, which `select()` tolerates.
  - This does *not* need an RTOS call inside the critical section. In A that matters: calling
    `osMessageQueueGetCount()` inside a BASEPRI section on this core would clear BASEPRI again,
    because the FreeRTOS thread path uses `taskENTER/EXIT_CRITICAL`.
- **Item 2:** never call the RTOS inside `csp_enter_critical()`. Capture the target inside the section
  and signal after `csp_exit_critical()`.
  - This needs a lifetime rule for the captured ALT, because `Alternative` objects can be
    block-scoped. The simplest robust option is to signal the waiting **thread**
    (`osThreadFlagsSet(tid, bit)`) instead of a per-`Alternative` event-flags object: the thread
    outlives the ALT.
  - That also removes the per-`Alternative` event group (32 B / 16 B static), and `select()` clears its
    own bits on entry.
  - This is a change to `alt.h`/`alternative.cpp` and applies to both options.
- **Items 4 and 7b:** a registration set (e.g. a small fixed array or bitmask of `{tid, bit}`, or a
  one-writer-ALT rule enforced by an assert). `unregister` removes only its *own* entry.
- **Item 7c:** guard state per binding or ALT, not per channel end.
- **Item 7a:** ALT `activate()` on input notifies writers.
- **Items 5 and 6:** check handles and assert at construction; add static timer storage
  (`StaticTimer_t` / `osRtxTimer_t`) to `csp_rtos_static.h`.
- **Item 7i:** delete copy and move constructors of the channel classes.
- **Item 7j:** `static_assert(std::is_trivially_copyable_v<T>)`.

### Comparison

| | **A: osMessageQueue, static** | **B: ring buffer + 2 static semaphores** |
|---|---|---|
| Item 1 (enable race) | Fixed by register-then-check | Fixed; either register-then-check or check and register in one critical section with no RTOS call, since `count` is a plain field |
| Item 2 (RTOS in critical section) | Fixed by signalling after the section. KeepNewest still cannot be made atomic without an RTOS call under a lock (see item 3). | Fixed. The critical section touches only `buf`/indices; semaphore release and signalling happen afterwards. |
| **Item 3 (KeepNewest)** | **Not fixable in general.** Two queue operations. A mutex around them fixes task–task (+80 B FreeRTOS / +28 B RTX), but an ISR cannot take a mutex, so task↔ISR stays racy. Raising BASEPRI around the two calls breaks on FreeRTOS/M55 (item 2). | **Fixed.** One critical section: if full, advance `tail` and overwrite in place; `count` and the semaphores are unchanged. ISR-safe by construction. |
| `T dummy` | Still needed for KeepNewest (default-constructible `T`, `sizeof(T)` on the task or ISR stack) | Not needed |
| Item 5 | Static creation can still fail (RTX `cb_size`/alignment/kernel state): needs a check | Only semaphore creation, static; needs a check |
| Backend-specific code | Queue CB types **and** two storage-size formulas | Semaphore CB type only; it already exists in `csp_rtos_static.h` |
| Memory, `T`=`uint32_t`, SIZE=8, FreeRTOS | 48 + 80 + 32 = **160 B** | ≈64 + 32 + 2×80 = **≈256 B** |
| Memory, `T`=`uint32_t`, SIZE=8, RTX5 | 48 + 52 + 128 = **228 B** | ≈64 + 32 + 2×16 = **≈128 B** |
| Memory, 1 KB struct, SIZE=8, FreeRTOS | 48 + 80 + 8,192 = **8,320 B** | ≈64 + 8,192 + 160 = **≈8,416 B** |
| Memory, 1 KB struct, SIZE=8, RTX5 | 48 + 52 + 8,288 = **8,388 B** | ≈64 + 8,192 + 32 = **≈8,288 B** |
| ISR safety | Queue put/get from ISR is supported by both backends. KeepNewest task-vs-ISR is racy. | All index updates run under BASEPRI. An ISR never blocks: it uses `osSemaphoreAcquire(slots, 0)` or overwrite, and releases "items" after the section. |
| Interrupt latency | FreeRTOS copies `T` inside its own critical section (`prvCopyDataToQueue` under `taskENTER_CRITICAL`); RTX copies in SVC/ISR context | Copies `T` under BASEPRI, which is comparable to A on FreeRTOS. For large `T`, a later reserve/copy/commit refinement is possible. |
| Verification effort | Less new code, but correctness still rests on the glue (notification, registration, KeepNewest), which is exactly where every bug in Step 2 lives. Queue internals are opaque to a model. | About 150 lines of new, self-contained code. Every operation is one short critical section plus a semaphore op. The existing sweep harness (T1/T3) can be reused, and each operation maps 1:1 to a model transition. |
| Fit with a CSP-M bounded buffer | Weak. `BUFF(s)` with KeepNewest `in?x → BUFF(tail(s)^⟨x⟩)` is one event, but A implements it as two, and the intermediate state is visible, which is what T3 found. | Direct. Buffer state (`s`, `#s`) is explicit, each critical section is one CSP event, and Block uses the semaphores as the `#s < N` / `#s > 0` guards. |
| API impact | Internal `BufferedChannel<T, SIZE, P>`; delete copy/move. Public API unchanged. | Same. Raw storage removes the default-constructible requirement. Public API unchanged. |

"≈" for B is my estimate of the object layout: v1.0.0's 48 B minus the queue handle, plus `head`,
`tail`, `count` and two semaphore IDs. It is not compiled yet.

### Recommendation: **B** (ring buffer under the CSP critical section, two static counting semaphores), together with the storage-independent fixes above

Reasons:
1. B is the only option that fixes **item 3** for all writers, including ISRs. KeepNewest is the policy
   that real applications use with several producers (Himax KWS reporter channels).
2. B contains no RTOS call inside a critical section **by construction**, which removes the whole
   class of item-2 problems for this component. A keeps two RTOS operations that must be made atomic.
3. It maps directly to a CSP-M bounded-buffer model, which fits the project's formal-verification
   direction.
4. Backend-specific code shrinks to one existing type. There is no RTX-specific storage formula to get
   wrong.
5. The memory cost is comparable. B is about 100 B larger per channel on FreeRTOS, because FreeRTOS
   semaphores are 80 B each, and smaller on RTX. **B′** (thread flags) would remove the semaphores if
   the FreeRTOS overhead matters, at the price of a waiter list for Any2One.

**What A would still require:** a mutex, and accepting that KeepNewest task↔ISR remains racy, or
forbidding `putFromISR()` on KeepNewest channels.

---

## How to rerun

See `tests/fvp_sse300/README.md`. In short:

```sh
cd <arm_fvp_helloworld>/helloworld_sse300 && git switch csp4cmsis-wt-tests && source ../env.sh
cbuild hello.csolution.yml --packs --toolchain AC6 --rebuild
$FVP_BIN_DIR/FVP_Corstone_SSE-300_Ethos-U55 -a out/MPS3-Corstone-300/hello.axf \
    -C ethosu.num_macs=128 -f model_config_sse300.txt --simlimit 900 --stat
```

Compare the output with `tests/fvp_sse300/results/2026-09-26_fvp_run.txt`.

## Open questions

1. **A vs B**: your decision; I recommend B. If B, should it use semaphores or thread flags (B′)? B′
   needs a waiter set for Any2One writers.
2. **ALT signalling:** is switching `AltScheduler` from per-`Alternative` event flags to thread flags
   acceptable? It fixes the lifetime problem of signalling outside the critical section and saves the
   event group. It needs a reserved thread-flag bit range, because `rendezvous_channel.h` already uses
   `RENDEZVOUS_FLAG`.
3. **Multiple ALT writers (item 4):** support them with a registration set, or restrict to one ALT
   writer per channel with an assert? No current example needs them.
4. **Scope of `STATIC_ALLOCATION`:** should it also cover `RelTimeoutGuard` (static timer storage) and
   the channel mutexes and semaphores in `sync_channel.cpp`, `alt_channel_sync.cpp` and `barrier.cpp`?
   These are outside this task but needed for "genuinely heap-free".
5. **RTX:** not built or run here; items 2 and 7l are source-based for RTX. Should an RTX5 variant of
   the harness be added (CMSIS-RTX 5.9.1 is available in the Alif pack root) before implementing?
6. **Large `T` under BASEPRI in B:** is copying 1 KB with BASEPRI raised acceptable (about the same as
   FreeRTOS's own queue copy), or should B use reserve/copy/commit from the start?
7. **Upstream report:** the adapter's missing `__ARM_ARCH_8_1M_MAIN__` in `IS_IRQ_MASKED()` (item 2) is
   an ARM::CMSIS-FreeRTOS issue on Armv8.1-M. Should it be reported to ARM-software/CMSIS-FreeRTOS?

---

# Implementation (2.0, branch `buffered-channel-v2`): decisions, commits, results

**Decisions (maintainer):**
- **BufferedChannel: design B.** Static ring buffer, two counting semaphores (items/spaces) with static
  control blocks. ALT `activate()` takes the token with timeout 0. No RTOS call inside a CSP critical
  section, including semaphore releases and `putFromISR()`.
- **ALT wakeups: thread flags** in a documented, reserved bit range. `select()` re-verifies wakeups and
  checks the wait result.
- **Any number of blocking writers**, but at most one ALTing reader and one ALTing writer per channel
  (assert). Guard state per ALT, not per channel.
- Also in scope: static `TimerGuard`, handle checks at construction, deleted copy/move, `static_assert`s.
- **RTX5 harness first**, with the v1.0.0 regression baseline on both backends.
- **CSP-M model:** write only; the maintainer runs FDR.
- The pdsc version is **not** bumped and the pack is **not** rebuilt yet.

## Commits (on top of `21c0e09`)

| Commit | Change |
|---|---|
| `a7ee998` | tests: version- and backend-agnostic regression suite (PASS/FAIL), T2 via armlink `$Sub$$` interposition, T3i (real ISR), T8–T10 |
| `5c827a4` | fatal-error hook (`csp_fatal.h`); static `TimerGuard` control block (FreeRTOS: `StaticTimer_t` + adapter callback wrapper); non-copyable timeout guards; `Alternative` binds by reference |
| `7b293e1` | ALT via thread flags (bits 8–23; bit 0 = `RENDEZVOUS_FLAG`); `select()`: error check, disable only enabled guards, `confirm()`, `activate()` → bool; rendezvous `putFromISR()` snapshots then acts |
| `203bffd` | guard state per channel-end handle (`GuardSlot` in `Chanin`/`Chanout`), rendezvous and buffered |
| `302cc67` | tests: 1 KB worker stacks (RAM budget for the static T5 channel) |
| `6920d1c` | `BufferedChannel<T, SIZE, P>`: design B |
| `3709984` | tests: T11/T12 (rendezvous ALT regression), README, results |
| `f1711cf` | tests: compile-time checks (`static_assert`s, deleted copy/move, by-reference bindings) |
| `0af7731` | tests: T13/T13b (livelock of a high-priority ALT vs a preempted partner); **FAIL** on `6920d1c` |
| `a34d608` | `select()` backs off (one tick) after a lost `activate()` race: fixes the livelock |
| *(this commit)* | CSP-M model, CMSIS-FreeRTOS issue draft, `CHANGES_2.0.md`, this section |

Each library commit was built with 0 warnings under `-Wall -Wextra` on both backends and run through the
regression suite on both backends:

| After commit | FreeRTOS | RTX5 | Newly passing |
|---|---|---|---|
| v1.0.0 (baseline, 15-test suite) | 2 / 12 / 1 | 2 / 12 / 1 | — |
| `5c827a4` | 3 / 11 / 1 | 3 / 11 / 1 | T6 |
| `7b293e1` | 4 / 10 / 1 | 4 / 10 / 1 | T4b |
| `203bffd` | 5 / 9 / 1 | 5 / 9 / 1 | T4c |
| `6920d1c` | 15 / 0 / 0 (17 / 0 / 0 with T11–T12) | same | T1a T1b T2 T3 T3i T4a T5 T7a T9 T10 |
| `6920d1c` + T13/T13b | 17 / **2** / 0 | 17 / **2** / 0 | — (livelock found) |
| **`a34d608` (HEAD)** | **19 / 0 / 0** | **19 / 0 / 0** | T13 T13b |

Entries are PASS / FAIL / SKIP. Rows before T11–T13 existed used the smaller suite of that time.

## Final regression (identical 19-test suite; outputs in `tests/fvp_sse300/results/`)

| Library | FreeRTOS 11.3.0 | Keil RTX5 5.9.1 |
|---|---|---|
| **2.0 @ `a34d608`** | **PASS 19, FAIL 0, SKIP 0** | **PASS 19, FAIL 0, SKIP 0** |
| v1.0.0 @ `a789d2a` | PASS 6, FAIL 12, SKIP 1 | PASS 6, FAIL 12, SKIP 1 |

On v1.0.0, only T0, T8, T11, T12, T13 and T13b pass. T13/T13b pass there because v1.0.0 never retries
`activate()`: the livelock was introduced by, and fixed within, the 2.0 work.

On 2.0, every phase sweep (T1a, T1b, T3, T3i, T13, T13b; two sweeps each, about 2,500 trials per sweep,
covering the full v1.0.0 bug windows) recorded **0** bug trials. Compile checks: 14/14 on both backends
(`tests/compile_checks/`).

**End-to-end:** the FVP demo application (`helloworld_sse300` `application.cpp`) built against 2.0 and ran
for 24 s of simulated time. It uses rendezvous channels, pipe-syntax ALT, `fairSelect` and
`Run(InParallel)`.
- 0 data errors on both backends.
- Functional output identical to the migration reference (v1.0.0 pack).
- Messages verified in 24 s: FreeRTOS 470,000 (v1.0.0: 460,000), RTX5 520,000.

**Heap at the end of the suite** (2.0): FreeRTOS 360 B, RTX5 416 B. This comes from the rendezvous
channels' mutexes, which are outside this work (see `docs/CHANGES_2.0.md`). BufferedChannel, `Alternative`
and `RelTimeoutGuard` use 0 B (T5, T6).

## Findings made during the implementation

1. **Livelock in the first design-B commit (`6920d1c`), found by reviewing the CSP-M model.**
   - What happens: a high-priority ALT whose `enable()` reports ready (`count_ > 0`) while the
     semaphore token is not yet released, because a lower-priority writer was preempted between its
     critical section and `osSemaphoreRelease()`, retried `select()` immediately and spun forever.
   - The same applies to the output side, and to a blocking competitor holding the token.
   - Reproduced by T13/T13b on both backends (21–23 spins per sweep on FreeRTOS, 12–15 on RTX5).
   - Fixed in `a34d608` with a one-tick back-off. The model file keeps the draft variant, whose assertion
     is expected to fail with divergence.
   - **Residual cost:** in this rare race the ALT sees up to one tick of extra latency. It never
     busy-waits.
2. **v1.0.0 on RTX5: KeepNewest `putFromISR()` always loses the newest value.** `isrRtxMessageQueueGet()`
   defers freeing the message block to post-processing (PendSV). The immediate re-`put` in the same ISR
   therefore finds no free block, and the drop has already happened. That is why T3i on v1.0.0/RTX5 fails
   on every trial, not only inside a race window.
3. **Rendezvous and signal-channel guards cannot be re-verified generically.** In an ALT-to-ALT rendezvous
   the partner copies the data during *its* `activate()` and clears the registration. The receiving
   guard's `disable()` then reports "not ready" although it has completed, and `SyncChannel`'s `disable()`
   behaves similarly. These guards keep "trust the wakeup" (the default `confirm()`), as in 1.0.0. Only
   buffered and timer guards re-verify.
4. **FreeRTOS adapter static timers** need `cb_size >= sizeof(StaticTimer_t) + sizeof(TimerCallback_t)`;
   otherwise the callback wrapper is `pvPortMalloc`ed silently. RTX5 needs `cb_size == sizeof(osRtxTimer_t)`
   exactly. `csp_static_timer_storage_t` covers both.
5. **Upstream (ARM-software/CMSIS-FreeRTOS):** the `IS_IRQ_MASKED()` Armv8.1-M gap (item 2 of this
   analysis) still exists on `main` @ `c3e5dc3` (2026-09-01, `11.3.1-dev`). An issue is drafted, **not
   filed**: `docs/upstream/CMSIS-FreeRTOS_IS_IRQ_MASKED_Armv8.1-M.md`.

## Formal model (`docs/formal/buffered_channel_v2.csp`)

*Status update: the model was later checked with ProB 1.16.1; see "Review round 2", section 2. The text
below describes the model as first written.*

The model was written for FDR4 and was not run at that point; the "expected" annotations were design
intent.
1. **ALT reader vs. one final message, no successor.**
   - v1.0.0 protocol: expected to deadlock (the lost wakeup) and to fail `[FD=`.
   - The `6920d1c` draft: expected to fail `[FD=` with **divergence** (the livelock).
   - 2.0 with back-off: expected deadlock-free, no take-from-empty, and eventual delivery, even with an
     adversarial stale signal.
2. **KeepNewest, capacity 2, task writer and ISR writer.** The v1.0.0 two-step write should fail
   refinement of the serial specification (counterexample ending `rd.2, rd.4`). The 2.0 single atomic
   update should pass `[T=` and `[FD=`.
3. **Block bounded buffer, capacity 1, two blocking writers, items/spaces tokens.** It should refine
   `BUFF` and keep per-writer FIFO order.

Assumptions:
- Each CSP critical section is one atomic event.
- An ISR's operation is atomic relative to tasks.
- The one-tick back-off ends only after the preempted partner has finished its in-flight step. This is a
  scheduling (fairness) assumption, stated in the file.

## Remaining limitations and risks

- **Rendezvous and signal channels still have one ALT registration slot per direction**, and are not
  covered by the one-ALTing-process assert or by re-verification.
- **Remaining RTOS heap use:** rendezvous and `SyncChannel` mutexes and semaphores, and `Barrier`.
- **Interrupt latency:** `putFromISR()` and every buffered operation copy `sizeof(T)` bytes with BASEPRI
  raised.
- **Thread-flag collision** with native FreeRTOS index-0 task notifications on CSP threads (documented).
- ~~**RTX5, channels constructed before `osKernelInitialize()`** may fail~~. Superseded (review round 2,
  T14): namespace-scope construction works on both backends.
- ~~**The back-off** adds up to one tick of latency~~. Superseded: the back-off was removed in `dd954a9`
  (review round 2).
- **Test coverage:** Arm Compiler 6 at `-O0` only; FVP only, no hardware; the Cortex-M55 core only.
  The FreeRTOS ISR-path variant of item 2 (Armv7-M / Armv8-M Mainline) was not built.

---

# Review round 2 (branch `buffered-channel-v2`, not merged, version not bumped)

| Commit | Change |
|---|---|
| `dd954a9` | BufferedChannel: ALT readiness from the semaphore count; **back-off removed** |
| `8ba4bdb` | tests: T2 interposition for GCC (`--wrap`) |
| `b5856a3` | tests: T14 (namespace-scope channels / pre-`main()` creation) |
| `17cead1` | `buffered_channel.h`: masked element copy documented (comment only) |
| *(this commit)* | results, README, CSP-M model extensions, this section |

## 1. Livelock: structural fix instead of the back-off (implemented in `dd954a9`)

### Mechanism of the `6920d1c` livelock

- Readiness came from the ring state (`count_ > 0`), but `activate()` needs the semaphore token.
- A partner preempted between its ring update and `osSemaphoreRelease()` created a state where
  "ready" was true while no token existed.
- A higher-priority ALT then retried forever. `a34d608` hid this behind a one-tick sleep, which made
  progress depend on the tick and on the preempted partner getting CPU time.

### Fix

Readiness is the token count, `osSemaphoreGetCount(items | spaces) > 0`. The protocol, with R, C, P, T,
S, G as in the header of `buffered_channel.h`:

| Side | Step | What it does |
|---|---|---|
| ALT `enable()` | R | register `{thread, flag}` (critical section) |
| ALT `enable()` | C | readiness = token count > 0 (an RTOS call, therefore after R and outside the section) |
| Partner | P | ring update (critical section) |
| Partner | T | `osSemaphoreRelease()` |
| Partner | S | snapshot the registration (critical section; moved **after** T) |
| Partner | G | `osThreadFlagsSet()` |

### Proof sketch, input side (one ALT reader, any number of writers)

- **No lost wakeup.** The ALT blocks only if C saw 0 tokens, i.e. C happened before the relevant T. It is
  then not woken only if S missed the registration, i.e. S happened before R. With R before C (program
  order) and T before S (program order), that requires R < C < T < S < R, a cycle. So a token released
  after C is always followed by a signal to the registered thread.
- **No false readiness.** "Ready" means a token exists at C. With one ALT reader, no other thread
  consumes `items` tokens, so `activate()`'s `osSemaphoreAcquire(items, 0)` cannot fail. If extra
  blocking readers exist anyway, a failure means one of them took the token; the next round then reads
  the reduced count.
- **A writer preempted between P and T** leaves the count at 0, so the ALT registers and blocks. The
  writer's later T, S, G wake it.

### Output side (one ALT writer, any number of blocking writers, one reader)

- `spaces` tokens are taken by blocking writers **before** their ring update. A blocking writer that
  holds a token but is preempted before its push has therefore already removed that token from the
  count, and the ALT writer sees the correct, lower value.
- The reader releases `spaces` after its pop (T), then snapshots the writer registration (S) and signals
  (G), so the same cycle argument applies.
- `activate()` can fail only if a blocking writer acquired the token between the ALT's C/confirm and its
  acquire, i.e. another writer made progress. The retry reads the new count and blocks if it is 0.
  **Each failed round implies another thread's progress, so there is no livelock** without any sleep.

### Other changes in `dd954a9`

- **Cost:** one extra, short critical section per operation (S). The readiness check is an RTOS call
  (`osSemaphoreGetCount()`), made outside critical sections.
- **`pending()` / `space_available()`** remain informational (ring state).
- **`select()`:** the back-off is removed; a failed `activate()` starts a new round. The `Guard`
  contract in `alt.h` now states that readiness must not become true before the partner's operation is
  complete.

### Results

- T13/T13b show **0 spins in all 48 sweeps**: 12 configurations × 2 tests × 2 sweeps.
- The full suite passes in every configuration (see 3).

## 2. Formal model: checked with **ProB 1.16.1** (FDR not used)

**History.** FDR4 was not installed, and its licence needs the maintainer's registration, so the model
was first committed unchecked. It was then checked with ProB (`probcli` 1.16.1-final, 716929f, SICStus
4.9.0; Linux tarball from <https://prob.hhu.de>), which reads FDR-dialect CSP-M (commit `04a4a3d`).

**What changed while checking.**
- **Positive control #1 failed at first:** `SYS_OLD` synchronised on `S1`, which lacks `backoff_tick`.
  `STATE1` could then do `backoff_tick` forever, so the v1.0.0 system was wrongly deadlock free, and
  assertion 2 failed only through that divergence. `SYS_OLD` now synchronises on `S1B`. This was a model
  bug; FDR would have given the same result.
- `st == 1 & alt & sg_send` became `st == 1 and alt & sg_send`: ProB groups the first form as
  `(st == 1 & alt) & sg_send` and rejects it. The second form is valid in both dialects.
- **Alphabet audit:** for every `SYS_*` system, ProB confirmed that no event outside the
  synchronisation set occurs except the intended observables (`done`, `done_alt`, `done_blk`, `idle`,
  `error`, `wdone2`, `sg_rdone`, `sg_sdone`). The audit flags the original `SYS_OLD` at once.
- **Mutation controls (Part 1d):** m1 (token count read before registering) and m2 (registration
  snapshot taken before the token release). Both deadlock, so the model distinguishes the orderings that
  the final protocol depends on.
- The unused channels `wflag_r`/`wflag_w` were removed.

**Results: all 25 assertions match their annotations.** The full table with counterexamples is in the
model header.

| # | Assertion | Expected / ProB |
|---|---|---|
| 1 | `SYS_OLD` deadlock free (positive control) | FAIL: `clear_flag, pending_chk.false, put_old, notify_old.false, register_old`, deadlock |
| 2 | `DELIVERED [FD= SYS_OLD` | FAIL: `done` refused after the empty trace |
| 3 | `DELIVERED [FD= SYS_DRAFT` (positive control) | FAIL: divergence (the `6920d1c` livelock) |
| 4–6 | `SYS_NEW` (back-off): deadlock, delivery, no take-from-empty | PASS |
| 7–12 | `SYS_C`, `SYS_C2` (final, `dd954a9`): same three properties | PASS |
| 13, 14 | mutations m1, m2 deadlock free | FAIL: lost-wakeup deadlocks |
| 15 | KeepNewest v1.0.0 | FAIL: `rd.2` |
| 16, 17 | KeepNewest 2.0 `[T=`, `[FD=` | PASS |
| 18, 19 | Block buffer: `BUFF`, per-writer FIFO | PASS |
| 20, 21 | Rendezvous trust path | PASS |
| 22 | Rendezvous, re-verifying | FAIL: deadlock |
| 23 | Rendezvous trust path + stale flag | FAIL: `rdone.false` (known limitation) |
| 24 / 25 | SyncChannel trust / re-verifying | PASS / FAIL: deadlock |

**Reproduce:**
`probcli docs/formal/buffered_channel_v2.csp -csp_assertion '<assertion without "assert">'`.
The loop over all assertions is in the model header. ProB writes a parse cache, `*.csp.pl`, which is
ignored by git.

**What the model covers** (as of review round 2: 23 assertions; the mutation controls, #13 and #14, were
added later):

| Protocol | Systems | Expected |
|---|---|---|
| (a) v1.0.0 check-then-register | `SYS_OLD` | deadlock (lost wakeup); fails `[FD=` delivery |
| (b) ring-count readiness | `SYS_DRAFT` (`6920d1c`) | divergence (livelock) |
| (b) ring-count readiness + back-off | `SYS_NEW` (`a34d608`) | passes, under the stated fairness assumption |
| **(c) semaphore-count readiness (`dd954a9`)** | `SYS_C` (stale-signal adversary); `SYS_C2` (competing blocking reader, two messages) | deadlock-free, divergence-free, never takes from an empty buffer, both readers served. No fairness assumption is needed. |
| KeepNewest, task writer + ISR writer | two-step (v1.0.0) vs. one atomic update (2.0) | v1.0.0 fails the serial specification; 2.0 passes |
| Block buffer, items/spaces tokens | — | refines `BUFF` and keeps per-writer FIFO order |
| **Rendezvous ALT-vs-ALT (trust path)** | `SYS_RV(true)` | completes |
| Rendezvous, re-verifying variant | `SYS_RV(false)` | deadlocks, which shows why these guards must trust the wakeup |
| Rendezvous trust path + stale flag | `SYS_RV_STALE` | a stale flag lets `select()` return without data (the documented limitation) |
| **SyncChannel ALT receiver (trust path)** | `SYS_SG(true)` | completes |
| SyncChannel, re-verifying variant | `SYS_SG(false)` | deadlocks |

Modelling assumptions are stated in the file: each critical section and each mutex section is one event;
an ISR's operation is atomic relative to tasks; for 4b, only the interleaving in which the receiver
registers first is modelled.

## 3. Optimisation levels and toolchains

**Build matrix.**
- Arm Compiler 6.24 and GCC 14.2.1 (Arm GNU 14.2.Rel1, Armv8.1-M MVE hard-float multilib);
- `-O0`, `-O2`, `-Os`;
- FreeRTOS and RTX5.

That is 12 images. Each was built with the exact harness flags plus `-Wall -Wextra`: **0 warnings in 60
translation units per toolchain.**

| Toolchain | Backend | `-O0` | `-O2` | `-Os` |
|---|---|---|---|---|
| AC6 | FreeRTOS | 20/0/0 | 20/0/0 | 20/0/0 |
| AC6 | RTX5 | 20/0/0 | 20/0/0 | 20/0/0 |
| GCC | FreeRTOS | 20/0/0 | 20/0/0 | 20/0/0 |
| GCC | RTX5 | 20/0/0 | 20/0/0 | 20/0/0 |

Entries are PASS/FAIL/SKIP. Every race sweep covered both regimes: at least 42 EARLY and 674 LATE trials
per sweep, with 0 BUG and 0 ANOMALY. The boundary `kb` moves with the code speed, from about 26,500 (GCC
`-O0`) to about 54,000 (GCC `-O2`), and the sweep follows it.

**Positive control.** v1.0.0 built with GCC `-O2` and with AC6 `-O2` (FreeRTOS) gives PASS=6 FAIL=12
SKIP=1, the same as at `-O0`. T2's `--wrap` detector counts 5 RTOS calls with BASEPRI raised under GCC,
and the race sweeps still find the v1.0.0 bugs. The harness is therefore sensitive at those settings.

### Ring-index access review (task/ISR, compiler barriers)

- `head_`, `storage_` and the `AltWake` registrations are only accessed between `csp_enter_critical()` and
  `csp_exit_critical()`.
- In CMSIS 6.0.0, both `__set_BASEPRI_MAX()` and `__set_BASEPRI()` are `asm volatile` with a
  **`"memory"` clobber**, in `cmsis_armclang_m.h` and `cmsis_gcc_m.h` alike. They are therefore full
  compiler barriers, and no ring access can be moved out of the section at any optimisation level.
  `volatile` is not needed on those fields.
- `count_` is `volatile` because `pending()`/`space_available()` read it outside the section. Those are
  informational only; ALT readiness now comes from the RTOS semaphore count.
- Single core: tasks and ISRs share one view of memory in program order, so no `DMB` is needed between
  a critical section and an ISR. The M55 data cache is coherent for the core's own accesses.
- `TimerGuard::fired` / `wake_thread` / `wake_flag` are `volatile` single words written in one thread and
  read in the timer thread, so their accesses are atomic on Cortex-M. A torn `{thread, flag}` pair is
  harmless: at worst a stale flag, which is re-verified.
- **Open point (hardening proposal, not implemented):** `csp_enter_critical()` issues `MSR BASEPRI_MAX`
  without a following `DSB`/`ISB`, whereas the FreeRTOS ARM_CM55 port's `ulSetInterruptMask()` uses
  `msr basepri; dsb; isb`.
  - My understanding is that an MSR that *raises* execution priority takes effect for subsequent
    instructions without a context-synchronisation event; I could not confirm this against the Arm ARM
    here.
  - Adding `__DSB(); __ISB();` after the MSR costs a few cycles and removes the question.
  - I recommend adding it; not done in this task.

## 4. Global channels on RTX5

**Survey.** I classified channel declarations by scope, resolving type aliases (e.g. `using AltChannel =
Channel<Message>`) and ignoring vendored library copies:

| Repository | Backend(s) | Namespace scope | Function-local `static` |
|---|---|---|---|
| This repo (CSP4CMSIS) | — | 0 (no examples) | 0 |
| FVP `helloworld_sse300` demo | FreeRTOS / RTX5 harness | 0 | 2 |
| Alif-DK-E8-CSP4CMSIS (`neuropathway`, `pack_test`) | FreeRTOS adapter | 0 | 4 |
| Alif `csp4cmsis_alt_test` (uses `Alif/DK-E8/application.cpp`) | **RTX5** | 0 | 2 (`chan_A`/`chan_B` in `MainApp_Task`) |
| HimaxWE2-CSP4CMSIS | FreeRTOS | 32 | 22 |
| The_Way_of_Static_Process_Networks/GithubCode (copies of Himax and Nucleo) | FreeRTOS | 37 | 26 |
| CSP4CMSIS-B-L475E-IOT01A | FreeRTOS | 1 | 1 |
| CSP4CMSIS-Nucleo, CSP4CMSIS_for_NUCLEO-G474RE | FreeRTOS | 0 | 2 each |

- The only RTX5 project constructs its channels as function-local statics *after* the kernel has
  started.
- That same project creates `MainApp_Task` (static control block) with `osThreadNew()` **before** its
  explicit `osKernelInitialize()`.
- All 70 namespace-scope channels are in FreeRTOS projects.

**Measured behaviour (T14, AC6 and GCC, both backends).** During C++ static initialisation:

| Backend | Kernel state | Static `osSemaphoreNew()` | Dynamic `osSemaphoreNew()` | Namespace-scope 2.0 `BufferedChannel` used after start |
|---|---|---|---|---|
| FreeRTOS | `Inactive` | succeeds | succeeds | works |
| RTX5 | **`Ready`** | succeeds | succeeds | works |

- **The reason on RTX5:** CMSIS-RTX 5.9.1 initialises the kernel before C++ constructors, through weak
  hooks in `rtx_lib.c`:
  - `_platform_post_stackheap_init()` (Arm Compiler);
  - `software_init_hook()` (GCC/newlib; verified linked in the GCC image);
  - `$Sub$$__iar_data_init3` (IAR).
- A later explicit `osKernelInitialize()` in `main()` is harmless. This is also why DK-E8's pre-init
  `osThreadNew()` works.
- **My earlier concern (analysis item 7l, CHANGES) was wrong** and has been corrected.

**Options:**

| Option | Pros | Cons |
|---|---|---|
| **A. Eager construction (current), documented rule** | Works today on both backends; no hot-path cost; creation failure is already fatal (loud) | Depends on the RTX pre-init hook (standard CMSIS startup plus C library init); an application that overrides the hook or bypasses C library init gets a fatal error at boot |
| B. Deferred creation on first use | Independent of init order | Needs a one-time-init protocol on every operation. RTOS creation cannot run inside a critical section, so it needs a lock or a CAS state machine. A first use from an ISR (`putFromISR()`) could not create objects at all. Adds a branch to every operation |
| C. Creation in `Run()` | Deterministic point | Channels are not known to `Run()` (processes hold handles), so a registry is needed; channels used outside `Run()` would break |

**Recommendation: A.** Keep eager construction and add a documented rule to
`CSP4CMSIS_Configuration.md`: *channels may be constructed at namespace scope or as function-local
statics. On RTX5 this relies on CMSIS-RTX's pre-`main()` `osKernelInitialize()` hook, so do not override
`_platform_post_stackheap_init` / `software_init_hook` / `__iar_data_init3` without calling
`osKernelInitialize()`. Never construct channels in an ISR.* T14 guards the rule in the regression suite.
Not implemented yet, as instructed.

## 5. `putFromISR()`: masked copy

**Documented** in `buffered_channel.h` (`17cead1`):
- every element copy (`output()`, `input()`, `putFromISR()`, ALT `activate()`) runs with BASEPRI raised;
- interrupts above `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` are unaffected;
- the documentation includes the index/pointer pattern for large payloads.

**Measured** (AC6 `-O2`, instructions between `MSR BASEPRI_MAX` and `MSR BASEPRI` in the
KeepNewest/`putFromISR()` path):

| Element | Masked section |
|---|---|
| `uint32_t` | 12 instructions, no calls (plus 1 instruction in the snapshot section) |
| 1 KB struct | 16 instructions plus `__aeabi_memcpy4` of 1,024 bytes |

For the 1 KB case I *estimate* about 0.5–0.7 k cycles of LDM/STM traffic, i.e. roughly 15–20 µs at the
AN552's 32 MHz, or about 2–3 µs at 250 MHz. That is an estimate, not a measurement: the FVP is not
cycle-accurate.

**Proposal (not implemented):**
1. **Compile-time limit for ISR use:** `static_assert(sizeof(T) <= CSP4CMSIS_ISR_MAX_ELEMENT_BYTES)`,
   default 64 bytes, overridable with `-D`.
   - It belongs in the **non-virtual** `Chanout<T>::putFromISR()`, not in the virtual
     `BufferedChannel::putFromISR()`. A virtual member of a class template is instantiated with every
     channel type, so the assert would fire for large-`T` channels that never use an ISR. The non-virtual
     wrapper is only instantiated when an ISR path actually calls it.
   - 64 bytes is a 16-word copy, about the cost of an RTOS queue operation.
2. **Index/pointer pattern for large payloads:** a static pool plus a channel of indices (`uint8_t`).
   This is documented in the header.
   - A complete ISR-side free list would need a non-blocking ISR read (e.g.
     `bool getFromISR(T&)`); until then, an ISR must use indices it already owns (e.g. a pre-assigned
     ping-pong pair).
   - A future `getFromISR()` would be a small addition following the same pattern: acquire the token
     with timeout 0, pop, release, signal after the section.

---

# Review round 3: rendezvous/signal "trust the wakeup" (CSP-M assertion 23)

Analysis only. The library is unchanged; the tests and the fix model are new.

| Commit | Change |
|---|---|
| `cbb6ef8` | `.gitignore` for ProB caches; unused model channels removed; FDR section → ProB results |
| `20862b4` | tests: T15i, T15s, T15 (FAIL expected); reference results for the 23-test suite |
| *(this commit)* | `docs/formal/rendezvous_commit_record.csp` (fix model, checked); this section |

## 1. What an application observes

Rendezvous (`ChanInGuard`/`ChanOutGuard`) and signal (`SyncChannelInputGuard`) guards keep
`Guard::confirm()`'s default, which trusts the wakeup. Their `activate()` also returns true when no
partner is left (rendezvous "case 3"). So any flag for the guard's bit that does not come with a transfer
**for the current round** completes the guard.

| Observation | Mechanism | Test |
|---|---|---|
| **Rendezvous reported without data transfer (PHANTOM):** `select()` returns the channel's index; the destination still holds whatever was there before | a late wakeup from an earlier round, or a flag that carries no transfer | T15, T15i |
| **Silent consumption (SILENT):** `select()` returns another guard, but the channel's data was written into the destination (and the writer's `select()` reports success) | the ALT partner copies under the mutex and wakes later; the ALT meanwhile takes another guard. `disable()` sees no writer and says "not ready", not "already done" | T15 |
| **Wrong guard index:** the bit belongs to guard *i* of an earlier `select()`; if the thread now selects on a different `Alternative`, guard *i* of that one completes | thread flags carry only the index, not the `Alternative` or the round | not tested (follows from the mechanism) |
| **Phantom output:** an ALT writer's `select()` returns the channel although no reader received the value | `ChanOutGuard::activate()` case 3 after the reader withdrew | model only (`w_sent.false`) |
| **Lost signal, sender blocked for good** | `SyncChannel::unregisterAltIn()` resets the channel after a sender has already signalled; the sender waits on `sender_sem` forever | T15s |

Sources of the stray flag in the current code:
- the ALT-vs-ALT partner's `wakeUp()` after `osMutexRelease()` in `ChanInGuard/ChanOutGuard::activate()`;
- `SyncChannel::output()`'s wakeup after releasing the mutex;
- rendezvous `putFromISR()`, which wakes an ALT reader **without transferring anything** (deterministic);
- a `KeepNewest`/`KeepOldest` rendezvous `output()`, which wakes the ALT reader and then drops the data
  (read in the code, not tested);
- application code setting bits 8–23 (documented as reserved).

`select()` clears its bits at the start of every round. That only removes flags that arrive *before* the
clear; T15's flag arrives after it.

## 2. Reachability on the FVP

| Test | 2.0, 12 configurations | v1.0.0 (AC6, FreeRTOS + RTX5) |
|---|---|---|
| T15i (rendezvous `putFromISR()` → ALT reader) | FAIL everywhere: `putFromISR()` returns 1; the reader selects the channel with its destination unchanged (`0xdeadbeef`) | FAIL, same |
| T15s (signal lost while the receiver takes X) | FAIL everywhere: 2nd select times out; the sender never completes | FAIL, same |
| T15 (sweep, ALT-vs-ALT late wakeup) | FAIL everywhere: **61–120 bad trials** per two sweeps; each is exactly one SILENT + one PHANTOM | FAIL: 106–121 SILENT+PHANTOM pairs |

- The other 20 tests pass in all 12 configurations; T13/T13b show 0 spins.
- These are **long-standing defects**, not 2.0 regressions.
- The T15 bug window is k ≈ 30,800–31,700 (FreeRTOS AC6) and 31,600–31,730 (RTX5 AC6). It is wider on
  FreeRTOS because the reader, waiting on the channel mutex, preempts the writer at `osMutexRelease()`.

## 3. Fix model: per-round commit record (`docs/formal/rendezvous_commit_record.csp`)

**System:**
- ALT reader {C rendezvous, X buffered} doing two selects;
- ALT writer {C};
- a third-party X message;
- one adversarial stale flag (for either side).

That is T15 plus assertion 23's adversary. **Fix (FIX):**
1. `AltScheduler` numbers its rounds.
2. `enable()` registers `{alt, flag, round}`.
3. A partner that transfers data into an ALT's registration does it under the channel mutex, **together
   with a commit record `{round}`** and the removal of the registration. The wakeup follows after the
   mutex, as today.
4. `disable()` reports *committed in my round*; **a committed guard wins the selection**, with no
   `activate()` because the transfer is already done.
5. Any other wakeup is re-verified by readiness, and `activate()` without a partner returns false, which
   starts a new round.

**ProB 1.16.1 results** (all as expected):

| Variant | Result |
|---|---|
| CUR (current code) | fails `[T=` / `[FD=`: `r_sel.gX.true` (SILENT); the trace `r_sel.gX.true, r_sel.gC.false` (= T15) and `w_sent.false` also exist |
| **FIX**, with a stale flag | **passes `[T=`, `[FD=` (divergence-free, live), deadlock freedom, round bound** |
| REVER: re-verify by readiness, no commit record (assertion 22's approach) | fails: deadlock (the reader sees "not ready" after the partner moved the data), and data loss |
| NORC: no round check, record kept | fails: `r_sel.gC.true, w_sent.true, r_sel.gC.false` (the old record completes the next select) |
| NORCCLR: no round check, record consumed when read | **passes** (see below) |
| NOWIN: commit only counts for the chosen guard | fails: SILENT |
| CASE3: `activate()` without a partner returns true | fails: `w_sent.false` |

- **Assertion 23's scenario passes, and assertion 22's deadlock does not return.**
- **On the round number:** in this task-only model a commit needs the live registration, and `disable()`
  removes the registration under the same mutex. A commit therefore never outlives its round if the ALT
  consumes it in `disable()`. The round check and consume-on-read are then interchangeable; dropping
  *both* fails (NORC).
- The round number becomes necessary where a commit is **not** serialised with `disable()` by the mutex:
  - partners in ISRs (`putFromISR()` uses the CSP critical section, while task code uses the mutex, so
    the two do not exclude each other);
  - any design that keeps the record across rounds.

  **Recommendation: keep both** (round-tagged and consumed); it costs one word per registration.

**Not covered by the model:**
- **Two partners committing into the same ALT round through different channels** (e.g. ALT reader on two
  rendezvous channels, both with ALT writers). Each commit is under its own channel mutex, so both can
  succeed and one transfer is lost, today and with a per-channel record alike.
  - The fix needs a **per-ALT claim**: an atomic "round r: open → claimed", taken by the partner before it
    copies, with CSP critical section or LDREX/STREX.
  - The model should be extended to two committing partners before implementing.
- ISR partners, plain (non-ALT) partners, `SyncChannel`.

## 4. Implementation estimate (not implemented)

| Area | Change | Size |
|---|---|---|
| `alt.h`, `alternative.cpp` | round counter; guards report *committed* (tri-state `disable()` or a new virtual); `select()` prefers a committed guard; per-ALT claim word | ~80–120 lines |
| `alt_channel_sync.{h,cpp}` | `WaitingAlt.round`; commit record plus claim under the mutex in `activate()` case 2; case 3 → false; `disable()` reports the commit | ~80–120 lines |
| `rendezvous_channel.h` | `putFromISR()` must transfer and commit, or return false, for ALT readers. The shared state is currently mutex-protected on the task side and BASEPRI-protected on the ISR side, which do not exclude each other: e.g. the ISR between the two stores of `registerWaitingTask()` would `memcpy()` to a null pointer. Either move `AltChanSyncBase` state under CSP critical sections (like the buffered channel) or stop supporting ISR writes to rendezvous channels. Non-`Block` `output()` must not wake an ALT whose data it then drops | ~40–80 lines |
| `sync_channel.{h,cpp}` | commit record; `unregisterAltIn()` must not reset a signalled channel; `putFromISR()` currently returns true for a *blocking* receiver without releasing it (read, not tested), and reads `state` without any lock | ~60–100 lines (rework) |
| buffered/timer/skip guards | adapt to the `Guard` API change (never "committed") | ~10–20 lines |
| tests and model | T15 family turns green; new tests for two committing partners, SyncChannel ISR, phantom output; model extended with the per-ALT claim | ~200 lines |

**Total:** about 300–450 library lines, plus tests and model.

**Risk: medium–high.**
- It changes the `select()` protocol that every channel type goes through.
- ALT-vs-ALT with several partners needs a new cross-channel atomic step.
- `SyncChannel` needs a rework rather than a patch.
- The `Guard` interface changes (internal API).

**Mitigation:**
- model first, including the claim;
- then implement against the T15 family, T11/T12 (ALT-vs-ALT, fair select) and the full 12-configuration
  matrix;
- keep the buffered-channel path (already re-verifying, not affected) untouched.

---

# Review round 4: cheap gates, ALT design comparison, API questions

| Commit | Change |
|---|---|
| `cbdc9bc` | A1: `DSB; ISB` after raising BASEPRI |
| `4ab6647` | A2: `putFromISR()` element-size limit (`CSP4CMSIS_ISR_MAX_ELEMENT_SIZE`, default 64) |
| `57799f5` | A3: rule for where channels may be constructed (`Documentation/CSP4CMSIS_Configuration.md` §5) |
| `6a6e439`, `73594b3` | B: `docs/formal/alt_one_winner.csp` (new), `rendezvous_commit_record.csp` generalised; checked |
| `1ad3e11` | D: tests T16s, T16n, T16a; 26-test reference results |
| *(this commit)* | this section |

## A. Cheap gates (implemented)

After each of A1–A3 the full suite ran on all 12 configurations: **PASS=20 FAIL=3** (the known T15 family)
everywhere, and 0 T13/T13b spins. A3 is documentation only; its 12 images are byte-identical to A2's.

- **A1 `DSB; ISB`:** `objdump` shows the barriers after every `MSR BASEPRI_MAX` (AC6 `-O2`: 77 of 77
  inlined sites; GCC `-Os`: the one outlined copy).
  - **Cortex-M7 r0p1 (erratum 837070):** `DSB/ISB` does *not* fix it. The FreeRTOS `ARM_CM7/r0p1` port
    brackets the MSR with `CPSID i`/`CPSIE i`. This is documented in `csp_critical.h`, not implemented.
  - **No sibling project uses a Cortex-M7.** Cores found: M55 (Alif DK-E8, Himax WE2, FVP); M4F (STM32L475
    B-L475E-IOT01A, G474 NUCLEO, F401 Nucleo and its CubeIDE workspaces). Every M7 or "837070" hit is in
    vendor boilerplate (FreeRTOS `port.c` comments, HAL legacy headers, CMSIS-NN comments).
- **A2 size limit:** `static_assert` in the non-virtual `Chanout<T>::putFromISR()`.
  - Compile checks: 20/20 with AC6 and GCC flags on both backends (new probes: 65 B fails; 64 B passes; a
    1 KB task-only channel passes; `-D` 128 allows 100 B).
  - Every `putFromISR()` element in the sibling projects is ≤ 12 bytes.
- **A3:** namespace scope and function-local statics are allowed; never construct a channel in an ISR; the
  RTX5 start-up-hook rule; enable an interrupt only after its channel exists.
- **Noticed, not changed:** `CSP4CMSIS_Configuration.md` §3 still says the priority define is needed only
  with `BufferedChannel`/`putFromISR()`. `rendezvous_channel.h` includes `csp_critical.h`, which `#error`s
  without it, so every build needs it (FVP repo `UPSTREAM_ISSUES.md` #2).

## B. ALT with rendezvous/signal channels: one-winner vs commit-record

**Common model** (both files):
- **System:** ALT reader A over two channels, two selects, destination reset per select. One partner per
  channel (ALT writer, plain writer, or signal sender), plus an optional adversarial stale flag.
- **Scenarios:**
  - **SC1** = T15 (SILENT, PHANTOM);
  - **SC2** = two ALT partners on *different* channels racing for the same ALT;
  - **SC3** = T15s (lost signal).
- **Specification:** each channel selected once, with its own data and without the other's; every
  partner's message delivered; A always progresses.
- **Positive controls:** the current code fails all three scenarios (SC1: SILENT; SC2: phantom output
  `w_done.1.false`; SC3: phantom signal / deadlock).

| | Commit-record + per-ALT claim (`CLAIM`) | One-winner (`OW`, `OWRV`) |
|---|---|---|
| Who performs the transfer | the partner, into the ALT's destination, under the channel mutex | the ALT process, in `activate()`; partners stay pending |
| ProB, SC1–SC3 with stale flag | passes `[FD=`, deadlock freedom, round bound | passes `[FD=`, deadlock freedom, round bound (both variants) |
| Mechanisms, each shown necessary by a failing mutation | round-tagged record or consume-on-read (NORC); committed guard wins (NOWIN); `activate()` without partner → new round (CASE3); **per-ALT claim** (round-3 FIX fails SC2: two commits into one round); **claim-aware partner readiness** (CLAIMNR: unbounded retries = livelock) | re-verification or state check after a wakeup (OWTRUST: PHANTOM); **atomic two-word claim** for ALT-vs-ALT (OWSPLIT: deadlock) |
| Per-round sequence numbers | needed (or consume-on-read) | not needed |
| Remaining assumptions | single core; each mutex/critical section atomic; ISR partners not modelled; ALT writers with one guard. With more guards the writer's own ALT must be claimed too, i.e. the design converges on the two-word claim | the same, except that the multi-guard case is covered by an argument, not a check: a claim needs both state words claimable in one critical section, so two ALTs cannot claim each other crosswise |
| Interaction with buffered/timer guards (already verified) | none: records and claims only on rendezvous guards | **OWRV: none**: buffered and timer guards keep re-verifying and never claim. (OW with the WAITING check would require every partner, including the buffered S step and the timer callback, to set READY, which changes the verified buffered protocol. Avoid.) |
| Signal channels | `SyncChannel` needs a commit record and a rework | a signal is a rendezvous without data: `SyncChannel` can be replaced |
| Estimated library change | ~350–500 lines (records, rounds, claim word, `Guard` tri-state, `SyncChannel` rework) | ~250–350 lines (state word plus claim ~60; rendezvous guards ~150; partner paths ~60), with a **net reduction** if `SignalChannel` reuses the rendezvous code (`sync_channel.cpp`, 290 lines) |
| Fit with the book (CSP ☐ over inputs) | observationally correct with the claim, but the partner decides and performs the event on the ALT's behalf | the occam/JCSP realisation of ☐: an offer stays until the choosing process takes it; the event happens when the ALT performs it |

**Recommendation: one-winner with re-verification (`OWRV`)**, keeping ALT-vs-ALT (T11) through the atomic
two-word claim in one CSP critical section.
- It needs fewer mechanisms (two instead of five, each shown necessary).
- It needs no round numbers.
- It leaves the verified buffered-channel protocol untouched.
- It lets the signal channel collapse into the rendezvous code, and it matches the book's semantics.

**Before implementing:**
- extend the model to ISR partners, if C1 keeps them;
- extend it to ALT writers with two guards.

## C. API questions (recommendations; nothing changed)

### C1. `putFromISR()` on rendezvous and signal channels

**Uses** (application code; test code in this repository omitted):

| Project | File | Channel | Reader | Return value |
|---|---|---|---|---|
| HimaxWE2 `csp4cmsis_shake_detection` | `tests.cpp:28` | `Channel<bool>` (rendezvous) | plain `>>` | ignored |
| HimaxWE2 `csp4cmsis_kws_iic` | `csp4cmsis_spn.cpp:83` | `Channel<bool>` | plain `>>` (I2C completion) | ignored |
| HimaxWE2 `csp4cmsis_kws_PCA9685` | `csp4cmsis_spn.cpp:83` | `Channel<bool>` | plain `>>` (I2C completion) | ignored |
| HimaxWE2 `csp4cmsis_kws_PCA9685_alt` | `csp4cmsis_spn.cpp:85` | `Channel<bool>` | plain `>>` (I2C completion) | ignored |
| HimaxWE2 `csp4cmsis_irq` | `tests.cpp:25` | `Channel<uint32_t>` | plain `read()` | ignored |
| HimaxWE2 `csp4cmsis_allon_sensor_tflm` (book ch. 8) | `camera_process.cpp:19` | `Channel<trigger_t>` | plain `>>` | checked (`sent`) |
| Book ch. 5 `nucleo-g474re_Interrupts` | `application.cpp:28` | `Channel<ButtonEvent>` | plain `>>` | ignored |
| Book ch. 6 `nucleo-g474re_Sensor_Data_Processing_Network` | `application.cpp:39` | `Channel<trigger_t>` | plain `>>` | ignored |
| Alif `DK-E8/critsec_isr_test.cpp` | `:91` | `BufferedOne2OneChannel<…, 16, KeepNewest>` | — | checked |
| Book site `CSP4CMSIS/api.md` | §3 example | "`my_chan`" (text describes buffer policies) | — | — |

- The `The_Way_of_Static_Process_Networks/GithubCode/CSP4CMSIS` tree is a copy of the Himax apps.
- **Signal-channel `putFromISR()`: no uses.**
- **Every rendezvous use is ISR → a process blocked in plain input.** None uses ALT (so T15i's phantom does
  not hit them).

**Code-read issue in the I2C-completion pattern** (3 Himax apps): the process starts the transfer and then
waits in `i2c_sync >> dummy`. If the ISR fires first, `putFromISR()` finds no reader and returns false
(ignored), and the process blocks forever. A buffered channel of capacity 1 removes the race. Not tested.

**Recommendation: remove `putFromISR()` from rendezvous and signal channels.**
- ISR → process only via buffered channels (`BufferedChannel<T, 1>` for a completion,
  `SamplingBufferedChannel<T, 1, KeepNewest>` for a latest value). The reader code is unchanged.
- With this, the rendezvous state becomes task-only, so the mutex-vs-BASEPRI mismatch (T16a) disappears
  entirely.
- `Chanout<T>` cannot tell channel kinds apart at compile time. Options:
  - (a) make it a fatal error at run time for rendezvous/signal in 2.0;
  - (b) give buffered channels an `isrWriter()` that returns an ISR-only end with `putFromISR()`, and drop
    `Chanout::putFromISR()` in the next major version.

  (b) gives a compile-time guarantee.
- **Migration:** 8 application sites plus the book text of chapters 5, 6 and 8 and the API page.

### C2. KeepNewest/KeepOldest on rendezvous channels

**Uses:** none in any code. The only occurrence is the declaration example in the book site's API page
(`SamplingChannel<Message, BufferPolicy::KeepNewest/KeepOldest>`). The regex hits in Himax
`csp4cmsis_lossy_policy_test` and Alif `critsec_isr_test` are `BufferedOne2OneChannel` (buffered).

**Semantics problem:** a non-blocking rendezvous writer cannot wait for an ALT reader's `activate()`.
- Under the current code it wakes the ALT and drops the value (T16n: PHANTOM).
- Under the one-winner design it could only drop the value, or block, which contradicts its policy.

**Recommendation: restrict sampling policies to buffered channels.** Add a `static_assert(P == Block)` in
`SamplingChannel`, and document `SamplingBufferedChannel<T, 1, KeepNewest>` as the "latest value" channel.

## D. Code-read defects as FVP tests (commit `1ad3e11`)

| Test | Defect | 2.0 (12 configurations) | v1.0.0 | Disappears under |
|---|---|---|---|---|
| T16s | `SignalChannel::putFromISR()` returns true, blocked receiver never released | FAIL everywhere | FAIL | **C1** |
| T16n | `KeepNewest` rendezvous `output()` wakes a waiting ALT reader and drops the value (PHANTOM) | FAIL everywhere | FAIL | **C2** |
| T16a | rendezvous `putFromISR()` (BASEPRI) vs task path (mutex): copy through a null pointer between the two stores of `registerWaitingTask()` | PASS everywhere; **not conclusive** (window 1–2 instructions with AC6, absent with GCC `-O2` because the stores are reordered) | PASS | **C1** |
| T15i (round 3) | rendezvous `putFromISR()` to an ALT reader: PHANTOM | FAIL | FAIL | **C1** |
| T15, T15s (round 3) | late wakeup SILENT/PHANTOM; lost signal | FAIL | FAIL | only a protocol change (B) |

**Suite now (26 tests):** 2.0 PASS=21 FAIL=5 in all 12 configurations; v1.0.0 PASS=8 FAIL=17 SKIP=1.

---

# Review round 5: OWRV implementation

## Decisions (from the review)

1. **OWRV** (one-winner with re-verification) is the protocol for rendezvous and signal channels.
   CLAIMNR (`rendezvous_commit_record.csp`) stays as a permanent mutation control.
2. **C1:** `putFromISR()` is removed from `Chanout`. Buffered channels provide an ISR writer end, which
   carries the size `static_assert`. **C2:** `KeepNewest`/`KeepOldest` only on buffered channels
   (compile-time error otherwise). **The signal channel** becomes a data-less rendezvous on OWRV.
3. The model is extended before implementing (below).

## Why re-verification is safe in OWRV, and why it deadlocked before

**Before (current code; assertion 22 / mutation REVER):**
- In an ALT-vs-ALT rendezvous the *partner* completes the communication in its own `activate()`: it copies
  the data and removes the woken ALT's registration, and only then signals.
- The woken ALT re-verifies readiness, but the evidence (a registered partner) has already gone, *because
  the communication already happened*.
- The ALT concludes "stale", registers again and waits for a partner that has finished: a deadlock. That
  is why those guards had to trust the wakeup, which in turn produced PHANTOM and SILENT (T15).

**OWRV:**
- **No partner ever completes a communication on the ALT's behalf.**
  - A plain partner stays pending (blocked in `output()`/`input()`, its data in its own buffer) until the
    ALT's `activate()` takes it.
  - An ALT partner that claims this ALT sets both state words to `CLAIMED(c)` in one critical section. It
    then waits for the reader side to perform the transfer; on a claimed guard, `activate()` cannot fail.
- So the evidence of readiness (a pending partner, or the ALT's own `CLAIMED(c)`) cannot disappear
  between the partner's offer and the ALT's `activate()`. Only the ALT itself removes it.
- A wakeup that finds nothing ready and no claim is therefore genuinely stale, and a new round is safe.
- The only readiness that can vanish is that of an ALT partner that has *registered* but not claimed, and
  that then selects another guard. The ALT's `activate()` claim then fails and a new round starts. The
  partner made progress, so this cannot repeat without bound (checked as the round bound).
- Buffered and timer guards already re-verify (semaphore count, `fired`), so they need no change and
  never touch the state word.

## Model extensions (`docs/formal/alt_owrv_extended.csp`, ProB 1.16.1): all as expected

**3a, symmetric crossing:** A ALTs {c1!, c2?}, B ALTs {c1?, c2!}, both guard orders for B, plus a stale
flag.
- Both select the same channel with exactly one communication: `[FD=` (divergence-free), deadlock-free,
  round bound all hold.
- **Mutation TOCTOU** (the claim checks both state words in one section and writes them in a later one):
  **fails** with crossing guard orders: `x_sel.0.2.false` (both ALTs claimed crosswise, two
  communications). With identical orders no crossing is possible, and it passes.
- A first mutation that only split the two *writes* passed, because the first write already makes the
  partner unclaimable. It is not kept.

**3b, mixed ALT:** A ALTs {c0 rendezvous, c1 buffered}; an ISR writes c1 (atomic push, token, snapshot
and signal); the task partner on c0 is a plain writer or an ALT writer; stale flag.
- `[FD=`, deadlock freedom, round bound and audit hold for both partner kinds.
