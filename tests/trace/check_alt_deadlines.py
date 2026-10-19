#!/usr/bin/env python3
"""Check the ALT deadlines of the suite's timeout tests T20-T24 and of
alt_offers.cpp in a CSV of tools/trace/decode.py.

    check_alt_deadlines.py events.csv[.xz]

A select() instance starts with the AltGuard events of its Alternative
(guard 0 again: a new Alternative, possibly at a reused stack address) and
ends with the next AltSelected of the thread that waited in it or, if it
never waited, with the next AltSelected after its AltGuard events (the
tests run one select() at a time). Exit status 0 if
- every wait of an instance follows an AltDeadline; the time left
  (deadline - now) is 1..the shortest timeout; all waits of an instance
  have the same deadline; every ResumeTimeout is at or after the deadline,
  every Resume before it or at it;
- T20: 5 instances each for timeouts 1, 3, 10, all selecting the timeout,
  ending with ResumeTimeout; T22: one, selecting guard 2 (timeout 5) by
  ResumeTimeout, time left at most 5; T23: its two Alternatives never wait,
  select 1 (no channel) and 0 (channel ready); T24: one, more than one wait
  (stale wakeups), the same deadline, ending with ResumeTimeout, selecting 1;
  T21: 15 instances, each selecting the channel (0) or the timeout (1), and
  every timeout selection after a ResumeTimeout; alt_timeout and alt_full:
  one wait each, ResumeTimeout, selecting 1.
"""
import collections, csv, lzma, re, sys

T20 = {d: ['input test_T20()::never', 'timeout %d' % d] for d in (1, 3, 10)}
T21 = ['input t21_start()::ch', 'timeout 10']
T22 = ['input test_T22()::never', 'timeout 30', 'timeout 5', 'timeout 15']
T23 = (['input test_T23()::never', 'timeout 0'], ['input t21_start()::ch', 'timeout 0'])
T24 = ['input test_T24()::never', 'timeout 10']
ALT_TIMEOUT = ['input alt_idle', 'timeout 3']
ALT_FULL = ['output alt_full_buf', 'timeout 2']


def short(name):
    return re.sub(r'\(anonymous namespace\)::', '', name)


class Inst:
    def __init__(self, alt):
        self.alt, self.guards, self.waits, self.selected = alt, {}, [], None
        self.pending = None               # (now, deadline) of an AltDeadline not yet followed by WaitAlt

    def glist(self):
        return [self.guards[i] for i in sorted(self.guards)]


path = sys.argv[1]
f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
current = {}                              # Alternative -> open Inst
by_thread = {}                            # thread -> Inst it waits in
unwaited = []                             # instances described but neither waited in nor selected
done, errors = [], []
for row in csv.DictReader(f):
    if row['component'] != 'CSP':
        continue
    v, ev, th = row['value'], row['event'], row['thread']
    alt = re.search(r'\b(?:alt|object)=([^,]+)', v)
    alt = alt.group(1) if alt else None
    if ev == 'AltGuard':
        i = int(re.search(r'guard=(\d+)', v).group(1))
        if i == 0 or alt not in current:
            current[alt] = Inst(alt)
            unwaited.append(current[alt])
        kind = re.search(r'kind=(\w+)', v).group(1)
        tgt = re.search(r'(?:channel|ticks|target)=([^,]+)', v).group(1)
        current[alt].guards[i] = '%s %s' % (kind, short(tgt))
    elif ev == 'AltDeadline' and alt in current:
        inst = current[alt]
        inst.pending = (int(re.search(r'now=(\d+)', v).group(1)), int(re.search(r'deadline=(\d+)', v).group(1)))
    elif ev == 'WaitAlt' and alt in current:
        inst = current[alt]
        by_thread[th] = inst
        if inst in unwaited:
            unwaited.remove(inst)
        inst.waits.append({'deadline': inst.pending, 'resume': None})
        inst.pending = None
    elif ev in ('Resume', 'ResumeTimeout') and th in by_thread and alt == by_thread[th].alt:
        tick = re.search(r'tick=(\d+)', v)
        by_thread[th].waits[-1]['resume'] = (ev, int(tick.group(1)) if tick else None)
    elif ev == 'AltSelected':
        sel = int(re.search(r'guard=(\d+)', v).group(1))
        inst = by_thread.pop(th, None)
        if inst is None and unwaited:
            inst = unwaited.pop()
        if inst is None:
            continue
        inst.selected = sel
        done.append(inst)
        if current.get(inst.alt) is inst:
            del current[inst.alt]


