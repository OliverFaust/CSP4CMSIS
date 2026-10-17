#!/usr/bin/env python3
"""Decode a CMSIS-View Event Recorder log (Semihosting variant) to CSV.

    decode.py -a app.elf -I RTX5.scvd log.evr -o events.csv [--summary s.txt]

Runs eventlist with the RTOS's .scvd file(s) and trace/CSP4CMSIS.scvd, and
writes one CSV row per event:

    time, component, event, thread, value

- time: eventlist's time stamp in seconds (timer count / EVENT_TIMESTAMP_FREQ).
  With --model-time the column is named model_time_s: on a simulator (FVP)
  the timer counts model cycles, which are not simulated seconds.
- component: taken from the event ID in the log, named by the .scvd files
  (eventlist 1.2.0 labels every event with the last component it read).
- thread: the thread the event names (CSP4CMSIS: val1; "ISR" for 0 in an
  interrupt handler; RTOS events: thread_id=, pxCurrentTCB=, ...).
- value: eventlist's value text; addresses of channels and processes that
  eventlist printed as numbers are resolved to symbols with nm.

The log is also read directly, to get each event's ID and raw time stamp:
eventlist does not print the ID. --summary writes counts per event and
whether the time stamps are monotonic.
"""
import argparse
import bisect
import collections
import csv
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
CSP_SCVD = os.path.normpath(os.path.join(HERE, '..', '..', 'trace', 'CSP4CMSIS.scvd'))
CSP_COMPONENT = 0x50

# Keys of RTOS event values that name a thread (RTX5, CMSIS-FreeRTOS).
THREAD_KEYS = ('thread_id', 'thread', 'pxCurrentTCB', 'pxNewTCB', 'pxTCB', 'xTaskToDelete', 'xTask')
ADDR_KEYS = ('channel', 'process')


def read_log(path, tail):
    """Records of an Event Recorder log file: (id, ts, irq, val1). Bytes
    after the last complete record are counted in tail[0]."""
    data = open(path, 'rb').read()
    i = 0
    while i + 4 <= len(data):
        typ, length = struct.unpack_from('<HH', data, i)
        if length < 12 or i + 4 + length > len(data):
            break
        ts, info = struct.unpack_from('<QI', data, i + 4)
        val1 = struct.unpack_from('<I', data, i + 16)[0] if typ != 3 and length >= 16 else None
        yield info & 0xFFFF, ts, info >> 31, val1
        i += 4 + length
    tail[0] = len(data) - i


def component_names(scvd_files):
    names = {0xFF: 'EventRecorder'}
    for f in scvd_files:
        for c in ET.parse(f).getroot().iter('component'):
            if c.get('no') is not None:
                names[int(c.get('no'), 0)] = c.get('brief') or c.get('name')
    return names


class Symbols:
    """Data/function symbols of an ELF file (nm), for address lookups."""
    def __init__(self, elf, nm):
        self.starts, self.syms = [], []
        out = subprocess.run([nm, '-C', '-S', '--defined-only', elf],
                             check=True, capture_output=True, text=True).stdout
        rows = []
        for line in out.splitlines():
            parts = line.split(None, 3)
            if len(parts) == 4 and len(parts[2]) == 1:
                rows.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        for start, size, name in sorted(rows):
            self.starts.append(start)
            self.syms.append((start, size, name))

    def name(self, addr):
        k = bisect.bisect_right(self.starts, addr) - 1
        for start, size, name in reversed(self.syms[max(k - 32, 0):k + 1]):
            if start <= addr < start + max(size, 1):
                return name if addr == start else '%s+0x%x' % (name, addr - start)
        return None


