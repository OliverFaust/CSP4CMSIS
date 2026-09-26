# HimaxWE2-CSP4CMSIS: lost I2C completion (task blocks for good)

**Status:** found by reading the code (review rounds 4–5). Not reproduced on hardware. The sibling
repository is unchanged; this note only describes the problem and the fix.

**Checked revision:** `HimaxWE2-CSP4CMSIS` @ `77124fa` (2026-09-25). The same code is also in
`The_Way_of_Static_Process_Networks/GithubCode/CSP4CMSIS`.

## Affected applications

All four use the same pattern: start an interrupt-driven I2C transfer, then wait for its completion
callback on a **rendezvous** channel that the callback writes with `putFromISR()`.

| Application (`EPII_CM55M_APP_S/app/scenario_app/…`) | File | Channel | ISR callback | Waiting process |
|---|---|---|---|---|
| `csp4cmsis_kws_iic` | `csp4cmsis_spn.cpp` | `Channel<bool> g_pcf_i2c_isr_chan` (l. 80) | `pcf_i2c_callback()` (l. 82) | `Pcf8574Process::wait_for_i2c_isr()` (l. 285) |
| `csp4cmsis_kws_PCA9685` | `csp4cmsis_spn.cpp` | `Channel<bool> g_pca9685_i2c_isr_chan` (l. 80) | `pca9685_i2c_callback()` (l. 82) | `Pca9685Process::wait_for_i2c_isr()` (l. 334) |
| `csp4cmsis_kws_PCA9685_alt` | `csp4cmsis_spn.cpp` | `Channel<bool> g_pca9685_i2c_isr_chan` (l. 82) | `pca9685_i2c_callback()` (l. 84) | `Pca9685Process::wait_for_i2c_isr()` (l. 421) |
| `csp4cmsis_shake_detection` | `tests.cpp` | `Channel<bool> i2c_isr_chan` (l. 24) | `i2c_callback()` (l. 27) | `Adxl345Reader::wait_for_i2c_isr()` (l. 38) |

(The previous analysis round listed three applications; `csp4cmsis_shake_detection` has the same
pattern, so there are four.)

## The pattern

```cpp
static Channel<bool> g_pcf_i2c_isr_chan;                     // rendezvous (capacity 0)

extern "C" void pcf_i2c_callback(void) {                     // I2C completion ISR
    g_pcf_i2c_isr_chan.writer().putFromISR(true);            // return value ignored
}

void wait_for_i2c_isr() { bool dummy; i2c_sync >> dummy; }   // i2c_sync = the channel's reader

hx_drv_i2cm_interrupt_write(USE_DW_IIC_0, addr, buf, n, (void*)pcf_i2c_callback);
wait_for_i2c_isr();
```

## Failure

A rendezvous channel has no buffer: `putFromISR()` only succeeds if a reader is *already waiting* in
`>>`, and otherwise returns `false`. The transfer is started *before* the process reaches `>>`.
- If the completion interrupt fires in between, `putFromISR()` finds no reader and returns `false`.
  Nobody checks that, so the completion is lost.
- The process then enters `i2c_sync >> dummy` and **blocks forever**: no further callback comes, because
  nothing starts another transfer.
- This happens whenever the transfer is short enough, or the process is preempted long enough, e.g. by a
  higher-priority process or an interrupt burst between `hx_drv_i2cm_interrupt_write()` and
  `wait_for_i2c_isr()`. It is a race, so it can pass testing and fail in the field.

Adxl345 reads in `csp4cmsis_shake_detection` issue two transfers back to back (register address, then
data), so they are exposed twice per sample.

## Fix: a buffered channel of capacity 1

The completion is an *event that must not be lost*, so it needs a place to wait. With CSP4CMSIS 2.0, an ISR
can only write into a buffered channel anyway (rendezvous channels no longer have `putFromISR()`).

```cpp
static BufferedChannel<bool, 1> g_pcf_i2c_isr_chan;          // capacity 1, Block policy

extern "C" void pcf_i2c_callback(void) {
    (void)g_pcf_i2c_isr_chan.isrWriter().putFromISR(true);   // stored even if nobody waits yet
}

void wait_for_i2c_isr() { bool dummy; i2c_sync >> dummy; }   // unchanged
```

- **Why capacity 1 is enough:** each transfer produces exactly one callback, and the process consumes it
  before it starts the next transfer, so at most one completion is ever pending. With the Block policy,
  `putFromISR()` returns `false` only if a second completion arrives before the first was read, which
  would itself be a driver bug worth trapping (e.g. `if (!…putFromISR(true)) error_hook();`).
- **The reader side** (`Chanin<bool>`, `>>`) and the process code do not change.
- **With CSP4CMSIS 1.x** the same fix is `BufferedChannel<bool, 1>` with `writer().putFromISR(true)`.
  1.x has other defects in that path, so moving to 2.0 is preferable.
- **Requirement:** the callback's interrupt priority must be at or below
  `CSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY` (`Documentation/CSP4CMSIS_Configuration.md` §3), as for any
  ISR write.

## What else in the sibling projects needs changing for 2.0

For the full migration list per project, see `BUFFERED_CHANNEL_ANALYSIS.md`, "Review round 5", section
"Migration of sibling projects and book chapters". None of it has been done yet.
