# tools/trace

Decoding and checking CSP4CMSIS traces (`CSP4CMSIS_TRACE`).

## Hardware traces from the RAM ring backend

    ring2log.py ring.bin -o events.log --report ring_report.txt
    decode.py -a app.elf -I ../../trace/FreeRTOS_ring.scvd events.log \
        -o events.csv --summary summary.txt --stable stable.csv --perfetto trace.json
    csp_trace.py events.csv --stable stable.csv --out conf --mutate
    csp_trace.py --prove conf

`ring.bin` is the debugger's dump of the ring (header and records, see
`ring2log.py`). `ring2log.py` writes an Event Recorder log (Semihosting
format), the same input as an FVP trace, so the rest of the path is the
FVP's.

## eventlist

`decode.py` runs eventlist (CMSIS-Toolbox, version 1.2) to name events and
format their values. Its executable is, in this order: `--eventlist PATH`,
the environment variable `EVENTLIST`, `eventlist` on the `PATH`. Example:

    export EVENTLIST=/path/to/cmsis-toolbox/bin/eventlist

`decode.py --no-eventlist` is a fallback for FreeRTOS traces only:
`scvd_eval.py` formats the values from the `.scvd` files itself and does not
support every `.scvd` feature (typedefs and `%E` enumerations, used by
RTX5.scvd, are not). The decoded events, the stable points and the
conformance checks are the same as with eventlist; the CSV differs in
formatting (see `scvd_eval.py`).

## Other tools

- `PROBCLI`: the ProB command line (`csp_trace.py --prove`).
- `NM`: `nm` for symbols (default `arm-none-eabi-nm`).
