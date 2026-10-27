#!/usr/bin/env python3
"""Decode a CMSIS-View Event Recorder log (Semihosting variant) to CSV and/or
a Perfetto (Chrome Trace Event JSON) trace.

    decode.py -a app.elf -I RTX5.scvd log.evr -o events.csv [--summary s.txt]
    decode.py -a app.elf -I RTX5.scvd log.evr --perfetto trace.json [--window T0:T1]

Runs eventlist with the RTOS's .scvd file(s) and trace/CSP4CMSIS.scvd and
reads the log itself (eventlist prints neither event IDs nor all values).
eventlist (CMSIS-Toolbox, version 1.2) is taken from --eventlist, else the
environment variable EVENTLIST, else the PATH. --no-eventlist is a fallback
for FreeRTOS traces only: scvd_eval.py formats the values from the .scvd
files itself and does not support every .scvd feature (RTX5.scvd fails).

CSV (-o): one row per event: time, component, event, thread, value.
- time: seconds (timer count / EVENT_TIMESTAMP_FREQ). With --model-time the
  column is named model_time_s: on a simulator (FVP) the timer counts model
  cycles, which are not simulated seconds.
- component: from the event ID, named by the .scvd files (eventlist 1.2.0
  labels every event with the last component it read).
- thread: CSP4CMSIS events: the context, shown as the thread id, "irq N"
  (handler mode, exception number N) or "pre-kernel"; RendezvousDone: the
  thread that recorded it. RTOS events: thread_id=, pxCurrentTCB=, ...
- value: eventlist's value text; addresses of channels, processes and other
  objects resolved to symbols with nm; RendezvousDone writer/reader named.

Perfetto (--perfetto, open in ui.perfetto.dev): per thread a "running" track
(from the RTOS switch events), a "waiting" track (Wait... to Resume) and an
"events" track (CSP4CMSIS events as instants, rendezvous as short slices with
a flow arrow from writer to reader); an "interrupts" group with one track per
exception number; a "buffered channels" group with one occupancy counter per
channel. Buffered channels: each buffer is replayed as a FIFO in record order;
a put is a short slice "put <channel>" on its context's track (thread or irq),
a get a slice "get <channel> <- <source>" with a flow arrow (category
"buffered") from the put whose value it took. Puts that are never read are
named by their outcome: "(overwritten, never read)" (KeepNewest), "(dropped)"
(KeepOldest), "(still buffered at the end)". Args: the raw event and value,
the replayed depth and the library's element count after the event. Time in
microseconds of the log's time stamps.
Selection: --window T0:T1 (seconds), --threads NAME,... ; --marker COMP:MSG
with --test NAME: markers are records of component COMP, message MSG whose
data is a name, each placed at the end of a test; the window is from the
marker before the one named NAME to that one.

Library state (conformance.py), from the whole log whatever the selection:
ring slots of buffered channels checked against the FIFO replay, WaitAlt
guard masks resolved through AltGuard to (channel, direction) or timeouts,
and stable points (every live process in an open wait, the idle thread
running) with their offers and a classification. --checks FILE writes these
results as JSON, --stable FILE the stable points (CSV if FILE ends in .csv,
else JSON); the Perfetto trace gets a "stable points" track. The idle thread
is the one whose control block's symbol contains "idle" (RTX5
os_idle_thread_cb, FreeRTOS xIdleTaskTCB), or --idle NAME.

--summary writes counts per event, whether the time stamps are monotonic,
the results of these checks and ring2log.py's "ring: ..." notes (records
overwritten, dropped or torn, data events truncated at 64 bytes).
"""
import argparse
import bisect
import collections
import csv
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True    # no __pycache__ next to the tools
import conformance  # noqa: E402
import scvd_eval  # noqa: E402
CSP_SCVD = os.path.normpath(os.path.join(HERE, '..', '..', 'trace', 'CSP4CMSIS.scvd'))
CSP_COMPONENT = 0x50

# Keys of RTOS event values that name a thread (RTX5, CMSIS-FreeRTOS).
THREAD_KEYS = ('thread_id', 'thread', 'pxCurrentTCB', 'pxNewTCB', 'pxTCB', 'xTaskToDelete', 'xTask')
ADDR_KEYS = ('channel', 'process', 'alt', 'barrier', 'join', 'object')

# CSP4CMSIS message numbers (csp4cmsis_trace.h)
PROCESS_START, PROCESS_END, PROCESS_NAME = 0x00, 0x01, 0x02
RENDEZVOUS_DONE, ALT_SELECTED, ALT_GUARD, ALT_DEADLINE = 0x10, 0x20, 0x21, 0x22
BUFFERED_PUT, BUFFERED_GET, PUT_FROM_ISR, DROPPED_NEW, OVERWROTE_OLDEST = 0x30, 0x31, 0x32, 0x33, 0x34
SPAWN_FAILED = 0x40
WAIT_KIND = {0x60: 'channel', 0x61: 'channel (full)', 0x62: 'channel (empty)', 0x63: 'ALT',
             0x64: 'barrier', 0x65: 'join'}
