#!/usr/bin/env python3
"""Check a decode.py --perfetto trace with Perfetto's trace_processor.

    check_perfetto.py trace.json [...]

Needs the Python package perfetto (pip install perfetto; it fetches the
trace_processor binary on first use). Loads each file, queries it with SQL
and compares with the counts decode.py wrote into its otherData:

- flows: one per RendezvousDone, per channel;
- running slices: one per RTOS switch-in, per thread (none if the log has
  no switch events);
- waits: every wait slice ends at its Resume ("(no resume)" only for a
  thread still blocked when the log ends), and their number;
- counters: buffered-channel occupancy never negative, never above the
  channel's capacity;
- buffered channels (decode.py replays each buffer as a FIFO): one flow per
  get, per channel; no get without a source put; puts + elements buffered at
  the window start = gets + overwritten + dropped + still buffered at the end;
  the replayed depth equals the library's element count after every put and
  get.

Exit status 0 if every check passes.
"""
import collections
import json
import sys

from perfetto.trace_processor import TraceProcessor


def rows(tp, sql):
    return list(tp.query(sql))


def check(path):
    meta = json.load(open(path))['otherData']
    tp = TraceProcessor(trace=path)
    ok = True

    def report(name, good, detail):
        nonlocal ok
        ok &= good
        print('  %-8s %s  %s' % (name, 'PASS' if good else 'FAIL', detail))

    # flows per channel
    got = {r.name[len('rendezvous '):]: r.n for r in rows(tp, '''
        SELECT s.name AS name, COUNT(*) AS n FROM flow f JOIN slice s ON f.slice_out = s.id
        WHERE s.category = 'rendezvous' GROUP BY s.name''')}
    want = meta['expected_flows_per_channel']
    report('flows', got == want, '%d flows on %d channels' % (sum(got.values()), len(got)))

    # running slices per thread (process name = thread name)
    got = {r.name: r.n for r in rows(tp, '''
        SELECT p.name AS name, COUNT(*) AS n FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id JOIN thread t USING (utid) JOIN process p USING (upid)
        WHERE s.name = 'running' GROUP BY p.name''')}
    want = meta['expected_switch_ins_per_thread'] if meta['running_slices'] else {}
    report('running', got == want, '%d slices on %d threads%s' % (
        sum(got.values()), len(got), '' if meta['running_slices'] else ' (' + meta.get('running_slices_note', '') + ')'))

    # waits
    waits = rows(tp, "SELECT name, ts, dur FROM slice WHERE name LIKE 'waiting on %'")
    open_ = [w for w in waits if w.name.endswith('(no resume)')]
    end_ns = meta['trace_end_us'] * 1000
    early = [w for w in open_ if abs(w.ts + w.dur - end_ns) > 1000]   # not at the end of the log
    n_resumed = len(waits) - len(open_)
    want = meta['stats'].get('wait_slices', 0)
    report('waits', not early and n_resumed == want,
           '%d wait slices with their Resume, %d still blocked at the end, %d without a Resume earlier'
           % (n_resumed, len(open_), len(early)))

    # counters
    cap = meta['channel_capacity']
    bad, n = [], 0
    for r in rows(tp, '''SELECT t.name AS name, MIN(c.value) AS lo, MAX(c.value) AS hi, COUNT(*) AS n
                         FROM counter c JOIN counter_track t ON c.track_id = t.id GROUP BY t.name'''):
        n += 1
        chan = r.name[:-len(' elements')] if r.name.endswith(' elements') else r.name
        if r.lo < 0 or chan not in cap or r.hi > cap[chan]:
            bad.append('%s %s..%s (capacity %s)' % (chan, r.lo, r.hi, cap.get(chan)))
    report('counters', not bad and meta['occupancy_mismatches'] == 0,
           '%d channels, all within 0..capacity; decoder vs library count mismatches: %d%s'
           % (n, meta['occupancy_mismatches'], ('; ' + '; '.join(bad)) if bad else ''))
    if 'expected_buffered_flows_per_channel' in meta:
        ok &= check_buffered(tp, meta, report)
    tp.close()
    return ok


