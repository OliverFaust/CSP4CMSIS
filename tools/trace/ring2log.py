#!/usr/bin/env python3
"""Convert a RAM trace ring dump into an Event Recorder log (Semihosting
variant), the input of decode.py.

    ring2log.py ring.bin -o events.log [--report report.txt]

ring.bin: the debugger's "dump binary memory" of struct csp_trace_ring: a
32-byte header followed by `capacity` records of 16 bytes.

Header (little endian): magic 'CSPR' (0x52505343), u16 format (1), u16
record size (16), u32 capacity (records, a power of two), u32 policy (0
overwrite the oldest, 1 stop when full), u32 write count (records written
since init, monotonic), u32 clock (Hz of the time stamps), u32 records
dropped (policy 1), u32 DWT_CTRL after init.

Record: u32 ts (DWT CYCCNT), u16 id (component << 8 | message), u16 seq
(write index mod 2^16), u32 a, u32 b. Events with more than two values use
consecutive records, as Event Recorder splits them: EventRecord4 is id, a, b
then 0x0001, c, d; EventRecordData is id, bytes 0-7 then 0xFF01 + i, 8 bytes
each (zero-padded: the length is not recorded; the data are strings).

Output: one Event Recorder record per event. The first is Event Recorder's
init record (0xFF00, values 0 and the time stamp frequency 1e9): time
stamps are nanoseconds from the first event kept. CYCCNT is unwrapped from
consecutive records (the backend's 1 s heartbeat keeps them less than one
wrap apart) at the clock in force: the clock records (0x5101) and
heartbeats (0x5100) of component 0x51 carry SystemCoreClock; before the
first one, the header's clock.

Marked in the output as Event Recorder data records 0xFF0E ("ring: ..."),
and in the report: records lost to overwriting (write count - capacity) or
dropped (policy 1), records whose seq is not their write index (torn: the
target was stopped while writing the slot), continuation records whose
first record was lost, heartbeat intervals that are not about 1 s, and
data events the backend cut at 64 bytes (its truncation record 0x5106,
a = the event's id, b = its full length, replaced by "ring: truncated").
"""
import argparse
import struct
import sys

MAGIC = 0x52505343
HEADER = struct.Struct('<IHHIIIIII')
RECORD = struct.Struct('<IHHII')
VAL4_NEXT, DATA_NEXT = 0x0001, 0xFF01
DATA_IDS = {0x5002, 0x5103}            # ProcessName, TaskName: EventRecordData events
CLOCK_IDS = {0x5100, 0x5101}           # heartbeat and clock records: a = SystemCoreClock
HEARTBEAT = 0x5100
TRUNCATED = 0x5106                     # a: id of the data event just written, b: its full length
MAX_DATA = 64                          # bytes the backend keeps of a data event
CSP_COMPONENT = 0x50
NO_CONTEXT = {0x02, 0x10}              # CSP events whose first value is not a context
MARK = 0xFF0E
TYPE_DATA, TYPE_VAL2, TYPE_VAL4 = 1, 2, 3
FREQ_OUT = 1000000000


