#!/usr/bin/env python3
"""Check the stable points of stable_multi.cpp.

    check_stable_multi.py events.csv[.xz] stable.csv

events.csv: decode.py -o; stable.csv: decode.py --stable (CSV). Exit status 0
if multi_samples got exactly 2 PutFromISR; at least one stable point has the
offers of all three processes ("Multi timed: multi_never ?" and "timeout 3
ticks", "Multi sampler: multi_samples ?", "Multi sink: multi_link ?"); every
stable point with an offer of these processes is classified "awaiting
environment" (later points depend on when the interrupt comes: the sampler's
alone, or the timed process's and the sink's); and no stable point of the
log is "deadlock". A log without RTOS switch events passes with every such
point "stability unconfirmed".
"""
import csv, lzma, re, sys

events, stable = sys.argv[1], sys.argv[2]
f = lzma.open(events, 'rt') if events.endswith('.xz') else open(events, newline='')
isr = sum(1 for r in csv.DictReader(f)
          if r['component'] == 'CSP' and r['event'] == 'PutFromISR' and re.search(r'channel=[^,]*\bmulti_samples\b', r['value']))
points = list(csv.DictReader(open(stable, newline='')))
ALL = ['Multi sampler: multi_samples ?', 'Multi sink: multi_link ?', 'Multi timed: multi_never ?',
       'Multi timed: timeout 3 ticks']


def offers(p):
    return sorted(re.sub(r'(^|; )[^:]*::', r'\1', o) for o in p['offers'].split('; '))


together = [p for p in points if offers(p) == ALL]
mine = [p for p in points if any(o.startswith('Multi ') for o in offers(p))]
unconfirmed = bool(points) and all(p['classification'] == 'stability unconfirmed' for p in points)
want = 'stability unconfirmed' if unconfirmed else 'awaiting environment'
deadlocks = [p for p in points if p['classification'] == 'deadlock']
ok = isr == 2 and together and all(p['classification'] == want for p in mine) and not deadlocks
print('PutFromISR on multi_samples: %d (expected 2)' % isr)
print('stable points with all three processes\' offers: %d; with offers of these processes: %d, classified %s' % (
    len(together), len(mine), sorted(set(p['classification'] for p in mine))))
print('stable points in the log: %d, deadlock: %d' % (len(points), len(deadlocks)))
print('stable multi: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