def check_buffered(tp, meta, report):
    ok = True

    def rep(name, good, detail):
        nonlocal ok
        ok &= good
        report(name, good, detail)

    flows = {r.ch: r.n for r in rows(tp, '''
        SELECT EXTRACT_ARG(si.arg_set_id, 'args.channel') AS ch, COUNT(*) AS n FROM flow f
        JOIN slice so ON f.slice_out = so.id JOIN slice si ON f.slice_in = si.id
        WHERE so.category = 'buffered' AND si.category = 'buffered' GROUP BY ch''')}
    gets = {r.ch: r for r in rows(tp, '''
        SELECT EXTRACT_ARG(arg_set_id, 'args.channel') AS ch, COUNT(*) AS n,
               SUM(EXTRACT_ARG(arg_set_id, 'args.source') IS NULL) AS no_source,
               SUM(EXTRACT_ARG(arg_set_id, 'args.source_before_window')) AS before
        FROM slice WHERE category = 'buffered' AND name LIKE 'get %' GROUP BY ch''')}
    want = {ch: g.n - g.no_source - g.before for ch, g in gets.items() if g.n - g.no_source - g.before}
    before = ''.join('; %s: %d gets of puts before the window' % (ch, g.before)
                     for ch, g in sorted(gets.items()) if g.before)
    rep('bflows', flows == want and flows == meta['expected_buffered_flows_per_channel'],
        '%d flows on %d channels, one per get%s' % (sum(flows.values()), len(flows), before))
    no_source = sum(g.no_source for g in gets.values())
    rep('bsource', no_source == 0, '%d gets without a source put' % no_source)

    puts = collections.defaultdict(collections.Counter)
    for r in rows(tp, '''SELECT EXTRACT_ARG(arg_set_id, 'args.channel') AS ch,
                                EXTRACT_ARG(arg_set_id, 'args.event') AS ev, COUNT(*) AS n
                         FROM slice WHERE category = 'buffered' AND name LIKE 'put %' GROUP BY ch, ev'''):
        puts[r.ch][r.ev] = r.n
    end = {}                              # last value of each occupancy counter
    for r in rows(tp, '''SELECT t.name AS name, c.value AS v FROM counter c
                         JOIN counter_track t ON c.track_id = t.id ORDER BY c.ts, c.id'''):
        end[r.name[:-len(' elements')] if r.name.endswith(' elements') else r.name] = int(r.v)
    if meta.get('thread_selection'):
        rep('bcount', True, 'not checked (--threads: other threads\' puts and gets are not in the trace)')
    else:
        bad, n = [], 0
        chans = sorted(set(puts) | set(gets))
        for ch in chans:
            p = puts[ch]
            carried = meta['buffered_carried_in'].get(ch, 0)
            n_get = gets[ch].n if ch in gets else 0
            n += sum(p.values())
            if sum(p.values()) + carried != n_get + p['OverwroteOldest'] + p['DroppedNew'] + end.get(ch, 0):
                bad.append('%s: %d puts + %d carried in, %d gets, %d overwritten, %d dropped, %d at the end'
                           % (ch, sum(p.values()), carried, n_get, p['OverwroteOldest'], p['DroppedNew'],
                              end.get(ch, 0)))
        rep('bcount', not bad, '%d puts = gets + overwritten + dropped + still buffered, on %d channels%s'
            % (n, len(chans), ('; ' + '; '.join(bad)) if bad else ''))
    d = rows(tp, '''SELECT COUNT(*) AS n, SUM(EXTRACT_ARG(arg_set_id, 'args.depth') !=
                                             EXTRACT_ARG(arg_set_id, 'args.elements_after')) AS bad
                    FROM slice WHERE category = 'buffered' ''')[0]
    rep('bdepth', not d.bad and meta['occupancy_mismatches'] == 0,
        'replayed depth = library element count: %d puts/gets in the trace (%d differ), '
        'every event of the log (%d differ)' % (d.n, d.bad or 0, meta['occupancy_mismatches']))
    return ok


def main():
    ok = True
    for path in sys.argv[1:]:
        print(path)
        ok &= check(path)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