def log_record(typ, eid, ts, irq, payload):
    info = (eid & 0xFFFF) | (0x80000000 if irq else 0)
    if typ == TYPE_DATA:
        info |= (len(payload) & 0x7FFF) << 16
    body = struct.pack('<QI', ts, info) + payload
    return struct.pack('<HH', typ, len(body)) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', help='dump of struct csp_trace_ring')
    ap.add_argument('-o', '--output', required=True, help='Event Recorder log (decode.py input)')
    ap.add_argument('--report', help='text report (default: stdout)')
    args = ap.parse_args()

    data = open(args.dump, 'rb').read()
    if len(data) < HEADER.size:
        sys.exit('%s: shorter than the header' % args.dump)
    magic, fmt, rsize, cap, policy, wcount, clock, dropped, dwt_ctrl = HEADER.unpack_from(data, 0)
    if magic != MAGIC or fmt != 1 or rsize != RECORD.size:
        sys.exit('%s: not a format 1 trace ring (magic 0x%08x, format %d, record size %d)'
                 % (args.dump, magic, fmt, rsize))
    if cap == 0 or cap & (cap - 1) or cap >= 65536:
        sys.exit('%s: capacity %d is not a power of two below 65536' % (args.dump, cap))
    if len(data) < HEADER.size + cap * RECORD.size:
        sys.exit('%s: %d bytes, the ring needs %d' % (args.dump, len(data), HEADER.size + cap * RECORD.size))

    first = max(0, wcount - cap)
    overwritten = first
    raw, torn = [], []
    for k in range(first, wcount):
        ts, rid, seq, a, b = RECORD.unpack_from(data, HEADER.size + (k & (cap - 1)) * RECORD.size)
        if seq != k & 0xFFFF:
            torn.append(k)
            raw.append(None)
        else:
            raw.append((k, ts, rid, a, b))

    # Group records into events.
    events, orphans, i = [], 0, 0
    while i < len(raw):
        r = raw[i]
        if r is None:
            i += 1
            continue
        k, ts, rid, a, b = r
        if rid == VAL4_NEXT or DATA_NEXT <= rid < DATA_NEXT + 0x80:
            orphans += 1                     # its first record was overwritten or torn
            i += 1
            continue
        nxt = raw[i + 1] if i + 1 < len(raw) else None
        if rid in DATA_IDS:
            chunks, j = [struct.pack('<II', a, b)], i + 1
            while j < len(raw) and raw[j] is not None and raw[j][2] == DATA_NEXT + len(chunks) - 1:
                chunks.append(struct.pack('<II', raw[j][3], raw[j][4]))
                j += 1
            events.append((k, ts, rid, TYPE_DATA, b''.join(chunks).rstrip(b'\0')))
            i = j
        elif nxt is not None and nxt[2] == VAL4_NEXT:
            events.append((k, ts, rid, TYPE_VAL4, struct.pack('<IIII', a, b, nxt[3], nxt[4])))
            i += 2
        else:
            events.append((k, ts, rid, TYPE_VAL2, struct.pack('<II', a, b)))
            i += 1

    # Unwrap the 32-bit cycle counts into nanoseconds.
    freq = next((struct.unpack_from('<I', p)[0] for _, _, rid, _, p in events if rid in CLOCK_IDS), clock) or clock
    out, ns, prev, beats, bad_beats = [], 0.0, None, [], []
    marks = []
    if overwritten or dropped:
        marks.append('ring: %d records overwritten, %d dropped before this point (write count %d, capacity %d)'
                     % (overwritten, dropped, wcount, cap))
    for k, ts, rid, typ, payload in events:
        if prev is not None:
            ns += ((ts - prev) & 0xFFFFFFFF) * 1e9 / freq
        prev = ts
        if rid in CLOCK_IDS:
            freq = struct.unpack_from('<I', payload)[0] or freq
        if rid == HEARTBEAT:
            beat = struct.unpack_from('<I', payload, 4)[0]
            if beats and beat == beats[-1][0] + 1 and not 0.9e9 < ns - beats[-1][1] < 1.1e9:
                bad_beats.append((beats[-1][0], beat, (ns - beats[-1][1]) / 1e9))
            beats.append((beat, ns))
        out.append((k, int(round(ns)), rid, typ, payload))

    with open(args.output, 'wb') as f:
        f.write(log_record(TYPE_VAL2, 0xFF00, 0, False, struct.pack('<II', 0, FREQ_OUT)))
        for text in marks:
            f.write(log_record(TYPE_DATA, MARK, 0, False, text.encode()))
        torn_set = set(torn)
        last_k = first - 1
        truncated = 0
        for k, ts, rid, typ, payload in out:
            gap = [t for t in range(last_k + 1, k) if t in torn_set]
            if gap:
                f.write(log_record(TYPE_DATA, MARK, ts, False,
                                   ('ring: %d torn record(s) dropped (write index %d..%d)'
                                    % (len(gap), gap[0], gap[-1])).encode()))
            last_k = k
            if rid == TRUNCATED:
                eid, length = struct.unpack('<II', payload)
                truncated += 1
                f.write(log_record(TYPE_DATA, MARK, ts, False,
                                   ('ring: truncated: data event 0x%04X, %d of %d bytes recorded'
                                    % (eid, MAX_DATA, length)).encode()))
                continue
            irq = (rid >> 8) == CSP_COMPONENT and (rid & 0xFF) not in NO_CONTEXT and typ != TYPE_DATA \
                and struct.unpack_from('<I', payload)[0] & 0x80000000
            f.write(log_record(typ, rid, ts, irq, payload))

    span = out[-1][1] / 1e9 if out else 0.0
    lines = [
        'ring: capacity %d records, policy %s, write count %d, clock %d Hz, DWT_CTRL 0x%08x'
        % (cap, 'overwrite oldest' if policy == 0 else 'stop when full', wcount, clock, dwt_ctrl),
        'records kept: %d (write index %d..%d); overwritten %d; dropped %d; torn %d; orphan continuation records %d'
        % (wcount - first, first, wcount - 1, overwritten, dropped, len(torn), orphans),
        'events: %d over %.6f s (%.0f events/s, %.0f records/s)'
        % (len(out), span, len(out) / span if span else 0, (wcount - first) / span if span else 0),
        'heartbeats: %d (%s); intervals not about 1 s: %s'
        % (len(beats), 'beats %d..%d' % (beats[0][0], beats[-1][0]) if beats else 'none',
           ', '.join('%d->%d %.3f s' % b for b in bad_beats) or 'none'),
        'data events truncated at %d bytes: %d' % (MAX_DATA, truncated),
    ]
    text = '\n'.join(lines) + '\n'
    if args.report:
        open(args.report, 'w').write(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