WAIT_CHANNEL = 0x60
RESUME, RESUME_TIMEOUT = 0x6E, 0x6F

EVENT_TYPE_DATA, EVENT_TYPE_VAL2, EVENT_TYPE_VAL4 = 1, 2, 3


def read_log(path, tail):
    """Records of an Event Recorder log file: (id, ts, irq, values, data).
    values: 2 or 4 integers (None for a data record); data: bytes or None.
    Bytes after the last complete record are counted in tail[0]."""
    data = open(path, 'rb').read()
    i = 0
    while i + 4 <= len(data):
        typ, length = struct.unpack_from('<HH', data, i)
        if length < 12 or i + 4 + length > len(data):
            break
        ts, info = struct.unpack_from('<QI', data, i + 4)
        vals = payload = None
        if typ == EVENT_TYPE_VAL2:
            vals = struct.unpack_from('<II', data, i + 16)
        elif typ == EVENT_TYPE_VAL4:
            vals = struct.unpack_from('<IIII', data, i + 16)
        elif typ == EVENT_TYPE_DATA:
            payload = data[i + 16:i + 4 + length]
        yield info & 0xFFFF, ts, info >> 31, vals, payload
        i += 4 + length
    tail[0] = len(data) - i


def context(val1):
    """CSP4CMSIS context value (csp4cmsis_trace.h) as text."""
    if val1 == 0:
        return 'pre-kernel'
    if val1 & 0x80000000:
        return 'irq %d' % (val1 & 0x1FF)
    return '0x%08x' % val1


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

    def lookup(self, addr):
        """(symbol, offset) of the object containing addr, or None."""
        k = bisect.bisect_right(self.starts, addr) - 1
        for start, size, name in reversed(self.syms[max(k - 32, 0):k + 1]):
            if start <= addr < start + max(size, 1):
                return name, addr - start
        return None

    def name(self, addr):
        s = self.lookup(addr)
        if s is None:
            return None
        return s[0] if s[1] == 0 else '%s+0x%x' % s

    def short(self, addr):
        """Symbol of the object containing addr, without namespace prefix and offset."""
        s = self.lookup(addr)
        return re.sub(r'^\(anonymous namespace\)::', '', s[0]) if s else None


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


class Names:
    """Thread names: CSProcess::name() from ProcessName, else the symbol of
    the thread's control block (static allocation), else 'thread 0x...'.
    Processes also get a role summary, from the channels they used:
    "Merger — ALT in: a, b; out: c"."""
    def __init__(self, syms):
        self.syms, self.process = syms, {}
        self.roles = collections.defaultdict(lambda: {'ALT in': {}, 'in': {}, 'out': {}})
        self.pending_in = {}              # thread -> channel read, until its next event shows how

    def get(self, tid):
        if tid in self.process:
            return self.process[tid]
        s = self.syms.short(tid)
        return s if s else 'thread 0x%08x' % tid

    def display(self, tid):
        name = self.get(tid)
        if tid not in self.process or tid not in self.roles:
            return name
        r = self.roles[tid]
        parts = ['%s: %s' % (k, ', '.join(r[k])) for k in ('ALT in', 'in', 'out') if r[k]]
        return name + (' \u2014 ' + '; '.join(parts) if parts else '')

    def channel(self, addr):
        return self.syms.short(addr) or '0x%08x' % addr

    def csp(self, msg, vals):
        """Role bookkeeping for one CSP4CMSIS event."""
        if vals is None:
            return
        if msg == ALT_GUARD:
            return                        # describes the Alternative's guards: no role
        if msg == RENDEZVOUS_DONE:
            writer, reader, chan = vals[0], vals[1], vals[2]
            self.resolve(writer, False)
            self.roles[writer]['out'][self.channel(chan)] = True
            self.resolve(reader, False)
            self.pending_in[reader] = chan
            return
        tid = vals[0]
        if tid == 0 or tid & 0x80000000:
            return
        if msg in WAIT_KIND or msg in (RESUME, RESUME_TIMEOUT, ALT_DEADLINE):
            return                        # the ALT's own waits come between read and AltSelected
        if msg == ALT_SELECTED:
            self.resolve(tid, True)
            return
        self.resolve(tid, False)
        if msg in (BUFFERED_PUT, DROPPED_NEW, OVERWROTE_OLDEST):
            self.roles[tid]['out'][self.channel(vals[1])] = True
        elif msg == BUFFERED_GET:
            self.pending_in[tid] = vals[1]

    def resolve(self, tid, by_alt):
        chan = self.pending_in.pop(tid, None)
        if chan is not None:
            self.roles[tid]['ALT in' if by_alt else 'in'][self.channel(chan)] = True