def run_eventlist(eventlist, elf, scvd_files, log):
    """eventlist's events as dicts (time, eventProperty, value), streamed
    from its XML output (a long log gives millions of events)."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'events.xml')
        cmd = [eventlist, '-a', elf]
        for f in scvd_files:
            cmd += ['-I', f]
        cmd += ['-f', 'xml', '-o', out, log]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if not os.path.exists(out):
            sys.exit('eventlist failed: %s%s' % (r.stdout, r.stderr))
        for _, el in ET.iterparse(out):
            if el.tag == 'events':
                yield {c.tag: (c.text or '') for c in el}
                el.clear()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', help='Event Recorder log (EVENT_LOG_FILENAME)')
    ap.add_argument('-a', '--elf', required=True, help='image that wrote the log')
    ap.add_argument('-I', dest='scvd', action='append', default=[], help='RTOS .scvd file (repeatable)')
    ap.add_argument('-o', '--output', required=True, help='CSV file')
    ap.add_argument('--summary', help='summary text file')
    ap.add_argument('--model-time', action='store_true', help='name the time column model_time_s (simulator)')
    ap.add_argument('--eventlist', default=os.environ.get('EVENTLIST', 'eventlist'))
    ap.add_argument('--nm', default=os.environ.get('NM', 'arm-none-eabi-nm'))
    args = ap.parse_args()

    eventlist = shutil.which(args.eventlist) or args.eventlist
    scvd = args.scvd + [CSP_SCVD]
    comps = component_names(scvd)
    syms = Symbols(args.elf, args.nm)
    tail = [0]
    records = read_log(args.log, tail)
    events = run_eventlist(eventlist, args.elf, scvd, args.log)

    counts = collections.Counter()
    decreases, last_ts, n, first_time, last_time = 0, None, 0, None, None
    time_col = 'model_time_s' if args.model_time else 'time_s'
    with open(args.output, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow([time_col, 'component', 'event', 'thread', 'value'])
        for ev in events:
            rec = next(records, None)
            if rec is None:
                sys.exit('eventlist decoded more events than the log has records')
            eid, ts, irq, val1 = rec
            n += 1
            if last_ts is not None and ts < last_ts:
                decreases += 1
            last_ts = ts
            comp_no = eid >> 8
            comp = comps.get(comp_no, '0x%02X' % comp_no)
            value = ev.get('value', '')
            thread = ''
            if comp_no == CSP_COMPONENT and val1 is not None:
                thread = 'ISR' if (val1 == 0 and irq) else '0x%08x' % val1
            else:
                m = re.search(r'\b(?:%s)=(0x[0-9A-Fa-f]+)' % '|'.join(THREAD_KEYS), value)
                if m:
                    thread = '0x%08x' % int(m.group(1), 16)
            # eventlist prints %S as a hex number (without 0x) when it finds
            # no symbol name.
            def resolve(m):
                name = syms.name(int(m.group(2), 16))
                return '%s=%s' % (m.group(1), name) if name else m.group(0)
            value = re.sub(r'\b(%s)=(0x[0-9A-Fa-f]+|[0-9A-Fa-f]{8})\b' % '|'.join(ADDR_KEYS), resolve, value)
            t = float(ev['time'])
            first_time = t if first_time is None else first_time
            last_time = t
            w.writerow(['%.8f' % t, comp, ev['eventProperty'], thread, value])
            counts[(comp, ev['eventProperty'])] += 1

    left = sum(1 for _ in records)
    if left:
        sys.exit('the log has %d records that eventlist did not decode' % left)
    if args.summary:
        csp = sum(k for (c, _), k in counts.items() if c == comps[CSP_COMPONENT])
        with open(args.summary, 'w') as f:
            f.write('events: %d (CSP4CMSIS %d, other %d); trailing bytes in log: %d\n'
                    % (n, csp, n - csp, tail[0]))
            f.write('time stamps: %s (%d decreases)\n' % ('monotonic' if decreases == 0 else 'NOT monotonic', decreases))
            f.write('%s: first %s, last %s\n' % (time_col, first_time, last_time))
            for (c, e), k in sorted(counts.items()):
                f.write('%8d  %s %s\n' % (k, c, e))


if __name__ == '__main__':
    main()
