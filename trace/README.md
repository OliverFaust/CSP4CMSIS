# Event Recorder tracing

Opt-in: build the whole project with `CSP4CMSIS_TRACE` defined, add
CMSIS-View:Event Recorder, initialise it (or let the RTOS do it) and enable
component 0x50:

    EventRecorderEnable(EventRecordAll, 0x50U, 0x50U);

Without `CSP4CMSIS_TRACE` nothing changes and CMSIS-View is not needed. The
events are listed in `csp4cmsis/inc/csp/csp4cmsis_trace.h`; `CSP4CMSIS.scvd`
decodes them. Decode a log (Semihosting variant) to CSV with

    tools/trace/decode.py -a app.elf -I <RTOS>.scvd EventRecorder.log -o events.csv --summary summary.txt

(`eventlist` from the CMSIS-Toolbox and `arm-none-eabi-nm` on the PATH, or
`EVENTLIST=` / `NM=`). On a simulator pass `--model-time`: time stamps then
count model cycles, not simulated seconds.

## Results, FVP regression matrix (Step 1)

30 configurations: Corstone-300 (Cortex-M55) and MPS2 Cortex-M4; Arm Compiler
6.24 and GCC 14.2.1; FreeRTOS 11.3 (CMSIS-FreeRTOS adapter), RTX5 5.9.1 and
ST's FreeRTOS 10.3.1; -O0/-O2/-Os, heap-free and timer-priority variants.
Event Recorder 1.6.0, Semihosting variant, DWT cycle counter time stamps.

- Tracing off: in every configuration the loaded image is byte-identical to
  v3.0.1 and the test results are the same (PASS=36, heap-free PASS=37;
  FAIL=0 SKIP=0 REPLACED=4).
- Tracing on: the same results everywhere. CSP4CMSIS events decode in all 30
  (about 173,600 per run), RTOS thread events in the RTX5 and FreeRTOS 11.3
  configurations (ST's FreeRTOS records none). Logs complete, time stamps
  monotonic.
- Content checks, all 30: 500 RendezvousDone on each of the two channels of
  test T12; its 1000 AltSelected select guard 0 / 1 exactly when the channel
  that just paired is the first / second; one PutFromISR from an interrupt on
  each buffered channel of T2; every ProcessStart precedes the other events of
  its thread; two SpawnFailed in T28.
- Buffer overflow is not testable here: the Semihosting variant writes every
  record to the file as it is recorded. Deferred to hardware.

Cost (bytes, text = code + read-only data):

| | GCC -O2 | GCC -O0 | Arm Compiler -O2 | Arm Compiler -O0 |
|---|---|---|---|---|
| library objects, off (= v3.0.1) | 3650 | 5830 | 3775 | 5913 |
| library objects, on | 4061 (+411) | 6284 (+454) | 4201 (+426) | 6227 (+314) |
| whole image, on vs off (incl. channel templates, RTOS event code) | +7468 | +6756 | +8300 | +6896 |
| Event Recorder object (code / RAM) | 2394 / 1192 | 3294 / 1192 | 6970 / 1192 | 3896 / 1192 |

(Corstone-300, RTX5; library data/bss unchanged at 0/4. The whole-image
figure also contains RTX5's own event functions and the Event Recorder.)