class Perfetto:
    """Chrome Trace Event JSON, written as events arrive (one per line)."""
    RUN, WAIT, EVENTS = 1, 2, 3

    def __init__(self, path, names, syms, window, threads, has_switch_events):
        self.f = open(path, 'w')
        self.f.write('{"traceEvents":[\n')
        self.first = True
        self.names, self.syms = names, syms
        self.t0, self.t1 = window
        self.threads = threads            # set of names, or None
        self.pids = {}                    # thread id -> pid
        self.running = None               # (thread, start us)
        self.waits = {}                   # thread -> (start us, kind, object)
        self.deadlines = {}               # thread -> (tick, deadline tick) of its next ALT wait
        self.fifo = collections.defaultdict(collections.deque)   # channel address -> puts not yet taken
        self.capacity = {}                # channel name -> capacity
        self.carried_in = {}              # channel name -> elements at the window start
        self.irqs = set()
        self.flow = 0
        self.stats = collections.Counter()
        self.expect_rv = collections.Counter()
        self.expect_bf = collections.Counter()
        self.pre_kernel = False
        self.expect_run = collections.Counter()
        self.has_switch = has_switch_events
        self.occ_mismatch = 0
        self.last_us = 0.0
        self.started = False

    def emit(self, ev):
        self.f.write(('' if self.first else ',\n') + json.dumps(ev, separators=(',', ':')))
        self.first = False

    def inside(self, us):
        return (self.t0 is None or us >= self.t0) and (self.t1 is None or us <= self.t1)

    def wanted(self, tid):
        return self.threads is None or self.names.get(tid) in self.threads

    def pid(self, tid):
        if tid not in self.pids:
            self.pids[tid] = 10 + len(self.pids)
        return self.pids[tid]

    def slice(self, tid, track, name, start, end, args=None):
        """Complete event clipped to the window; False if outside."""
        if self.t0 is not None:
            start = max(start, self.t0)
        if self.t1 is not None:
            end = min(end, self.t1)
        if end < start or not self.wanted(tid):
            return False
        p = self.pid(tid)
        ev = {'ph': 'X', 'name': name, 'pid': p, 'tid': p * 10 + track, 'ts': start, 'dur': end - start}
        if args:
            ev['args'] = args
        self.emit(ev)
        return True

    def instant(self, pid, tid, name, us, args):
        self.emit({'ph': 'i', 's': 't', 'name': name, 'pid': pid, 'tid': tid, 'ts': us, 'args': args})

    def switch_in(self, tid, us):
        if self.running is not None:
            self.switch_out(self.running[0], us)
        self.running = (tid, us)
        if self.inside(us) and self.wanted(tid):
            self.expect_run[tid] += 1     # switch-ins in the window (named at the end)

    def switch_out(self, tid, us):
        if self.running is not None and self.running[0] == tid:
            start = self.running[1]
            # a slice that began before the window is named apart (no switch-in in the window)
            name = 'running' if self.t0 is None or start >= self.t0 else 'running (from before window)'
            self.slice(tid, self.RUN, name, start, us)
            self.running = None

    def csp(self, msg, us, vals, irq, value):
        self.last_us = us
        a = vals[0] if vals else 0
        handler = vals is not None and (a & 0x80000000) != 0
        if msg == ALT_GUARD:
            return                        # described in the ALT's WaitAlt slices (args)
        if msg == ALT_DEADLINE:
            self.deadlines[a] = (vals[2], vals[3])
            return
        if msg in WAIT_KIND:
            kind = WAIT_KIND[msg]
            if msg == WAIT_CHANNEL:
                kind = 'channel (%s)' % ('write' if vals[1] & 1 else 'read')
            self.waits[a] = (us, kind, vals[1])
            return
        if msg in (RESUME, RESUME_TIMEOUT):
            w = self.waits.pop(a, None)
            if w is None:
                self.stats['resume_without_wait'] += 1
                return
            start, kind, obj = w
            if obj != vals[1]:
                self.stats['resume_other_object'] += 1
            label = self.syms.short(obj & ~1) or '0x%08x' % (obj & ~1)
            args = {'object': label, 'timeout': msg == RESUME_TIMEOUT}
            if len(vals) == 4:            # a wait with a deadline
                args.update({'wait_tick': self.deadlines.get(a, (None,))[0], 'deadline_tick': vals[3],
                             'resume_tick': vals[2]})
            self.deadlines.pop(a, None)
            if self.slice(a, self.WAIT, 'waiting on %s %s' % (kind, label), start, us, args):
                self.stats['wait_slices'] += 1
                if msg == RESUME_TIMEOUT:
                    self.stats['timeout_slices'] += 1
            return
        if not self.inside(us):
            if msg in BUFFERED_EVENTS:
                self.buffered(msg, us, vals, value, False)
            return
        if not self.started:              # counters start at the window with their current value
            self.started = True
            for chan, q in self.fifo.items():
                label = self.syms.short(chan) or '0x%08x' % chan
                self.carried_in[label] = len(q)
                self.emit({'ph': 'C', 'name': label, 'pid': 2, 'ts': self.t0 if self.t0 is not None else us,
                           'args': {'elements': len(q)}})
        name = EVENT_NAME.get(msg, '0x%02X' % msg)
        args = {'value': value}
        if msg == RENDEZVOUS_DONE:
            writer, reader, chan = vals[0], vals[1], vals[2]
            label = self.syms.short(chan) or '0x%08x' % chan
            if self.wanted(writer) and self.wanted(reader):
                self.flow += 1
                for tid, key in ((writer, 'flow_out'), (reader, 'flow_in')):
                    p = self.pid(tid)
                    self.emit({'ph': 'X', 'cat': 'rendezvous', 'name': 'rendezvous ' + label, 'pid': p,
                               'tid': p * 10 + self.EVENTS, 'ts': us, 'dur': 0.001, 'bind_id': self.flow, key: True,
                               'args': {'channel': label, 'writer': self.names.get(writer),
                                        'reader': self.names.get(reader)}})
                self.expect_rv[label] += 1
            return
        if msg in BUFFERED_EVENTS:
            self.buffered(msg, us, vals, value, True)
            return
        if handler:
            n = a & 0x1FF
            if n not in self.irqs:
                self.irqs.add(n)
            self.instant(3, 30000 + n, name, us, args)
        elif a and self.wanted(a):
            p = self.pid(a)
            self.instant(p, p * 10 + self.EVENTS, name, us, args)

    def context_track(self, ctx):
        """(pid, tid) of the track for events of a context, or None (thread not selected)."""
        if ctx & 0x80000000:
            self.irqs.add(ctx & 0x1FF)
            return 3, 30000 + (ctx & 0x1FF)
        if ctx == 0:
            self.pre_kernel = True
            return 4, 40000
        if not self.wanted(ctx):
            return None
        p = self.pid(ctx)
        return p, p * 10 + self.EVENTS

    def context_name(self, ctx):
        return context(ctx) if ctx == 0 or ctx & 0x80000000 else self.names.get(ctx)

    def buffered(self, msg, us, vals, value, inside):
        """Replays the channel's buffer as a FIFO in record order: every put is
        one record (BufferedPut, PutFromISR, OverwroteOldest, DroppedNew) and
        every get takes the oldest put; a get gets a flow arrow from that put."""
        chan = vals[1]
        label = self.syms.short(chan) or '0x%08x' % chan
        count, self.capacity[label] = conformance.unpack_count(vals[2])
        q = self.fifo[chan]
        rec = {'us': us, 'ctx': vals[0], 'channel': label, 'event': EVENT_NAME[msg], 'value': value,
               'elements_after': count, 'inside': inside}
        source = None
        if msg in (BUFFERED_PUT, PUT_FROM_ISR):
            q.append(rec)
        elif msg == OVERWROTE_OLDEST:
            self.put_slice(q.popleft() if q else None, 'overwritten, never read')
            q.append(rec)
        elif msg == BUFFERED_GET:
            source = q.popleft() if q else None
        rec['depth'] = len(q)
        if len(q) != count:
            self.occ_mismatch += 1
        if msg == DROPPED_NEW:
            self.put_slice(rec, 'dropped')
        if msg == BUFFERED_GET:
            self.get_slice(rec, source)
        if inside:
            self.emit({'ph': 'C', 'name': label, 'pid': 2, 'ts': us, 'args': {'elements': len(q)}})

    def put_slice(self, rec, outcome, flow=None):
        """The put's slice, emitted once its outcome is known."""
        if rec is None or not rec['inside']:
            return
        track = self.context_track(rec['ctx'])
        if track is None:
            return
        name = 'put ' + rec['channel'] + ('' if outcome == 'read' else ' (%s)' % outcome)
        ev = {'ph': 'X', 'cat': 'buffered', 'name': name, 'pid': track[0], 'tid': track[1],
              'ts': rec['us'], 'dur': 0.001,
              'args': {'channel': rec['channel'], 'context': self.context_name(rec['ctx']), 'outcome': outcome,
                       'event': rec['event'], 'value': rec['value'], 'depth': rec['depth'],
                       'elements_after': rec['elements_after']}}
        if flow is not None:
            ev.update({'bind_id': flow, 'flow_out': True})
        self.emit(ev)
        self.stats['buffered_put_slices'] += 1

    def get_slice(self, rec, source):
        if not rec['inside']:
            if source is not None and source['inside']:
                self.put_slice(source, 'read after the window')
            return
        track = self.context_track(rec['ctx'])
        if source is None:
            self.stats['buffered_get_without_source'] += 1
            src_name = '?'
        else:
            src_name = self.context_name(source['ctx'])
        flow = None
        if source is not None:
            src_track = self.context_track(source['ctx']) if source['inside'] else None
            if track is not None and src_track is not None:
                self.flow += 1
                flow = self.flow
            if not source['inside']:
                self.stats['buffered_get_source_before_window'] += 1
            self.put_slice(source, 'read', flow)
        if track is None:
            return
        ev = {'ph': 'X', 'cat': 'buffered', 'name': 'get %s ← %s' % (rec['channel'], src_name),
              'pid': track[0], 'tid': track[1], 'ts': rec['us'], 'dur': 0.001,
              'args': {'channel': rec['channel'], 'context': self.context_name(rec['ctx']),
                       'source': None if source is None else src_name,
                       'source_before_window': source is not None and not source['inside'],
                       'event': rec['event'], 'value': rec['value'], 'depth': rec['depth'],
                       'elements_after': rec['elements_after']}}
        if flow is not None:
            ev.update({'bind_id': flow, 'flow_in': True})
            self.expect_bf[rec['channel']] += 1
        self.emit(ev)

    def stable_points(self, points):
        """Track "stable points": one slice per point, named by its classification."""
        n = 0
        for p in points:
            if self.slice_raw(5, 50000, 'stable: ' + p['classification'], p['start_us'], p['end_us'],
                              {'classification': p['classification'], 'reason': p['reason'],
                               'offers': ['%s: %s' % (self.names.get(o['thread']), o['offer']) for o in p['offers']],
                               'open_at_end': p['open_at_end']}):
                n += 1
        if n:
            self.emit({'ph': 'M', 'name': 'process_name', 'pid': 5, 'args': {'name': 'stable points'}})
            self.emit({'ph': 'M', 'name': 'thread_name', 'pid': 5, 'tid': 50000, 'args': {'name': 'stable'}})
        self.stats['stable_slices'] = n

    def slice_raw(self, pid, tid, name, start, end, args):
        if self.t0 is not None:
            start = max(start, self.t0)
        if self.t1 is not None:
            end = min(end, self.t1)
        if end < start:
            return False
        self.emit({'ph': 'X', 'name': name, 'pid': pid, 'tid': tid, 'ts': start, 'dur': max(end - start, 0.001),
                   'args': args})
        return True

    def close(self, end_us, meta):
        if self.running is not None:
            self.switch_out(self.running[0], end_us)
        for tid, (start, kind, obj) in self.waits.items():   # still blocked at the end
            label = self.syms.short(obj & ~1) or '0x%08x' % (obj & ~1)
            if self.slice(tid, self.WAIT, 'waiting on %s %s (no resume)' % (kind, label), start, end_us,
                          {'object': label, 'timeout': False}):
                self.stats['wait_open_at_end'] += 1
        for q in self.fifo.values():
            for rec in q:
                self.put_slice(rec, 'still buffered at the end')
        for tid, p in self.pids.items():
            name = self.names.display(tid)
            self.emit({'ph': 'M', 'name': 'process_name', 'pid': p, 'args': {'name': name}})
            for track, tname in ((self.RUN, 'running'), (self.WAIT, 'waiting'), (self.EVENTS, 'events')):
                self.emit({'ph': 'M', 'name': 'thread_name', 'pid': p, 'tid': p * 10 + track, 'args': {'name': tname}})
        self.emit({'ph': 'M', 'name': 'process_name', 'pid': 2, 'args': {'name': 'buffered channels'}})
        self.emit({'ph': 'M', 'name': 'process_name', 'pid': 3, 'args': {'name': 'interrupts'}})
        for n in sorted(self.irqs):
            self.emit({'ph': 'M', 'name': 'thread_name', 'pid': 3, 'tid': 30000 + n, 'args': {'name': 'irq %d' % n}})
        if self.pre_kernel:
            self.emit({'ph': 'M', 'name': 'process_name', 'pid': 4, 'args': {'name': 'pre-kernel'}})
            self.emit({'ph': 'M', 'name': 'thread_name', 'pid': 4, 'tid': 40000, 'args': {'name': 'pre-kernel'}})
        meta.update({
            'running_slices': self.has_switch,
            'channel_capacity': self.capacity,
            'buffered_carried_in': self.carried_in,
            'expected_flows_per_channel': dict(self.expect_rv),
            'expected_buffered_flows_per_channel': dict(self.expect_bf),
            'thread_selection': sorted(self.threads) if self.threads is not None else None,
            'expected_switch_ins_per_thread': dict(sum((collections.Counter({self.names.display(t): k})
                                                        for t, k in self.expect_run.items()), collections.Counter())),
            'trace_end_us': end_us,
            'occupancy_mismatches': self.occ_mismatch,
            'stats': dict(self.stats),
        })
        if not self.has_switch:
            meta['running_slices_note'] = 'no RTOS switch events in this log: running slices omitted'
        self.f.write('\n],"displayTimeUnit":"ns","otherData":%s}\n' % json.dumps(meta))
        self.f.close()


