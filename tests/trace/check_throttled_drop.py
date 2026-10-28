#!/usr/bin/env python3
"""Check the events of throttled_drop.cpp in a CSV of tools/trace/decode.py.

    check_throttled_drop.py events.csv[.xz]

The exact counts depend on when the interrupts fall, so the check is of
invariants on the channel throttled_samples (KeepOldest) and the sampler
that forwards it on throttled_forward:
- at least one DroppedNew before the sampler's last AltSelected (a drop
  during operation, not only in the tail);
- every DroppedNew at count == capacity;
- puts (PutFromISR + DroppedNew) = gets + dropped + still buffered at the
  end, and the replayed occupancy equals the count of every record;
- the occupancy never exceeds the capacity;
- a DroppedNew while the sampler waits to write throttled_forward
  (WaitChannel dir=write until its Resume): the backpressure chain.
Prints the counts and exits 0 if all hold.
"""
import csv, lzma, re, sys

CH, FWD = 'throttled_samples', 'throttled_forward'

path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
rows = [r for r in csv.DictReader(f) if r['component'] == 'CSP']


def val(r, k):
    m = re.search(r'(?:^|, )%s=([^,]*)' % k, r['value'])
    return m.group(1).strip() if m else None


def chan(r, name):
    return re.fullmatch(r'(?:.*::)?%s' % name, val(r, 'channel') or '') is not None


def us(r):
    # decode.py -o with --model-time (simulators) or without (hardware), as csp_trace.py reads it
    return float(r['model_time_s'] if 'model_time_s' in r else r['time_s']) * 1e6


ev = [r for r in rows if r['event'] in ('PutFromISR', 'BufferedPut', 'DroppedNew', 'BufferedGet') and chan(r, CH)]
puts = [r for r in ev if r['event'] != 'BufferedGet']
drops = [r for r in ev if r['event'] == 'DroppedNew']
gets = [r for r in ev if r['event'] == 'BufferedGet']
sampler = gets[0]['thread'] if gets else None
# the sampler's lifetime: the processes of earlier tests ran on the same pool
# (trace_pool.h), so their threads can have the same context
life = (float('-inf'), float('inf'))
for i, r in enumerate(rows):
    if r['event'] == 'ProcessName' and val(r, 'name') == 'Throttled sampler':
        st = next((x for x in reversed(rows[:i]) if x['event'] == 'ProcessStart'), None)
        if st is not None:
            sampler = st['thread']
            en = next((x for x in rows[i:] if x['event'] == 'ProcessEnd' and x['thread'] == sampler), None)
            life = (us(st), us(en) if en is not None else float('inf'))
mine = [r for r in rows if r['thread'] == sampler and life[0] <= us(r) <= life[1]]
sel = [r for r in mine if r['event'] == 'AltSelected']
last_sel = us(sel[-1]) if sel else float('-inf')
cap = int(val(ev[0], 'capacity')) if ev else 0

fails = []
# occupancy replay
occ, top, bad_occ = 0, 0, []
for r in ev:
    if r['event'] in ('PutFromISR', 'BufferedPut'):
        occ += 1
    elif r['event'] == 'BufferedGet':
        occ -= 1
    top = max(top, occ)
    if val(r, 'count') is not None and int(val(r, 'count')) != occ:
        bad_occ.append('%s at %.1f us: count=%s, replayed %d' % (r['event'], us(r), val(r, 'count'), occ))
end = occ
during = [r for r in drops if us(r) < last_sel]
not_full = [r for r in drops if val(r, 'count') != val(r, 'capacity')]

# the sampler's waits to write throttled_forward
waits, start = [], None
for r in mine:
    if r['event'] == 'WaitChannel' and val(r, 'dir') == 'write' and chan(r, FWD):
        start = us(r)
    elif r['event'] == 'Resume' and start is not None:
        waits.append((start, us(r)))
        start = None
in_wait = [r for r in drops if any(a <= us(r) <= b for a, b in waits)]

if not ev:
    fails.append('no events on %s' % CH)
if not during:
    fails.append('no DroppedNew before the sampler\'s last select (%.1f us)' % last_sel)
if not_full:
    fails.append('%d DroppedNew below capacity, first at %.1f us' % (len(not_full), us(not_full[0])))
if len(puts) != len(gets) + len(drops) + end:
    fails.append('puts %d != gets %d + dropped %d + still buffered %d' % (len(puts), len(gets), len(drops), end))
if bad_occ:
    fails.append('%d records whose count differs from the replay, first: %s' % (len(bad_occ), bad_occ[0]))
if top > cap:
    fails.append('occupancy %d above capacity %d' % (top, cap))
if not in_wait:
    fails.append('no DroppedNew while the sampler waits to write %s' % FWD)

print('%s: puts %d = gets %d + dropped %d + still buffered %d; capacity %d, maximum occupancy %d'
      % (CH, len(puts), len(gets), len(drops), end, cap, top))
print('dropped during operation %d (before the sampler\'s last select at %.1f us), in the tail %d; '
      '%d while the sampler waits to write %s (%d waits)'
      % (len(during), last_sel, len(drops) - len(during), len(in_wait), FWD, len(waits)))
print('sampler selects %d' % len(sel))
for x in fails:
    print('  ' + x)
print('throttled drop: ' + ('FAIL' if fails else 'PASS'))
sys.exit(1 if fails else 0)
