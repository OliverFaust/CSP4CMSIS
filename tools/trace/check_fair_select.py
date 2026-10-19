#!/usr/bin/env python3
"""Fairness of select() across the selects of an Alternative, from a decoded
trace (no ProB).

    check_fair_select.py events.csv[.xz] [--fair REGEX] [--evidence REGEX]

The rule, as Alternative::fairSelect() and AltScheduler::select() define it
(alternative.cpp): select() enables the guards in the order idx = (i +
offset) % n, i = 0..n-1, and stops at the first ready one; after a wakeup it
re-verifies them in that order. A claimed guard (an ALT partner committed to
it) wins; otherwise the first ready guard in that order. fairSelect() passes
offset = fair_select_start_index, which starts at 0 and after each select()
becomes (selected + 1) % n; priSelect() passes 0.

Checked consequence: a guard that is ready during the whole select() and
precedes the selected guard in that order would have been selected. Ready
during the whole select() is shown from the records alone:
  - rendezvous input guard: a plain writer's offer is pending (its pair
    WaitChannel(write) is recorded before the select's window opens, the
    RendezvousDone that takes it after the window closes);
  - buffered input guard: elements in the channel when the window opens
    (the count of its last buffered event before), and no get from it
    inside the window.
The window of select k opens at its thread's last record before the
select's first record (select() starts after it: a thread records in
program order) and closes at its AltSelected. Selects in which an ALT partner could claim are skipped (no
such partner on the checked channels). Alternatives whose name or guards
match --fair are checked against fairSelect()'s offset, the others against
priSelect()'s. --evidence: Alternatives that must give evidence: all but
three of their selects with another guard ready throughout, and at least
one violation of the other rule (the check can fail). Exit status 1 on any
violation or missing evidence.
"""
import argparse, bisect, collections, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csp_trace as T  # noqa: E402


def pending_offers(tr, chan):
    """[(from row, to row)]: a plain writer's offer pending on a rendezvous
    channel, from its pair WaitChannel(write) to the RendezvousDone taking it.
    A writer's operation records WaitChannel(write) and Resume (serial
    semaphore), then either its own RendezvousDone (second arriver), or --
    first arriver, offer -- WaitChannel(write) and Resume around its wait for
    the taker, whose RendezvousDone can come before that WaitChannel (no
    provable pending interval then) or between it and the Resume."""
    rv = tr.rvs[chan]
    state, pair, out = {}, {}, []
    for k, row, x in rv.items:
        if k == 'done':
            w = T.tid(row.d['writer'])
            st = state.get(w)
            if row.d.get('by_reader') == '0' and st == 'decide':
                state[w] = 'idle'             # the writer took a pending reader
            elif st == 'decide':
                state[w] = 'taken'            # taken before its pair WaitChannel was recorded
            elif st == 'pair':
                out.append((pair.pop(w), row.i))
                state[w] = 'taken_pair'
            continue
        if x != 'write':
            continue
        th, st = row.thread, state.get(row.thread, 'idle')
        if k == 'wait':
            if st == 'idle':
                state[th] = 'serial'
            elif st == 'decide':
                state[th], pair[th] = 'pair', row.i
            elif st == 'taken':
                state[th] = 'taken_pair'
        elif k == 'res':
            if st == 'serial':
                state[th] = 'decide'
            elif st == 'taken_pair':
                state[th] = 'idle'
    return out


def buffered_levels(tr, chan):
    """(row indices, element counts after) of a buffered channel's events, and the get rows."""
    c = tr.bufs[chan]
    ix, cnt, gets = [], [], []
    for k, row, x in c.items:
        if k in ('write', 'get'):
            ix.append(row.i)
            cnt.append(int(row.d['count']))
            if k == 'get':
                gets.append(row.i)
    return ix, cnt, gets


