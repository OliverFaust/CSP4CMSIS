#!/usr/bin/env python3
"""Host-side unit tests of tools/trace/conformance.py on synthetic event
streams (no target, no Event Recorder log).

    python3 tests/host/trace_decoder/test_conformance.py

The deadlock classification is tested here only: no target test deadlocks.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools', 'trace'))
import conformance as C  # noqa: E402

# Thread, channel and Alternative addresses of the synthetic streams.
P1, P2, P3, IDLE, OTHER = 0x100, 0x200, 0x300, 0x900, 0x800
A, B, BUF, ISRBUF, ALT = 0x1000, 0x2000, 0x3000, 0x4000, 0x5000
NAMES = {P1: 'P1', P2: 'P2', P3: 'P3', IDLE: 'idle', A: 'a', B: 'b', BUF: 'buf', ISRBUF: 'isrbuf', ALT: 'alt'}
IRQ = 0x80000012


def analyser(isr_fed=(), has_switch=True):
    return C.Analyser(lambda x: NAMES.get(x, '0x%x' % x), isr_fed, {IDLE}, has_switch)


def start(an, *threads):
    for t in threads:
        an.csp(0, C.PROCESS_START, (t, 0))


def wait_ch(an, us, t, chan, write):
    an.csp(us, C.WAIT_CHANNEL, (t, chan | (1 if write else 0)))


def packed(count, cap):
    return count << 16 | cap


class StablePoints(unittest.TestCase):
    def test_deadlock(self):
        # P1 reads a, P2 reads b, nobody writes either: no two offers match.
        an = analyser()
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, False)
        wait_ch(an, 11, P2, B, False)
        an.switch_in(12, IDLE)
        an.finish(20)
        self.assertEqual(len(an.stable), 1)
        p = an.stable[0]
        self.assertEqual(p['classification'], 'deadlock')
        self.assertEqual((p['start_us'], p['end_us'], p['open_at_end']), (12, 20, True))
        self.assertEqual(sorted(o['offer'] for o in p['offers']), ['a ?', 'b ?'])

    def test_deadlock_cycle_of_writes(self):
        # P1 writes a (P2 should read it), P2 writes b (P1 should read it).
        an = analyser()
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, True)
        wait_ch(an, 11, P2, B, True)
        an.switch_in(12, IDLE)
        an.finish(20)
        self.assertEqual(an.stable[0]['classification'], 'deadlock')

    def test_deadlock_buffered(self):
        # P1 gets from an empty buffer nobody writes; P2 puts to a full one.
        an = analyser()
        start(an, P1, P2)
        an.csp(1, C.BUFFERED_PUT, (P2, BUF, packed(1, 1), 0))
        an.csp(5, C.WAIT_EMPTY, (P1, A))
        an.csp(6, C.WAIT_FULL, (P2, BUF))
        an.switch_in(7, IDLE)
        an.finish(9)
        self.assertEqual(an.stable[0]['classification'], 'deadlock')

    def test_not_stable_while_a_process_runs(self):
        an = analyser()
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, False)
        an.switch_in(12, IDLE)            # P2 has no open wait (e.g. in SleepFor)
        an.finish(20)
        self.assertEqual(an.stable, [])

    def test_not_stable_without_idle(self):
        an = analyser()
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, False)
        wait_ch(an, 11, P2, B, False)
        an.switch_in(12, OTHER)           # a non-process thread runs
        an.finish(20)
        self.assertEqual(an.stable, [])

    def test_awaiting_environment_interrupt(self):
        an = analyser(isr_fed={ISRBUF})
        start(an, P1, P2)
        an.csp(10, C.WAIT_EMPTY, (P1, ISRBUF))
        wait_ch(an, 11, P2, A, False)
        an.switch_in(12, IDLE)
        an.csp(15, C.PUT_FROM_ISR, (IRQ, ISRBUF, packed(1, 4), 0))   # does not end the point
        an.switch_in(16, P1)
        an.csp(16, C.RESUME, (P1, ISRBUF))
        an.finish(30)
        self.assertEqual(len(an.stable), 1)
        p = an.stable[0]
        self.assertEqual(p['classification'], 'awaiting environment')
        self.assertIn('isrbuf (interrupt-fed)', p['reason'])
        self.assertEqual((p['start_us'], p['end_us']), (12, 16))

    def test_awaiting_environment_timeout(self):
        an = analyser()
        start(an, P1)
        an.csp(1, C.ALT_GUARD, (ALT, 0, A, 0))
        an.csp(1, C.ALT_GUARD, (ALT, 1, 5, 2))
        an.csp(2, C.WAIT_ALT, (P1, ALT, 0x3, 2))
        an.switch_in(3, IDLE)
        an.finish(9)
        p = an.stable[0]
        self.assertEqual(p['classification'], 'awaiting environment')
        self.assertEqual(sorted(o['offer'] for o in p['offers']), ['a ?', 'timeout 5 ticks'])

    def test_other_matching_offers(self):
        # Reader and writer of a both wait: a lost wakeup, never a stable state.
        an = analyser()
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, False)
        wait_ch(an, 11, P2, A, True)
        an.switch_in(12, IDLE)
        an.finish(20)
        self.assertEqual(an.stable[0]['classification'], 'other')
        self.assertIn('matching offers', an.stable[0]['reason'])

    def test_other_barrier(self):
        an = analyser()
        start(an, P1)
        an.csp(10, C.WAIT_BARRIER, (P1, 0x7000))
        an.switch_in(12, IDLE)
        an.finish(20)
        self.assertEqual(an.stable[0]['classification'], 'other')

    def test_unconfirmed_without_switch_events(self):
        an = analyser(has_switch=False)
        start(an, P1, P2)
        wait_ch(an, 10, P1, A, False)
        wait_ch(an, 11, P2, B, False)
        an.finish(20)
        p = an.stable[0]
        self.assertEqual(p['classification'], 'stability unconfirmed')
        self.assertIn('deadlock', p['reason'])

    def test_new_point_when_waits_change(self):
        # P3 is not a process: its activity does not end the point; P1's new
        # wait after an ISR-fed get does.
        an = analyser(isr_fed={ISRBUF})
        start(an, P1, P2)
        an.csp(10, C.WAIT_EMPTY, (P1, ISRBUF))
        wait_ch(an, 11, P2, A, False)
        an.switch_in(12, IDLE)
        an.csp(20, C.PUT_FROM_ISR, (IRQ, ISRBUF, packed(1, 4), 0))
        an.switch_in(21, P1)
        an.csp(21, C.RESUME, (P1, ISRBUF))
        an.csp(22, C.BUFFERED_GET, (P1, ISRBUF, packed(0, 4), 0))
        an.csp(23, C.WAIT_EMPTY, (P1, ISRBUF))
        an.switch_in(24, IDLE)
        an.finish(30)
        self.assertEqual([(p['start_us'], p['end_us']) for p in an.stable], [(12, 21), (24, 30)])


class RingSlots(unittest.TestCase):
    def test_fifo_and_wrap(self):
        an = analyser()
        ops = [(C.BUFFERED_PUT, 1, 0), (C.BUFFERED_PUT, 2, 1), (C.BUFFERED_GET, 1, 0), (C.BUFFERED_PUT, 2, 0),
               (C.BUFFERED_GET, 1, 1), (C.BUFFERED_GET, 0, 0)]
        for i, (msg, count, slot) in enumerate(ops):
            an.csp(i, msg, (P1, BUF, packed(count, 2), slot))
        self.assertEqual(an.ring['errors'], 0, an.ring_errors)
        self.assertEqual(an.ring['gets_ok'], 3)

    def test_get_reads_wrong_slot(self):
        an = analyser()
        an.csp(0, C.BUFFERED_PUT, (P1, BUF, packed(1, 2), 0))
        an.csp(1, C.BUFFERED_GET, (P1, BUF, packed(0, 2), 1))
        self.assertEqual(an.ring['errors'], 1)

    def test_slot_written_again_before_get(self):
        # Two puts claim slot 0: the first put's element was overwritten.
        an = analyser()
        an.csp(0, C.BUFFERED_PUT, (P1, BUF, packed(1, 2), 0))
        an.csp(1, C.BUFFERED_PUT, (P1, BUF, packed(2, 2), 0))
        an.csp(2, C.BUFFERED_GET, (P1, BUF, packed(1, 2), 0))
        self.assertIn('written again', ' '.join(an.ring_errors))

    def test_overwrite_evicts_oldest(self):
        an = analyser()
        an.csp(0, C.BUFFERED_PUT, (P1, BUF, packed(1, 2), 0))
        an.csp(1, C.BUFFERED_PUT, (P1, BUF, packed(2, 2), 1))
        an.csp(2, C.OVERWROTE_OLDEST, (P1, BUF, packed(2, 2), 0 << 16 | 0))
        an.csp(3, C.OVERWROTE_OLDEST, (P1, BUF, packed(2, 2), 0 << 16 | 0))   # wrong: the oldest is slot 1
        self.assertEqual(an.ring['overwrites_ok'], 1)
        self.assertEqual(an.ring['errors'], 1)

    def test_dropped_uses_no_slot(self):
        an = analyser()
        an.csp(0, C.BUFFERED_PUT, (P1, BUF, packed(1, 2), 0))
        an.csp(1, C.BUFFERED_PUT, (P1, BUF, packed(2, 2), 1))
        an.csp(2, C.DROPPED_NEW, (P1, BUF, packed(2, 2), C.NO_SLOT))
        an.csp(3, C.DROPPED_NEW, (P1, BUF, packed(2, 2), 0))
        self.assertEqual(an.ring['dropped_ok'], 1)
        self.assertEqual(an.ring['errors'], 1)   # the second names a slot


class AltOffers(unittest.TestCase):
    def test_mask_resolves_in_guard_list_order(self):
        an = analyser()
        an.csp(0, C.ALT_GUARD, (ALT, 0, A, 0))
        an.csp(0, C.ALT_GUARD, (ALT, 1, B, 1))
        an.csp(0, C.ALT_GUARD, (ALT, 2, 7, 2))
        self.assertEqual(an.offers(C.WAIT_ALT, (P1, ALT, 0b101, 3)), [('read', A), ('timeout', 7)])
        self.assertEqual(an.alt['unresolved_bits'], 0)

    def test_unresolved_bit(self):
        an = analyser()
        an.csp(0, C.ALT_GUARD, (ALT, 0, A, 0))
        an.offers(C.WAIT_ALT, (P1, ALT, 0b11, 2))
        self.assertEqual(an.alt['unresolved_bits'], 1)
        self.assertFalse(an.ok())

    def test_new_alternative_at_same_address(self):
        an = analyser()
        an.csp(0, C.ALT_GUARD, (ALT, 0, A, 0))
        an.csp(0, C.ALT_GUARD, (ALT, 1, B, 0))
        an.csp(5, C.ALT_GUARD, (ALT, 0, B, 1))     # a new Alternative, one guard
        self.assertEqual(an.offers(C.WAIT_ALT, (P1, ALT, 0b1, 1)), [('write', B)])
        self.assertEqual(an.alt['errors'], 0)


class Deadlines(unittest.TestCase):
    def alt(self, *delays):
        """An analyser with ALT: guard 0 reads a, then one timeout guard per delay."""
        an = analyser()
        an.csp(0, C.ALT_GUARD, (ALT, 0, A, 0))
        for i, d in enumerate(delays):
            an.csp(0, C.ALT_GUARD, (ALT, 1 + i, d, 2))
        return an

    def wait(self, an, us, now, deadline, n=2):
        an.csp(us, C.ALT_DEADLINE, (P1, ALT, now, deadline))
        an.csp(us, C.WAIT_ALT, (P1, ALT, (1 << n) - 1, n))

    def test_timeout_at_and_after_deadline(self):
        an = self.alt(5)
        self.wait(an, 1, 100, 105)
        an.csp(2, C.RESUME_TIMEOUT, (P1, ALT, 105, 105))
        an.csp(3, C.ALT_SELECTED, (P1, 1))
        self.wait(an, 4, 200, 205)
        an.csp(5, C.RESUME_TIMEOUT, (P1, ALT, 207, 205))
        self.assertEqual((an.dl['errors'], an.dl['resume_timeout'], an.dl['timeout_ticks_late_max']), (0, 2, 2))

    def test_timeout_before_deadline(self):
        an = self.alt(5)
        self.wait(an, 1, 100, 105)
        an.csp(2, C.RESUME_TIMEOUT, (P1, ALT, 104, 105))
        self.assertEqual(an.dl['errors'], 1)

    def test_resume_before_at_after(self):
        an = self.alt(5)
        for tick in (103, 105, 106):
            self.wait(an, 1, 100, 105)
            an.csp(2, C.RESUME, (P1, ALT, tick, 105))
        self.assertEqual((an.dl['resume_before'], an.dl['resume_at'], an.dl['resume_after'], an.dl['errors']),
                         (1, 1, 1, 1))

    def test_tick_wrap(self):
        an = self.alt(5)
        self.wait(an, 1, 0xFFFFFFFE, 3)
        an.csp(2, C.RESUME_TIMEOUT, (P1, ALT, 4, 3))
        self.assertEqual(an.dl['errors'], 0)

    def test_missing_deadline(self):
        an = self.alt(5)
        an.csp(1, C.WAIT_ALT, (P1, ALT, 0b11, 2))
        self.assertEqual(an.dl['errors'], 1)

    def test_deadline_without_timeout_guard(self):
        an = self.alt(5)
        self.wait(an, 1, 100, 105, n=1)        # only guard 0 (the channel) enabled
        self.assertEqual(an.dl['errors'], 1)

    def test_time_left_beyond_shortest_timeout(self):
        an = self.alt(9, 3)
        self.wait(an, 1, 100, 104, n=3)        # 4 ticks left, shortest timeout 3
        self.assertEqual(an.dl['errors'], 1)

    def test_deadline_moves_within_select(self):
        an = self.alt(5)
        self.wait(an, 1, 100, 105)
        an.csp(2, C.RESUME, (P1, ALT, 101, 105))   # stale wakeup: a new round
        self.wait(an, 3, 101, 106)
        self.assertEqual(an.dl['errors'], 1)

    def test_zero_timeout_never_waits(self):
        an = self.alt(0)
        self.wait(an, 1, 100, 101)
        self.assertGreaterEqual(an.dl['errors'], 1)

    def test_plain_wait_has_no_ticks(self):
        an = analyser()
        an.csp(0, C.ALT_GUARD, (ALT, 0, A, 0))
        an.csp(1, C.WAIT_ALT, (P1, ALT, 0b1, 1))
        an.csp(2, C.RESUME, (P1, ALT))
        an.csp(3, C.RESUME_TIMEOUT, (P1, ALT))
        self.assertEqual(an.dl['errors'], 1)        # a ResumeTimeout without a deadline


if __name__ == '__main__':
    unittest.main(verbosity=2)