EVENT_NAME = {PROCESS_START: 'ProcessStart', PROCESS_END: 'ProcessEnd', PROCESS_NAME: 'ProcessName',
              RENDEZVOUS_DONE: 'RendezvousDone', ALT_SELECTED: 'AltSelected', BUFFERED_PUT: 'BufferedPut',
              BUFFERED_GET: 'BufferedGet', PUT_FROM_ISR: 'PutFromISR', DROPPED_NEW: 'DroppedNew',
              OVERWROTE_OLDEST: 'OverwroteOldest', SPAWN_FAILED: 'SpawnFailed', ALT_GUARD: 'AltGuard'}
BUFFERED_EVENTS = (BUFFERED_PUT, BUFFERED_GET, PUT_FROM_ISR, DROPPED_NEW, OVERWROTE_OLDEST)


def write_stable(path, points, names):
    if path.endswith('.csv'):
        with open(path, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['start_us', 'end_us', 'classification', 'reason', 'offers'])
            for p in points:
                w.writerow(['%.3f' % p['start_us'], '%.3f' % p['end_us'] + (' (end of log)' if p['open_at_end'] else ''),
                            p['classification'], p['reason'],
                            '; '.join('%s: %s' % (o.get('name') or names.get(o['thread']), o['offer']) for o in p['offers'])])
    else:
        json.dump([dict(p, offers=[{'thread': o.get('name') or names.get(o['thread']), 'offer': o['offer']} for o in p['offers']])
                   for p in points], open(path, 'w'), indent=1)