def ready_throughout(tr, a, lo, hi, cache):
    """Guard indices provably ready during the whole window (lo, hi]."""
    out = set()
    for g, (kind, chan) in enumerate(a.glist()):
        if kind != 'input':
            continue
        if chan in tr.rvs:
            if ('rv', chan) not in cache:
                cache[('rv', chan)] = pending_offers(tr, chan)
            if any(f <= lo and t > hi for f, t in cache[('rv', chan)]):
                out.add(g)
        elif chan in tr.bufs:
            if ('buf', chan) not in cache:
                cache[('buf', chan)] = buffered_levels(tr, chan)
            ix, cnt, gets = cache[('buf', chan)]
            p = bisect.bisect_right(ix, lo) - 1
            if p >= 0 and cnt[p] > 0:
                q = bisect.bisect_right(gets, lo)
                if q >= len(gets) or gets[q] > hi:
                    out.add(g)
    return out


def claimable(tr, a):
    """An ALT partner (another Alternative) on one of the guards' channels."""
    chans = {t for k, t in a.guards.values() if k in ('input', 'output')}
    return any(b is not a and any(t in chans for k, t in b.guards.values()) for b in tr.alts)


def thread_rows(tr):
    by = collections.defaultdict(list)
    for r in tr.rows:
        by[r.thread].append(r.i)
    return by


def window_start(tr, a, s, by):
    """The ALT's thread's last record before the select's first record."""
    first = min(r.i for r in s.rows) if s.rows else s.end.i
    own = by[a.thread]
    p = bisect.bisect_left(own, first) - 1
    return max(own[p] if p >= 0 else -1, s.lo)


def check(tr, a, fair, by):
    n = len(a.guards)
    cache, res = {}, collections.Counter()
    viol = []
    prev = None
    for k, s in enumerate(a.selects):
        offset = 0 if not fair or prev is None else (prev + 1) % n
        prev = s.selected
        order = [(i + offset) % n for i in range(n)]
        ready = ready_throughout(tr, a, window_start(tr, a, s, by), s.end.i, cache) - {s.selected}
        if ready:
            res['selects with another guard ready throughout'] += 1
        before = [j for j in ready if order.index(j) < order.index(s.selected)]
        if before:
            viol.append((k, s.end.i, s.selected, sorted(before), offset))
        res['selects'] += 1
    return res, viol


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('events')
    ap.add_argument('--fair', default=r'$^', help='regex: Alternatives (name or guards) that use fairSelect()')
    ap.add_argument('--evidence', default=r'$^', help='regex: Alternatives that must give evidence')
    a = ap.parse_args()
    tr = T.Trace(T.read_rows(a.events))
    by = thread_rows(tr)
    bad = 0
    for alt in tr.alts:
        if len(alt.guards) < 2 or len(alt.selects) < 2:
            continue
        text = T.short(alt.alt) + ' ' + T.guard_text(alt.glist(), tr)
        fair = re.search(a.fair, text) is not None
        if claimable(tr, alt):
            print('%-60s skipped: an ALT partner may claim' % text[:60])
            continue
        res, viol = check(tr, alt, fair, by)
        res2, viol2 = check(tr, alt, not fair, by)
        print('%-60s %s: %d selects, %d with another guard ready throughout, %d violations '
              '(the other rule: %d)' % (text[:60], 'fairSelect' if fair else 'priSelect', res['selects'],
                                        res['selects with another guard ready throughout'], len(viol), len(viol2)))
        for v in viol[:5]:
            print('    select %d (row %d) chose guard %d, guard(s) %s ready throughout and earlier from offset %d' % v)
        bad += len(viol)
        if re.search(a.evidence, text):
            ev = res['selects with another guard ready throughout']
            if ev < res['selects'] - 3 or not viol2:
                print('    missing evidence: %d of %d selects with another guard ready throughout, %d violations of the other rule'
                      % (ev, res['selects'], len(viol2)))
                bad += 1
    print('fair select: ' + ('PASS' if not bad else 'FAIL'))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
