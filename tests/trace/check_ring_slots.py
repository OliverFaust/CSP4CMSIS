#!/usr/bin/env python3
"""Check the ring slots of ring_wrap.cpp and drop_policy.cpp in a CSV of
tools/trace/decode.py.

    check_ring_slots.py events.csv[.xz]

Exit status 0 if the events of each channel are exactly:
  ring_wrap (Block, 3): one BufferedPut/BufferedGet per letter of
      PPGPPGGPPGGGPG, slots as a ring of 3 from slot 0 gives them;
  keep_oldest (KeepOldest, 2): BufferedPut slots 0, 1; three DroppedNew;
  keep_newest (KeepNewest, 2): BufferedPut slots 0, 1; three OverwroteOldest
      evicting and writing slots 0, 1, 0.
"""
import csv, lzma, re, sys

RING_OPS, RING_SIZE = 'PPGPPGGPPGGGPG', 3


def ring_expected():
    head = count = 0
    out = []
    for op in RING_OPS:
        if op == 'P':
            out.append(('BufferedPut', 'slot=%d' % ((head + count) % RING_SIZE)))
            count += 1
        else:
            out.append(('BufferedGet', 'slot=%d' % head))
            head, count = (head + 1) % RING_SIZE, count - 1
    return out


EXPECTED = {
    'ring_wrap': ring_expected(),
    'keep_oldest': [('BufferedPut', 'slot=0'), ('BufferedPut', 'slot=1')] + [('DroppedNew', '')] * 3,
    'keep_newest': [('BufferedPut', 'slot=0'), ('BufferedPut', 'slot=1'),
                    ('OverwroteOldest', 'evicted=0, written=0'), ('OverwroteOldest', 'evicted=1, written=1'),
                    ('OverwroteOldest', 'evicted=0, written=0')],
}

BUFFERED = ('BufferedPut', 'BufferedGet', 'PutFromISR', 'DroppedNew', 'OverwroteOldest')

path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
got = {k: [] for k in EXPECTED}
for row in csv.DictReader(f):
    if row['component'] != 'CSP' or row['event'] not in BUFFERED:
        continue
    m = re.search(r'channel=(?:csp4cmsis_trace_ring_wrap|csp4cmsis_trace_drop_policy)(?:\(\))?::(\w+)\b', row['value'])
    if not m or m.group(1) not in got:
        continue
    slot = re.search(r'(slot=\d+|evicted=\d+, written=\d+)', row['value'])
    got[m.group(1)].append((row['event'], slot.group(1) if slot else ''))
ok = True
for ch, want in EXPECTED.items():
    good = got[ch] == want
    ok &= good
    print('%-12s %s  %s' % (ch, 'PASS' if good else 'FAIL', ' '.join('%s%s' % (e[len('Buffered'):] if e.startswith('Buffered') else e,
                                                                          '(' + s + ')' if s else '') for e, s in got[ch])))
    if not good:
        print('%-12s expected %s' % ('', want))
print('ring slots: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