def tick_diff(a, b):
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d & 0x80000000 else d


stats = collections.Counter()


def generic(inst):
    """Deadline rules for every wait of an instance with a timeout guard."""
    delays = [int(g.split()[1]) for g in inst.glist() if g.startswith('timeout ')]
    if not delays:
        return True
    good, deadlines = True, set()
    for w in inst.waits:
        if w['deadline'] is None or w['resume'] is None or w['resume'][1] is None:
            good = False
            continue
        now, dl = w['deadline']
        deadlines.add(dl)
        left = tick_diff(dl, now)
        good &= 1 <= left <= min(delays)
        d = tick_diff(w['resume'][1], dl)
        if w['resume'][0] == 'ResumeTimeout':
            stats['ResumeTimeout'] += 1
            good &= d >= 0
        else:
            stats['Resume before' if d < 0 else 'Resume at' if d == 0 else 'Resume after'] += 1
            good &= d <= 0
    return good and len(deadlines) <= 1


def last_resume(inst):
    return inst.waits[-1]['resume'][0] if inst.waits and inst.waits[-1]['resume'] else None


ok = True
for inst in done:
    if not generic(inst):
        ok = False
        errors.append('%s %s: deadline rules violated: %s' % (inst.alt, inst.glist(), inst.waits))


def pick(glist):
    return [x for x in done if x.glist() == glist]


def report(name, good, text):
    global ok
    ok &= bool(good)
    print('%-12s %s  %s' % (name, 'PASS' if good else 'FAIL', text))


for d, g in T20.items():
    xs = pick(g)
    report('T20 d=%d' % d, len(xs) == 5 and all(x.selected == 1 and last_resume(x) == 'ResumeTimeout' for x in xs),
           '%d selects, selected %s, waits %s, left %s' % (
               len(xs), [x.selected for x in xs], [len(x.waits) for x in xs],
               sorted(set(tick_diff(w['deadline'][1], w['deadline'][0]) for x in xs for w in x.waits if w['deadline']))))
xs = pick(T21)
report('T21', len(xs) == 15 and all(x.selected in (0, 1) and (x.selected == 0 or last_resume(x) == 'ResumeTimeout')
                                     for x in xs),
       '%d selects: channel %d, timeout %d; last resume %s' % (
           len(xs), sum(x.selected == 0 for x in xs), sum(x.selected == 1 for x in xs),
           dict(collections.Counter(last_resume(x) for x in xs))))
xs = pick(T22)
report('T22', len(xs) == 1 and xs[0].selected == 2 and last_resume(xs[0]) == 'ResumeTimeout'
       and all(tick_diff(w['deadline'][1], w['deadline'][0]) <= 5 for w in xs[0].waits if w['deadline']),
       '; '.join('selected %s, waits %d, left %s' % (x.selected, len(x.waits),
                 [tick_diff(w['deadline'][1], w['deadline'][0]) for w in x.waits if w['deadline']]) for x in xs) or 'none')
for g, want in zip(T23, (1, 0)):
    xs = pick(g)
    report('T23', len(xs) == 1 and xs[0].selected == want and not xs[0].waits,
           '%s: %s' % (', '.join(g), '; '.join('selected %s, waits %d' % (x.selected, len(x.waits)) for x in xs) or 'none'))
xs = pick(T24)
report('T24', len(xs) == 1 and xs[0].selected == 1 and len(xs[0].waits) > 1 and last_resume(xs[0]) == 'ResumeTimeout',
       '; '.join('selected %s, waits %d (%s), deadlines %d' % (
           x.selected, len(x.waits), dict(collections.Counter(w['resume'][0] for w in x.waits if w['resume'])),
           len(set(w['deadline'][1] for w in x.waits if w['deadline']))) for x in xs) or 'none')
for name, g in (('alt_timeout', ALT_TIMEOUT), ('alt_full', ALT_FULL)):
    xs = pick(g)
    report(name, len(xs) == 1 and xs[0].selected == 1 and len(xs[0].waits) == 1 and last_resume(xs[0]) == 'ResumeTimeout',
           '; '.join('selected %s, waits %d, %s' % (x.selected, len(x.waits), last_resume(x)) for x in xs) or 'none')
report('all', ok and not errors, '%d select() instances; resumes of deadline waits: %s' % (len(done), dict(stats)))
for e in errors[:10]:
    print('  ' + e)
print('ALT deadlines: ' + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
