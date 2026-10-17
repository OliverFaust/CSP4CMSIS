#!/usr/bin/env python3
"""Check the events of drop_policy.cpp in a CSV of tools/trace/decode.py.

    check_drop_policy.py events.csv[.xz]

Exit status 0 if each channel has exactly the expected events:
keep_oldest 2 BufferedPut + 3 DroppedNew, keep_newest 2 BufferedPut +
3 OverwroteOldest, and no other CSP4CMSIS event names them.
"""
import collections, csv, lzma, re, sys

EXPECTED = {('keep_oldest', 'BufferedPut'): 2, ('keep_oldest', 'DroppedNew'): 3,
            ('keep_newest', 'BufferedPut'): 2, ('keep_newest', 'OverwroteOldest'): 3}

path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
got = collections.Counter()
for row in csv.DictReader(f):
    if row['component'] != 'CSP':
        continue
    m = re.search(r'channel=csp4cmsis_trace_drop_policy(?:\(\))?::(keep_oldest|keep_newest)\b', row['value'])
    if m:
        got[(m.group(1), row['event'])] += 1
ok = got == collections.Counter(EXPECTED)
for key in sorted(set(got) | set(EXPECTED)):
    print('%-12s %-16s %d (expected %d)' % (key[0], key[1], got[key], EXPECTED.get(key, 0)))
print('drop policy: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
