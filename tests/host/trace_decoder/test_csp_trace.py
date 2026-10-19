#!/usr/bin/env python3
"""Host-side unit tests of tools/trace/csp_trace.py on synthetic decoded
rows (no target, no ProB).

    python3 tests/host/trace_decoder/test_csp_trace.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools', 'trace'))
import csp_trace as T  # noqa: E402

P1, P2 = '0x100', '0x200'


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
                self.add('AltGuard', thread, alt=alt, guard=i, kind=kind, ticks=target)
            else:
                self.add('AltGuard', thread, alt=alt, guard=i, kind=kind, channel=target)
        return self

    def trace(self):
        return T.Trace(self.rows)


def atd(tr, k=0):
    a = tr.alts[k]
    gl, why = T.atd_match(a)
    pairs, has_t, notes = T.atd_trace(a, T.atd_select(a), gl)
    return gl, why, [e for _, e in pairs], has_t, notes


class Values(unittest.TestCase):
    def test_nested_names(self):
        d = T.split_value('context=0x1, channel=(anon)::t7_run(bool, (anon)::Slot<256u>&)::chA, count=1, capacity=1, slot=0')
        self.assertEqual(d['channel'], '(anon)::t7_run(bool, (anon)::Slot<256u>&)::chA')
        self.assertEqual(d['slot'], '0')
        d = T.split_value('join=csp::Helper<(a)::W, (a)::R>::run(osPriority_t)::sem')
        self.assertEqual(list(d), ['join'])


class AltTimeout(unittest.TestCase):
    def test_timeout_select(self):
        r = Rows().guards(P1, 'A', ('input', 'c'), ('timeout', 3))
        r.add('AltDeadline', P1, context=P1, alt='A', now=20, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x03', guards=2)
        r.add('ResumeTimeout', P1, context=P1, object='A', tick=23, deadline=23)
        r.add('AltSelected', P1, context=P1, guard=1)
        gl, why, ev, has_t, notes = atd(r.trace())
        self.assertEqual(gl, ['GC', '3'])
        self.assertEqual(why, [])
        self.assertEqual(ev, ['a_block.3', 'tick', 'tick', 'tick', 'a_wake.true', 'asel.3'])
        self.assertTrue(has_t)

    def test_ticks_before_and_between_waits(self):
        # shortest timeout 3, first wait with 2 left: 1 tick before it; a
        # stale wakeup at tick 21, the next wait computed at tick 22
        r = Rows().guards(P1, 'A', ('timeout', 3))
        r.add('AltDeadline', P1, context=P1, alt='A', now=21, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A', tick=21, deadline=23)
        r.add('AltDeadline', P1, context=P1, alt='A', now=22, deadline=23)
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('ResumeTimeout', P1, context=P1, object='A', tick=23, deadline=23)
        r.add('AltSelected', P1, context=P1, guard=0)
        _, _, ev, _, _ = atd(r.trace())
        self.assertEqual(ev, ['tick', 'a_block.2', 'a_wake.false', 'tick', 'a_block.1', 'tick', 'a_wake.true', 'asel.3'])

    def test_channel_take_no_ticks(self):
        r = Rows().guards(P1, 'A', ('input', 'rv'))
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A')
        r.add('RendezvousDone', P2, writer='%s (W)' % P2, reader='%s (R)' % P1, channel='rv', by_reader=1)
        r.add('AltSelected', P1, context=P1, guard=0)
        _, _, ev, has_t, _ = atd(r.trace())
        self.assertEqual(ev, ['a_block.-1', 'a_wake.false', 'a_take', 'asel.GC'])
        self.assertFalse(has_t)

    def test_unmatched_reasons(self):
        r = Rows().guards(P1, 'A', ('input', 'a'), ('input', 'b'), ('timeout', 10))
        for _ in range(2):
            r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x07', guards=3)
            r.add('Resume', P1, context=P1, object='A')
            r.add('AltSelected', P1, context=P1, guard=0)
        _, why, _, _, _ = atd(r.trace())
        self.assertIn('timeout 10 ticks (the model\'s delays are 0..3)', why)
        self.assertIn('2 channel guards (the model has one, GC)', why)
        self.assertIn('2 select()s (the model is one select())', why)

    def test_stray_resume_kept(self):
        r = Rows().guards(P1, 'A', ('input', 'c'))
        r.add('WaitAlt', P1, context=P1, alt='A', enabled='0x01', guards=1)
        r.add('Resume', P1, context=P1, object='A')
        r.add('AltSelected', P1, context=P1, guard=0)
        r.add('Resume', P1, context=P1, object='A')      # no open wait
        _, _, ev, _, notes = atd(r.trace())
        self.assertEqual(ev[-1], 'a_wake.false')
        self.assertTrue(any('without an open wait' in n for n in notes))

    def test_thread_inference_skips_blocked_threads(self):
        # P2 is blocked in a WaitAlt when the AltGuard of B is recorded, and
        # its AltSelected comes first: the AltGuard is P1's
        r = Rows().guards(P2, 'A', ('input', 'x'))
        r.add('WaitAlt', P2, context=P2, alt='A', enabled='0x01', guards=1)
        r.add('AltGuard', '', alt='B', guard=0, kind='output', channel='y')
        r.add('Resume', P2, context=P2, object='A')
        r.add('AltSelected', P2, context=P2, guard=0)
        r.add('AltSelected', P1, context=P1, guard=0)
        tr = r.trace()
        b = [a for a in tr.alts if a.alt == 'B'][0]
        self.assertEqual(b.thread, P1)
        self.assertEqual(len(b.selects), 1)


class Buffered(unittest.TestCase):
    def blocking_pair(self, r, chan, cap, slot):
        r.add('WaitBufferedFull', P1, context=P1, channel=chan)
        r.add('Resume', P1, context=P1, object=chan)
        r.add('BufferedPut', P1, context=P1, channel=chan, count=1, capacity=cap, slot=slot)
        r.add('WaitBufferedEmpty', P2, context=P2, channel=chan)
        r.add('Resume', P2, context=P2, object=chan)
        r.add('BufferedGet', P2, context=P2, channel=chan, count=0, capacity=cap, slot=slot)

    def test_part3_identities(self):
        r = Rows()
        self.blocking_pair(r, 'c', 1, 0)
        self.blocking_pair(r, 'c', 1, 0)
        tr = r.trace()
        c = tr.bufs['c']
        self.assertEqual(T.policy(c), 'Block')
        reasons = T.buf_match(c, [])
        self.assertIsNone(reasons['Part 3 SYS_BUF'])
        ev = [e for _, e in T.buf_trace(c, 'Part 3 SYS_BUF', [])]
        self.assertEqual(ev, ['acq_space', 'bin.0', 'acq_item', 'bout.0', 'acq_space', 'bin.1', 'acq_item', 'bout.1'])
        ev = [e for _, e in T.buf_trace(c, 'Part 3 SYS_BUF', [], id_shift=(1, 1))]
        self.assertEqual(ev[-1], 'bout.2')

    def test_capacity_and_policy_reasons(self):
        r = Rows()
        for i in range(3):
            r.add('BufferedPut', P1, context=P1, channel='k', count=min(i + 1, 2), capacity=2, slot=i % 2)
        r.add('OverwroteOldest', P1, context=P1, channel='k', count=2, capacity=2, evicted=0, written=0)
        c = r.trace().bufs['k']
        self.assertEqual(T.policy(c), 'KeepNewest')
        reasons = T.buf_match(c, [])
        self.assertIn('policy KeepNewest (Part 3: Block)', reasons['Part 3 SYS_BUF'])
        self.assertIn('capacity 2 (Part 3: 1)', reasons['Part 3 SYS_BUF'])

    def test_offer_without_overflow_is_not_block(self):
        r = Rows().add('BufferedPut', P1, context=P1, channel='o', count=1, capacity=2, slot=0)
        self.assertEqual(T.policy(r.trace().bufs['o']), 'KeepNewest or KeepOldest')


class Construction(unittest.TestCase):
    def test_environment_escapes_until_the_marker(self):
        env = T.envt(['a_block.2', 'tick'], free_tick=False)
        self.assertEqual(env[0], 'ENV0 = a_block.2 -> ENV1 [] esc_t -> LOOP_P')
        self.assertEqual(env[1], 'ENV1 = tick -> ENV2 [] esc_t -> LOOP_P')
        self.assertEqual(env[2], 'ENV2 = mark_t -> OFFX_P')
        env = T.envt(['a_block.-1'], free_tick=True)
        self.assertIn('tick -> ENV0', env[0])

    def test_verdicts(self):
        f = {'kind': 'failures', 'prob': {'result': 'FAIL', 'counterexample': '[GO:,a_block.2,GO:,mark_t,DEADLOCKS]'}}
        self.assertEqual(T.verdict(f), 'PASS')
        f['prob']['counterexample'] = '[GO:,a_block.2,DEADLOCKS]'
        self.assertTrue(T.verdict(f).startswith('ERROR'))
        f['prob'] = {'result': 'PASS'}
        self.assertEqual(T.verdict(f), 'FAIL')
        self.assertEqual(T.verdict({'prob': {'result': 'PASS'}}), 'PASS')

    def test_alt_guard_mutation_wraps(self):
        r = Rows().guards(P1, 'A', ('input', 'c'), ('timeout', 2))
        r.add('AltSelected', P1, context=P1, guard=1)
        rows, desc = T.mutate_rows(r.rows, 'alt-guard', [0, 1, 2])
        self.assertEqual(rows[2].d['guard'], '0')
        rows, desc = T.mutate_rows(r.rows, 'resume', [0, 1, 2])
        self.assertIsNone(rows)


if __name__ == '__main__':
    unittest.main()
