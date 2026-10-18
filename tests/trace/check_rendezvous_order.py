#!/usr/bin/env python3
"""Check the events of rendezvous_order.cpp in a CSV of tools/trace/decode.py.

    check_rendezvous_order.py events.csv[.xz]

Exit status 0 if each channel has exactly 10 RendezvousDone, all completed by
the second partner to arrive (writer_first: by_reader=1, reader_first:
by_reader=0), each naming the pair's writer and reader.
"""
import collections, csv, lzma, re, sys

ROUNDS = 10
EXPECTED = {('writer_first', '1', 'WF writer', 'WF reader'): ROUNDS,
            ('reader_first', '0', 'RF writer', 'RF reader'): ROUNDS}

path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
got = collections.Counter()
for row in csv.DictReader(f):
    if row['component'] != 'CSP' or row['event'] != 'RendezvousDone':
        continue
    m = re.search(r'channel=[^,]*::(writer_first|reader_first)\b', row['value'])
    if not m:
        continue
    w = re.search(r'writer=0x[0-9a-f]+ \(([^)]*)\)', row['value'])
    r = re.search(r'reader=0x[0-9a-f]+ \(([^)]*)\)', row['value'])
    b = re.search(r'by_reader=([01])', row['value'])
    got[(m.group(1), b.group(1) if b else '?', w.group(1) if w else '?', r.group(1) if r else '?')] += 1
ok = got == collections.Counter(EXPECTED)
for key in sorted(set(got) | set(EXPECTED)):
    print('%-12s by_reader=%s %s -> %s: %d (expected %d)' % (key + (got[key], EXPECTED.get(key, 0))))
print('rendezvous order: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
