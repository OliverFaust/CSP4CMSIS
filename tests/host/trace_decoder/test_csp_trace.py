#!/usr/bin/env python3
"""Host-side unit tests of tools/trace/csp_trace.py and check_fair_select.py
on synthetic decoded rows (no target, no ProB).

    python3 tests/host/trace_decoder/test_csp_trace.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools', 'trace'))
import csp_trace as T  # noqa: E402
import check_fair_select as F  # noqa: E402

P1, P2, P3 = '0x100', '0x200', '0x300'


class Models(T.Models):
    """Every configuration available (as in the parameterised models)."""
    def __init__(self):
        super().__init__(tempfile.gettempdir())

    def src(self, model):
        return 'SYSN(\nSYSM(\nSYS_BUFN(\nSYS5G(\nSYS_KNE(\nSYS_KOE(\nSYS(\n'

    def has(self, model, name):
        return True


M = Models()


class Rows:
    """Builds rows as read_rows() returns them (values as text)."""
    def __init__(self):
        self.rows = []

    def add(self, event, thread, **d):
        self.rows.append(T.Row(len(self.rows), len(self.rows) * 1e-3, event, thread,
                               {k: str(v) for k, v in d.items()}))
        return self

    def guards(self, thread, alt, *gl):
        for i, (kind, target) in enumerate(gl):
            if kind == 'timeout':
                self.add('AltGuard', thread, context=thread, alt=alt, guard=i, kind=kind, ticks=target)
            else:
                self.add('AltGuard', thread, context=thread, alt=alt, guard=i, kind=kind, channel=target)
        return self

    def sel(self, thread, alt, g):
        return self.add('AltSelected', thread, context=thread, alt=alt, guard=g)

    def trace(self):
        return T.Trace(self.rows)


def atd(tr, k=0, s=0):
    a = tr.alts[k]
    gl, md, why = T.atd_config(a, M)
    pairs, has_t, notes, aux = T.atd_trace(a, a.all_selects()[s], gl)
    return gl, why, [e for _, e in pairs], has_t, notes


class Values(unittest.TestCase):
    def test_nested_names(self):
        d = T.split_value('context=0x1, channel=(anon)::t7_run(bool, (anon)::Slot<256u>&)::chA, count=1, capacity=1, slot=0')
        self.assertEqual(d['channel'], '(anon)::t7_run(bool, (anon)::Slot<256u>&)::chA')
        self.assertEqual(d['slot'], '0')


class AltTimeout(unittest.TestCase):
    def test_timeout_select(self):
        r = Rows().guards(P1, 'A', ('input', 'c'), ('timeout', 3))
        r.add('AltDeadline', P1, context=P1, alt='A', now=20, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x03', guards=2)
        r.add('ResumeTimeout', P1, context=P1, object='A', tick=23, deadline=23)
        r.sel(P1, 'A', 1)
        gl, why, ev, has_t, notes = atd(r.trace())
        self.assertEqual(gl, ['GC', '3'])
        self.assertEqual(why, [])
        self.assertEqual(ev, ['a_block.3', 'tick', 'tick', 'tick', 'a_wake.true', 'asel.3'])

    def test_ticks_before_and_between_waits(self):
        r = Rows().guards(P1, 'A', ('timeout', 3))
        r.add('AltDeadline', P1, context=P1, alt='A', now=21, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A', tick=21, deadline=23)
        r.add('AltDeadline', P1, context=P1, alt='A', now=22, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('ResumeTimeout', P1, context=P1, object='A', tick=23, deadline=23)
        r.sel(P1, 'A', 0)
        _, _, ev, _, _ = atd(r.trace())
        self.assertEqual(ev, ['tick', 'a_block.2', 'a_wake.false', 'tick', 'a_block.1', 'tick', 'a_wake.true', 'asel.3'])

    def test_channel_guards_and_take(self):
        r = Rows().guards(P1, 'A', ('input', 'a'), ('input', 'b'))
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x03', guards=2)
        r.add('Resume', P1, context=P1, object='A')
        r.add('RendezvousDone', P1, writer='%s (W)' % P2, reader='%s (R)' % P1, channel='b', by_reader=1)
        r.sel(P1, 'A', 1)
        gl, why, ev, has_t, _ = atd(r.trace())
        self.assertEqual(gl, ['GC', 'GC+1'])
        self.assertEqual(ev, ['a_block.-1', 'a_wake.false', 'a_take.1', 'asel.GC+1'])
        self.assertFalse(has_t)

    def test_per_select_windows(self):
        # two selects of one object, attributed by AltSelected's alt=, even
        # with another Alternative of the same thread in between
        r = Rows().guards(P1, 'A', ('input', 'a'))
        r.sel(P1, 'A', 0)
        r.guards(P1, 'B', ('input', 'b'))
        r.sel(P1, 'B', 0)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A')
        r.sel(P1, 'A', 0)
        tr = r.trace()
        a = [x for x in tr.alts if x.alt == 'A'][0]
        self.assertEqual(len(a.selects), 2)
        self.assertEqual(len(a.selects[1].waits), 1)

    def test_unmatched_reasons(self):
        r = Rows().guards(P1, 'A', ('input', 'a'), ('timeout', 30))
        r.sel(P1, 'A', 0)
        _, why, _, _, _ = atd(r.trace())
        self.assertIn('timeout 30 ticks (the model\'s delays are 0..15)', why)

    def test_stray_resume_kept(self):
        r = Rows().guards(P1, 'A', ('input', 'c'))
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A')
        r.sel(P1, 'A', 0)
        r.add('Resume', P1, context=P1, object='A')      # no open wait: in the next (open) select
        tr = r.trace()
        _, _, ev, _, notes = atd(tr, s=1)
        self.assertEqual(ev, ['a_wake.false'])
        self.assertTrue(any('without an open wait' in n for n in notes))


class Names(unittest.TestCase):
    def test_reused_thread_ids(self):
        r = Rows()
        r.add('ProcessStart', P1, context=P1, process='a').add('ProcessName', '', name='first')
        r.add('ProcessEnd', P1, context=P1, process='a')
        r.add('ProcessStart', P1, context=P1, process='b').add('ProcessName', '', name='second')
        tr = r.trace()
        self.assertEqual(tr.name_at(P1, 2), 'first')
        self.assertEqual(tr.name_at(P1, 4), 'second')


class Rendezvous(unittest.TestCase):
    def test_plain_projection(self):
        r = Rows()
        r.add('WaitChannel', P2, context=P2, channel='rv', dir='read')
        r.add('Resume', P2, context=P2, object='rv')
        r.add('WaitChannel', P2, context=P2, channel='rv', dir='read')
        r.add('WaitChannel', P1, context=P1, channel='rv', dir='write')
        r.add('Resume', P1, context=P1, object='rv')
        r.add('RendezvousDone', P1, writer='%s (W)' % P1, reader='%s (R)' % P2, channel='rv', by_reader=0)
        r.add('Resume', P2, context=P2, object='rv')
        tr = r.trace()
        rv = tr.rvs['rv']
        cfg, why = T.rvp_config(rv, tr, M)
        self.assertEqual(cfg, 'SYS(OK, true, true)')
        self.assertEqual([e for _, e in T.rvp_trace(rv)],
                         ['wait_r', 'res_r', 'wait_r', 'wait_w', 'res_w', 'done.false', 'res_r'])


class Buffered(unittest.TestCase):
    def blocking_pair(self, r, chan, cap, slot, pol='Block'):
        r.add('WaitBufferedFull', P1, context=P1, channel=chan)
        r.add('Resume', P1, context=P1, object=chan)
        r.add('BufferedPut', P1, context=P1, channel=chan, count=1, capacity=cap, policy=pol, slot=slot)
        r.add('WaitBufferedEmpty', P2, context=P2, channel=chan)
        r.add('Resume', P2, context=P2, object=chan)
        r.add('BufferedGet', P2, context=P2, channel=chan, count=0, capacity=cap, policy=pol, slot=slot)

    def test_part3_identities(self):
        r = Rows()
        self.blocking_pair(r, 'c', 1, 0)
        self.blocking_pair(r, 'c', 1, 0)
        tr = r.trace()
        c = tr.bufs['c']
        self.assertEqual(c.pol, 'Block')
        self.assertIsNone(T.buf_match(c, tr, M)['Part 3 SYS_BUF'])
        ev = [e for _, e in T.buf_trace(c, 'Part 3 SYS_BUF', tr)[0]]
        self.assertEqual(ev, ['acq_space', 'bin.0', 'acq_item', 'bout.0', 'acq_space', 'bin.1', 'acq_item', 'bout.1'])
        ev = [e for _, e in T.buf_trace(c, 'Part 3 SYS_BUF', tr, id_shift=(1, 1))[0]]
        self.assertEqual(ev[-1], 'bout.2')

    def test_part3n_capacity(self):
        r = Rows()
        for _ in range(3):
            self.blocking_pair(r, 'c', 3, 0)
        tr = r.trace()
        res = T.buf_match(tr.bufs['c'], tr, M)
        self.assertIn('Part 3N SYS_BUFN(3, 1)', res)

    def test_policy_from_the_record(self):
        r = Rows().add('BufferedPut', P1, context=P1, channel='o', count=1, capacity=2, policy='KeepOldest', slot=0)
        tr = r.trace()
        self.assertEqual(tr.bufs['o'].pol, 'KeepOldest')
        self.assertIn('Part 2bN SYS_KOE(2, -1, -1)', T.buf_match(tr.bufs['o'], tr, M))

    def test_alt_ends_per_thread(self):
        # two one-guard Alternatives of one thread, one after the other: one ALT reader
        r = Rows()
        for k in range(2):
            r.guards(P1, 'S%d' % k, ('input', 'm'))
            r.add('WaitAlt', P1, context=P1, alt='S%d' % k, enabled='0x01', guards=1)
            r.add('PutFromISR', 'irq 18', context='irq 18', channel='m', count=1, capacity=2, policy='Block', slot=k)
            r.add('Resume', P1, context=P1, object='S%d' % k)
            r.add('BufferedGet', P1, context=P1, channel='m', count=0, capacity=2, policy='Block', slot=k)
            r.sel(P1, 'S%d' % k, 0)
        tr = r.trace()
        self.assertIn('Part 5 SYS5R', T.buf_match(tr.bufs['m'], tr, M))


    def test_failure_check_before_the_first_put(self):
        # the ALT reader waits before the channel has any event: it is named
        # at the stable point, so the check applies
        r = Rows()
        r.add('ProcessStart', P1, context=P1, process='p').add('ProcessName', '', name='Sampler')
        r.guards(P1, 'S0', ('input', 'm'))
        r.add('WaitAlt', P1, context=P1, alt='S0', enabled='0x01', guards=1)
        r.add('PutFromISR', 'irq 18', context='irq 18', channel='m', count=1, capacity=2, policy='Block', slot=0)
        r.add('Resume', P1, context=P1, object='S0')
        r.add('BufferedGet', P1, context=P1, channel='m', count=0, capacity=2, policy='Block', slot=0)
        r.sel(P1, 'S0', 0)
        tr = r.trace()
        pt = {'start': tr.rows[3].t + 1e-4, 'offers': [('Sampler', 'm ?')]}
        chk = T.buf_failures(tr, {'chan': tr.bufs['m'], 'config': 'Part 5 SYS5R', 'pairs': []}, pt, pt)
        self.assertIsNotNone(chk)
        self.assertNotIn('isr_put', chk['X'])


class Stable(unittest.TestCase):
    def test_a_point_open_at_the_end_of_the_log(self):
        # decode.py marks a stable point that lasts to the end of the log
        with tempfile.NamedTemporaryFile('w', suffix='.csv', delete=False) as f:
            f.write('start_us,end_us,classification,reason,offers\n'
                    '484052.500,2001285.750 (end of log),deadlock,no two offers match,M: c !; P: d !\n'
                    '10.000,20.000,awaiting environment,an interrupt,S: m ?\n')
        try:
            pts = T.read_stable(f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual([p['open_at_end'] for p in pts], [True, False])
        self.assertAlmostEqual(pts[0]['end'], 2.00128575)
        self.assertEqual(pts[0]['offers'], [('M', 'c !'), ('P', 'd !')])


class Construction(unittest.TestCase):
    def test_environment_escapes_until_the_marker(self):
        env = T.envt(['a_block.2', 'tick'], free_tick=False)
        self.assertEqual(env[0], 'ENV0 = a_block.2 -> ENV1 [] esc_t -> LOOP_P')
        self.assertEqual(env[1], 'ENV1 = tick -> ENV2 [] esc_t -> LOOP_P')
        self.assertEqual(env[2], 'ENV2 = mark_t -> OFFX_P')
        self.assertIn('tick -> ENV0', T.envt(['a_block.-1'], free_tick=True)[0])

    def test_long_traces_are_chunked(self):
        defs = T.rec_defs('R', ['e'] * (2 * T.CHUNK + 1))
        self.assertEqual(len(defs), 3)
        self.assertTrue(defs[0].endswith('-> R_1'))
        self.assertTrue(defs[2].endswith('-> STOP'))

    def test_verdicts(self):
        f = {'kind': 'failures', 'prob': {'result': 'FAIL', 'counterexample': '[GO:,a_block.2,GO:,mark_t,DEADLOCKS]'}}
        self.assertEqual(T.verdict(f), 'PASS')
        f['prob']['counterexample'] = '[GO:,a_block.2,DEADLOCKS]'
        self.assertTrue(T.verdict(f).startswith('ERROR'))
        f['prob'] = {'result': 'PASS'}
        self.assertEqual(T.verdict(f), 'FAIL')
        self.assertEqual(T.verdict({'kind': 'trace', 'prob': {'result': 'PASS'}}), 'PASS')

    def test_alt_guard_mutation_wraps(self):
        r = Rows().guards(P1, 'A', ('input', 'c'), ('timeout', 2))
        r.sel(P1, 'A', 1)
        rows, desc = T.mutate_rows(r.rows, 'alt-guard', [0, 1, 2])
        self.assertEqual(rows[2].d['guard'], '0')
        self.assertIsNone(T.mutate_rows(r.rows, 'resume', [0, 1, 2])[0])


class Fairness(unittest.TestCase):
    def test_rule(self):
        r = Rows().guards(P1, 'A', ('input', 'a'), ('input', 'b'))
        # both writers offer (serial wait, resume, pair wait) before the first select
        for w, c in ((P2, 'a'), (P3, 'b')):
            r.add('WaitChannel', w, context=w, channel=c, dir='write')
            r.add('Resume', w, context=w, object=c)
            r.add('WaitChannel', w, context=w, channel=c, dir='write')
        r.add('BufferedPut', P1, context=P1, channel='mark', count=1, capacity=1, policy='Block', slot=0)
        r.add('RendezvousDone', P1, writer='%s (W)' % P2, reader='%s (R)' % P1, channel='a', by_reader=1)
        r.sel(P1, 'A', 0)
        r.add('Resume', P2, context=P2, object='a')
        r.add('WaitChannel', P2, context=P2, channel='a', dir='write')
        r.add('Resume', P2, context=P2, object='a')
        r.add('WaitChannel', P2, context=P2, channel='a', dir='write')
        r.add('BufferedPut', P1, context=P1, channel='mark', count=1, capacity=1, policy='Block', slot=0)
        # second select: fairSelect starts at guard 1 (b, ready): choosing a violates the fair rule
        r.add('RendezvousDone', P1, writer='%s (W)' % P2, reader='%s (R)' % P1, channel='a', by_reader=1)
        r.sel(P1, 'A', 0)
        r.add('RendezvousDone', P1, writer='%s (W)' % P3, reader='%s (R)' % P1, channel='b', by_reader=1)
        tr = r.trace()
        by = F.thread_rows(tr)
        res, viol = F.check(tr, tr.alts[0], True, by)
        self.assertEqual(len(viol), 1)
        self.assertEqual(viol[0][2], 0)
        res, viol = F.check(tr, tr.alts[0], False, by)
        self.assertEqual(viol, [])


if __name__ == '__main__':
    unittest.main()
