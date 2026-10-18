#!/usr/bin/env python3
"""Check the ALT offers of alt_offers.cpp and of the suite's test T12 in a CSV
of tools/trace/decode.py.

    check_alt_offers.py events.csv[.xz]

Exit status 0 if
- alt_timeout, alt_output, alt_full (alt_offers.cpp) each have exactly the
  AltGuard events below, exactly one WaitAlt (enabled=0x3, guards=2) and
  then AltSelected with the guard given;
- every WaitAlt of the Alternative whose guards are test_T12()::a and
  test_T12()::b (inputs) has enabled=0x3, guards=2 (at least one).
An Alternative is known by its address; a new Alternative at the same address
(stack) describes its guards again from guard 0.
"""
import collections, csv, lzma, re, sys

EXPECTED = {
    'alt_timeout': (['input alt_idle', 'timeout 3'], 1),
    'alt_output': (['output alt_out', 'input alt_idle'], 0),
    'alt_full': (['output alt_full_buf', 'timeout 2'], 1),
}
T12 = ['input test_T12()::a', 'input test_T12()::b']


path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
guards = {}                            # alt -> {index: text}
waits = collections.defaultdict(list)  # key -> [(enabled, n, selected)]
pending = {}                           # thread -> index into waits[key]
for row in csv.DictReader(f):
    if row['component'] != 'CSP':
        continue
    v, ev = row['value'], row['event']
    if ev == 'AltGuard':
        alt = re.search(r'alt=([^,]+)', v).group(1)
        i = int(re.search(r'guard=(\d+)', v).group(1))
        kind = re.search(r'kind=(\w+)', v).group(1)
        tgt = re.search(r'(?:channel|ticks|target)=([^,]+)', v).group(1)
        if i == 0:
            guards[alt] = {}
        tgt = re.sub(r'^\(anonymous namespace\)::', '', tgt)
        guards[alt][i] = '%s %s' % (kind, tgt)
    elif ev == 'WaitAlt':
        alt = re.search(r'alt=([^,]+)', v).group(1)
        g = [guards.get(alt, {}).get(i) for i in range(len(guards.get(alt, {})))]
        name = next((k for k in EXPECTED if alt.endswith('::' + k)), None)
        key = name or ('T12' if g == T12 else None)
        if key:
            enabled = re.search(r'enabled=(0x[0-9a-fA-F]+|\d+)', v).group(1)
            n = int(re.search(r'guards=(\d+)', v).group(1))
            waits[key].append([int(enabled, 0), n, None, g])
            pending[row['thread']] = waits[key][-1]
    elif ev == 'AltSelected' and row['thread'] in pending:
        pending.pop(row['thread'])[2] = int(re.search(r'guard=(\d+)', v).group(1))

ok = True
for name, (want_guards, want_sel) in EXPECTED.items():
    w = waits.get(name, [])
    good = len(w) == 1 and w[0][0] == 0x3 and w[0][1] == 2 and w[0][2] == want_sel and w[0][3] == want_guards
    ok &= good
    print('%-12s %s  %s' % (name, 'PASS' if good else 'FAIL',
                            '; '.join('guards [%s], enabled 0x%x of %d, selected %s' % (', '.join(map(str, x[3])), x[0], x[1], x[2])
                                      for x in w) or 'no WaitAlt'))
w = waits.get('T12', [])
good = bool(w) and all(x[0] == 0x3 and x[1] == 2 for x in w)
ok &= good
print('%-12s %s  %d waits, enabled masks %s' % ('T12', 'PASS' if good else 'FAIL', len(w),
                                               dict(collections.Counter('0x%x of %d' % (x[0], x[1]) for x in w))))
print('ALT offers: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
