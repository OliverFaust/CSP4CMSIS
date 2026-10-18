#!/usr/bin/env python3
"""Check the stable points of stable_env.cpp.

    check_stable_env.py events.csv[.xz] stable.csv

events.csv: decode.py -o; stable.csv: decode.py --stable (CSV). Exit status 0
if env_samples got exactly 3 PutFromISR; at least one stable point has
exactly the offers "Env sampler: env_samples ?" and "Env sink: env_link ?",
and every such point is classified "awaiting environment"; and no stable
point of the whole log is classified "deadlock". A log without RTOS switch
events passes with every such point "stability unconfirmed".
"""
import csv, lzma, re, sys

events, stable = sys.argv[1], sys.argv[2]
f = lzma.open(events, 'rt') if events.endswith('.xz') else open(events, newline='')
isr = sum(1 for r in csv.DictReader(f)
          if r['component'] == 'CSP' and r['event'] == 'PutFromISR' and re.search(r'channel=[^,]*\benv_samples\b', r['value']))
points = list(csv.DictReader(open(stable, newline='')))
WANT = ['Env sampler: env_samples ?', 'Env sink: env_link ?']


def offers(p):
    return sorted(re.sub(r'(^|; )[^:]*::', r'\1', o) for o in p['offers'].split('; '))


env = [p for p in points if offers(p) == WANT]
unconfirmed = bool(points) and all(p['classification'] == 'stability unconfirmed' for p in points)
want_class = 'stability unconfirmed' if unconfirmed else 'awaiting environment'
deadlocks = [p for p in points if p['classification'] == 'deadlock']
ok = isr == 3 and env and all(p['classification'] == want_class for p in env) and not deadlocks
print('PutFromISR on env_samples: %d (expected 3)' % isr)
print('stable points with the offers %s: %d, classified %s' % (
    ' and '.join(WANT), len(env), sorted(set(p['classification'] for p in env))))
print('stable points in the log: %d, deadlock: %d' % (len(points), len(deadlocks)))
print('stable env: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
