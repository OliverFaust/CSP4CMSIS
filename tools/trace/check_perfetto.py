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
  channel's capacity.

Exit status 0 if every check passes.
"""
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
        GROUP BY s.name''')}
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
    tp.close()
    return ok


def main():
    ok = True
    for path in sys.argv[1:]:
        print(path)
        ok &= check(path)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