def parse_hex(value, key):
    m = re.search(r'\b%s=(0x[0-9A-Fa-f]+)' % key, value)
    return int(m.group(1), 16) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', help='Event Recorder log (EVENT_LOG_FILENAME)')
    ap.add_argument('-a', '--elf', required=True, help='image that wrote the log')
    ap.add_argument('-I', dest='scvd', action='append', default=[], help='RTOS .scvd file (repeatable)')
    ap.add_argument('-o', '--output', help='CSV file')
    ap.add_argument('--summary', help='summary text file')
    ap.add_argument('--perfetto', help='Chrome Trace Event JSON file (ui.perfetto.dev)')
    ap.add_argument('--window', help='T0:T1, seconds (either side may be empty)')
    ap.add_argument('--threads', help='comma-separated thread names (Perfetto)')
    ap.add_argument('--marker', help='COMP:MSG of marker records (data: a name, at the end of each test)')
    ap.add_argument('--test', help='with --marker: the window from the previous marker to this one')
    ap.add_argument('--model-time', action='store_true', help='time stamps count simulator model cycles')
    ap.add_argument('--checks', help='JSON file: ring-slot, ALT-offer and stable-point results')
    ap.add_argument('--stable', help='stable points: CSV (.csv) or JSON file')
    ap.add_argument('--idle', help='comma-separated names of the idle thread(s) (default: symbol contains "idle")')
    ap.add_argument('--eventlist', default=os.environ.get('EVENTLIST', 'eventlist'),
                    help='the eventlist executable (default: $EVENTLIST, else eventlist on the PATH)')
    ap.add_argument('--no-eventlist', action='store_true',
                    help='fallback, FreeRTOS traces only: name the events and format their values from the '
                         '.scvd files (scvd_eval.py) instead of running eventlist')
    ap.add_argument('--nm', default=os.environ.get('NM', 'arm-none-eabi-nm'))
    args = ap.parse_args()
    if not (args.output or args.perfetto or args.summary or args.checks or args.stable):
        ap.error('nothing to write: give -o, --perfetto, --summary, --checks and/or --stable')

    eventlist = shutil.which(args.eventlist) or args.eventlist
    if not args.no_eventlist and not os.access(eventlist, os.X_OK):
        sys.exit('eventlist not found (%s): give --eventlist PATH or set EVENTLIST (CMSIS-Toolbox eventlist 1.2); '
                 '--no-eventlist is a FreeRTOS-only fallback' % args.eventlist)
    scvd = args.scvd + [CSP_SCVD]
    comps = component_names(scvd)
    syms = Symbols(args.elf, args.nm)
    names = Names(syms)

    # Window (microseconds). --test needs the markers first: one pass over the raw log.
    t0 = t1 = None
    if args.window:
        a, b = args.window.split(':')
        t0 = float(a) * 1e6 if a else None
        t1 = float(b) * 1e6 if b else None
    freq = None
    if args.test:
        if not args.marker:
            ap.error('--test needs --marker COMP:MSG')
        mc, mm = (int(x, 0) for x in args.marker.split(':'))
        prev, found = 0, None
        for eid, ts, irq, vals, payload in read_log(args.log, [0]):
            if eid == 0xFF00 and vals:
                freq = vals[1]
            if eid == (mc << 8 | mm) and payload is not None:
                if payload.split(b'\0')[0].decode('ascii', 'replace').strip() == args.test:
                    found = (prev, ts)
                    break
                prev = ts
        if found is None or not freq:
            sys.exit('marker %s not found' % args.test)
        t0, t1 = found[0] * 1e6 / freq, found[1] * 1e6 / freq

    # Library state: channels an interrupt writes to (whole log), the idle thread.
    isr_fed = set()
    for eid, ts, irq, vals, payload in read_log(args.log, [0]):
        if eid == (CSP_COMPONENT << 8 | PUT_FROM_ISR) and vals:
            isr_fed.add(vals[1])
    idle_names = set(args.idle.split(',')) if args.idle else None

    class Idle:
        def __contains__(self, tid):
            if tid is None:
                return False
            if idle_names is not None:
                return names.get(tid) in idle_names
            return 'idle' in (syms.short(tid) or '').lower()
    has_switch = any(f for f in args.scvd if 'RTX' in os.path.basename(f) or 'FreeRTOS' in os.path.basename(f))
    state = conformance.Analyser(lambda a: syms.short(a) or '0x%08x' % a, isr_fed, Idle(), has_switch)
    state.thread_name = names.get         # the name a thread has when a stable point opens

    tail = [0]
    records = read_log(args.log, tail)
    if args.no_eventlist:
        events = scvd_eval.events(scvd, read_log(args.log, [0]))
    else:
        events = run_eventlist(eventlist, args.elf, scvd, args.log)

    out = None
    if args.output:
        out = csv.writer(open(args.output, 'w', newline=''))
        out.writerow(['model_time_s' if args.model_time else 'time_s', 'component', 'event', 'thread', 'value'])
    perf = None
    if args.perfetto:
        perf = Perfetto(args.perfetto, names, syms, (t0, t1),
                        set(args.threads.split(',')) if args.threads else None, has_switch)
    counts = collections.Counter()
    decreases, last_ts, n, first_time, last_time = 0, None, 0, None, None
    pending_start = None      # thread of the last ProcessStart (its ProcessName follows)
    ring_notes = []           # ring2log.py's "ring: ..." notes (time, text)
    switch_seen = False
    for ev in events:
        rec = next(records, None)
        if rec is None:
            sys.exit('eventlist decoded more events than the log has records')
        eid, ts, irq, vals, payload = rec
        n += 1
        if last_ts is not None and ts < last_ts:
            decreases += 1
        last_ts = ts
        comp_no, msg = eid >> 8, eid & 0xFF
        comp = comps.get(comp_no, '0x%02X' % comp_no)
        prop = ev.get('eventProperty', '')
        value = ev.get('value', '')
        thread = ''
        if comp_no == CSP_COMPONENT and vals is not None:
            if msg == ALT_GUARD and len(vals) == 4:
                thread = context(vals[0])
                idx, kind = conformance.unpack_guard(vals[3])
                target = ('ticks=%d' % vals[2] if kind == 'timeout' else
                          'channel=0x%08x' % vals[2] if kind in ('input', 'output') else 'target=0x%x' % vals[2])
                value = 'context=%s, alt=0x%08x, guard=%d, kind=%s, %s' % (thread, vals[1], idx, kind, target)
            elif msg == RENDEZVOUS_DONE and len(vals) == 4:
                thread = context(vals[1] if vals[3] else vals[0])
                value = re.sub(r'\bwriter=[^,]*', 'writer=0x%08x (%s)' % (vals[0], names.get(vals[0])), value)
                value = re.sub(r'\breader=[^,]*', 'reader=0x%08x (%s)' % (vals[1], names.get(vals[1])), value)
            else:
                thread = context(vals[0])
                value = re.sub(r'\bcontext=[^,]*', 'context=' + thread, value)
                if msg == WAIT_CHANNEL:           # eventlist 1.2.0 prints this %t as empty
                    value = re.sub(r'\bdir=[^,]*', 'dir=' + ('write' if vals[1] & 1 else 'read'), value)
                elif msg in BUFFERED_EVENTS and len(vals) == 4:   # the same for the policy
                    value = re.sub(r'\bpolicy=[^,]*', 'policy=' + conformance.unpack_policy(vals[2]), value)
                elif msg in (RESUME, RESUME_TIMEOUT) and len(vals) == 4:
                    value = re.sub(r', tick=.*$', '', value) + ', tick=%d, deadline=%d' % (vals[2], vals[3])
        else:
            m = re.search(r'\b(?:%s)=(0x[0-9A-Fa-f]+)' % '|'.join(THREAD_KEYS), value)
            if m:
                thread = '0x%08x' % int(m.group(1), 16)
        # eventlist prints %S as a hex number (without 0x) when it finds no symbol name.
        def resolve(m):
            name = syms.name(int(m.group(2), 16))
            return '%s=%s' % (m.group(1), name) if name else m.group(0)
        value = re.sub(r'\b(%s)=(0x[0-9A-Fa-f]+|[0-9A-Fa-f]{8})\b' % '|'.join(ADDR_KEYS), resolve, value)
        if payload is not None:
            text = payload.split(b'\0')[0].decode('ascii', 'replace')
            if comp_no == CSP_COMPONENT and msg == PROCESS_NAME:
                value = 'name=' + text            # eventlist 1.2.0 prints %t of a data record as empty
                if pending_start is not None:
                    names.process[pending_start] = text
            elif comp_no in comps and comp_no != 0xFF and re.search(r'\w=$', value):
                value += text                     # the same for other data records (e.g. the ring's TaskName)
            elif comp_no not in comps or comp_no == 0xFF or not value.strip() or value.startswith('data='):
                value = 'text=' + text            # a data record no .scvd describes
                if comp_no == 0xFF and text.startswith('ring: '):
                    ring_notes.append((float(ev['time']), text))   # ring2log.py: lost, torn or truncated records
        if comp_no == CSP_COMPONENT and msg == PROCESS_START and vals:
            pending_start = vals[0]
        t = float(ev['time'])
        first_time = t if first_time is None else first_time
        last_time = t
        if out:
            out.writerow(['%.8f' % t, comp, prop, thread, value])
        counts[(comp, prop)] += 1
        if comp_no == CSP_COMPONENT:
            names.csp(msg, vals)
            state.csp(float(ev['time']) * 1e6, msg, vals)
        elif prop in ('ThreadSwitched', 'TaskSwitchedIn'):
            tid = parse_hex(value, 'thread_id' if prop == 'ThreadSwitched' else 'pxCurrentTCB')
            if tid is not None:
                state.switch_in(float(ev['time']) * 1e6, tid)
        if perf:
            us = t * 1e6
            if comp_no == CSP_COMPONENT:
                perf.csp(msg, us, vals, irq, value)
            elif prop in ('ThreadSwitched', 'TaskSwitchedIn'):
                tid = parse_hex(value, 'thread_id' if prop == 'ThreadSwitched' else 'pxCurrentTCB')
                if tid is not None:
                    switch_seen = True
                    perf.switch_in(tid, us)
            elif prop == 'TaskSwitchedOut':
                tid = parse_hex(value, 'pxCurrentTCB')
                if tid is not None:
                    perf.switch_out(tid, us)

    left = sum(1 for _ in records)
    if left:
        sys.exit('the log has %d records that eventlist did not decode' % left)
    state.finish(last_time * 1e6 if last_time is not None else 0)
    results = state.results()
    results['stable_note'] = None if has_switch else 'no RTOS switch events: stability unconfirmed'
    if args.checks:
        json.dump(results, open(args.checks, 'w'), indent=1)
    if args.stable:
        write_stable(args.stable, state.stable, names)
    if perf:
        perf.has_switch = switch_seen
        end = last_time * 1e6 if last_time is not None else 0
        if t1 is not None:
            end = min(end, t1)
        perf.stable_points(state.stable)
        perf.close(end, {
            'source': 'CSP4CMSIS tools/trace/decode.py',
            'time': ('model time: time stamps / EVENT_TIMESTAMP_FREQ; on an FVP these count model '
                     'cycles, which are not real (or simulated) time') if args.model_time else
                    'time stamps / EVENT_TIMESTAMP_FREQ',
            'window_us': [t0, t1], 'threads': args.threads, 'test': args.test,
            'library_state': results,
        })
    if args.summary:
        csp = sum(k for (c, _), k in counts.items() if c == comps[CSP_COMPONENT])
        with open(args.summary, 'w') as f:
            f.write('events: %d (CSP4CMSIS %d, other %d); trailing bytes in log: %d\n'
                    % (n, csp, n - csp, tail[0]))
            f.write('time stamps: %s (%d decreases)\n' % ('monotonic' if decreases == 0 else 'NOT monotonic', decreases))
            f.write('%s: first %s, last %s\n' % ('model_time_s' if args.model_time else 'time_s', first_time, last_time))
            for (c, e), k in sorted(counts.items()):
                f.write('%8d  %s %s\n' % (k, c, e))
            r = results['ring']
            f.write('ring slots: %s; %d puts, %d gets (%d read their put\'s slot), %d overwrites (%d evicted the '
                    'oldest put\'s slot), %d dropped (%d without slot)\n'
                    % ('PASS' if not r.get('errors') else 'FAIL %d' % r['errors'], r.get('puts', 0), r.get('gets', 0),
                       r.get('gets_ok', 0), r.get('overwrites', 0), r.get('overwrites_ok', 0), r.get('dropped', 0),
                       r.get('dropped_ok', 0)))
            a = results['alt']
            f.write('ALT offers: %s; %d waits, %d enabled guards, %d unresolved, %d guards described\n'
                    % ('PASS' if not a.get('errors') and not a.get('unresolved_bits') else 'FAIL', a.get('waits', 0),
                       a.get('enabled_bits', 0), a.get('unresolved_bits', 0), a.get('guards_described', 0)))
            d = results['deadlines']
            f.write('ALT deadlines: %s; %d timeout waits (%d AltDeadline), %d ResumeTimeout (at most %d ticks after '
                    'the deadline), %d Resume (%d before the deadline, %d at it, %d after)\n'
                    % ('PASS' if not d.get('errors') else 'FAIL %d' % d['errors'], d.get('timeout_waits', 0),
                       d.get('deadlines', 0), d.get('resume_timeout', 0), d.get('timeout_ticks_late_max', 0),
                       d.get('resume', 0), d.get('resume_before', 0), d.get('resume_at', 0), d.get('resume_after', 0)))
            f.write('stable points: %d %s%s\n' % (results['stable_points'], json.dumps(results['stable_classes']),
                                                 '; ' + results['stable_note'] if results['stable_note'] else ''))
            truncated = sum(1 for _, text in ring_notes if text.startswith('ring: truncated'))
            f.write('ring notes: %d (%d truncated data events)\n' % (len(ring_notes), truncated))
            for t, text in ring_notes:
                f.write('  %.8f  %s\n' % (t, text))


if __name__ == '__main__':
    main()
