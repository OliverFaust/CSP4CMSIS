"""Library-state analysis of a CSP4CMSIS event stream (used by decode.py).

Fed with the CSP4CMSIS events of a log in record order (Analyser.csp) and
the RTOS's switch-ins (Analyser.switch_in), it checks and derives:

- ring slots (buffered channels): every get reads the slot written by the
  put it takes (the oldest, FIFO), that slot was not written again in
  between, every OverwroteOldest evicts exactly the slot of the put it
  replaces, DroppedNew uses no slot;
- ALT offers: every enabled bit of a WaitAlt resolves through the
  Alternative's AltGuard events to (channel, direction) or a timeout;
- stable points: moments when every live CSP process is in an open wait and
  the idle thread runs, with the offer set of the open waits and a
  classification: "awaiting environment" (an open wait that only a timeout
  or an interrupt-fed buffered channel can end), "deadlock" (no two offers
  match, nothing awaits a timeout or an interrupt) or "other" (with the
  reason). Without switch events the same moments are reported as
  "stability unconfirmed".

No channel element values are recorded by the library, so none appear here.
"""
import collections

# CSP4CMSIS message numbers (csp4cmsis_trace.h)
PROCESS_START, PROCESS_END = 0x00, 0x01
RENDEZVOUS_DONE, ALT_SELECTED, ALT_GUARD = 0x10, 0x20, 0x21
BUFFERED_PUT, BUFFERED_GET, PUT_FROM_ISR, DROPPED_NEW, OVERWROTE_OLDEST = 0x30, 0x31, 0x32, 0x33, 0x34
WAIT_CHANNEL, WAIT_FULL, WAIT_EMPTY, WAIT_ALT, WAIT_BARRIER, WAIT_JOIN = 0x60, 0x61, 0x62, 0x63, 0x64, 0x65
RESUME, RESUME_TIMEOUT = 0x6E, 0x6F
NO_SLOT = 0xFFFFFFFF
GUARD_KIND = {0: 'input', 1: 'output', 2: 'timeout', 3: 'other'}
MAX_ERRORS = 20


def unpack_count(c):
    """(elements after, capacity) of a buffered event's third value."""
    return c >> 16, c & 0xFFFF


class Ring:
    """One buffered channel: the puts not yet taken, in FIFO order, and the
    last put that wrote each slot."""
    def __init__(self):
        self.fifo = collections.deque()
        self.writer = {}                  # slot -> put id
        self.count = 0
        self.capacity = None
        self.isr_puts = 0


class Analyser:
    def __init__(self, name=None, isr_fed=(), idle=(), has_switch=True):
        """name(addr) -> text for objects; isr_fed: channel addresses written
        by PutFromISR anywhere in the log; idle: thread ids of the idle
        thread(s); has_switch: the log has RTOS switch events."""
        self.name = name or (lambda a: '0x%08x' % a)
        self.isr_fed = set(isr_fed)
        self.idle = idle if hasattr(idle, '__contains__') else set(idle)   # a set, or any container
        self.has_switch = has_switch
        self.rings = collections.defaultdict(Ring)
        self.next_put = 0
        self.ring = collections.Counter()
        self.ring_errors = []
        self.alt_guards = {}              # Alternative -> {index: (kind, target)}
        self.alt = collections.Counter()
        self.alt_errors = []
        self.masks = collections.Counter()   # (Alternative, its guards, mask, n) -> waits
        self.live = set()                 # threads of running processes
        self.waits = {}                   # thread -> (us, kind, object, offers)
        self.running = None
        self.stable = []                  # finished stable points
        self.current = None               # open stable point

    # ---------------------------------------------------------------- events
    def csp(self, us, msg, vals):
        if vals is None:
            return
        if msg in (BUFFERED_PUT, PUT_FROM_ISR, BUFFERED_GET, DROPPED_NEW, OVERWROTE_OLDEST):
            self.buffered(us, msg, vals)
        elif msg == ALT_GUARD:
            alt, idx, target, kind = vals
            if idx == 0 or alt not in self.alt_guards:
                self.alt_guards[alt] = {}     # a new Alternative (possibly at a reused address)
            self.alt_guards[alt][idx] = (GUARD_KIND.get(kind, 'other'), target)
            self.alt['guards_described'] += 1
        elif msg == PROCESS_START:
            self.live.add(vals[0])
        elif msg == PROCESS_END:
            self.live.discard(vals[0])
            self.waits.pop(vals[0], None)
        elif WAIT_CHANNEL <= msg <= WAIT_JOIN:
            self.waits[vals[0]] = (us, msg, vals[1], self.offers(msg, vals))
        elif msg in (RESUME, RESUME_TIMEOUT):
            self.waits.pop(vals[0], None)
        else:
            return
        self.update(us)

    def switch_in(self, us, tid):
        self.running = tid
        self.update(us)

    def finish(self, end_us):
        if self.current is not None:
            self.close_point(end_us, open_at_end=True)

    # ----------------------------------------------------------------- rings
    def ring_error(self, text):
        self.ring['errors'] += 1
        if len(self.ring_errors) < MAX_ERRORS:
            self.ring_errors.append(text)

    def buffered(self, us, msg, vals):
        ctx, chan, packed, slot = vals
        r = self.rings[chan]
        r.count, r.capacity = unpack_count(packed)
        label = self.name(chan)
        if msg in (BUFFERED_PUT, PUT_FROM_ISR):
            self.ring['puts'] += 1
            if msg == PUT_FROM_ISR:
                r.isr_puts += 1
            if r.capacity and slot >= r.capacity:
                self.ring_error('%s: put at %.3f us wrote slot %d of %d' % (label, us, slot, r.capacity))
            self.next_put += 1
            r.writer[slot] = self.next_put
            r.fifo.append((self.next_put, slot, us))
        elif msg == OVERWROTE_OLDEST:
            self.ring['overwrites'] += 1
            evicted, written = slot >> 16, slot & 0xFFFF
            if r.fifo:
                victim = r.fifo.popleft()
                if victim[1] != evicted:
                    self.ring_error('%s: OverwroteOldest at %.3f us evicted slot %d, the oldest put (%.3f us) wrote slot %d'
                                    % (label, us, evicted, victim[2], victim[1]))
                else:
                    self.ring['overwrites_ok'] += 1
            else:
                self.ring_error('%s: OverwroteOldest at %.3f us with no put in the buffer' % (label, us))
            if evicted != written:
                self.ring['overwrite_slots_differ'] += 1
            self.next_put += 1
            r.writer[written] = self.next_put
            r.fifo.append((self.next_put, written, us))
        elif msg == DROPPED_NEW:
            self.ring['dropped'] += 1
            if slot != NO_SLOT:
                self.ring_error('%s: DroppedNew at %.3f us names slot 0x%x' % (label, us, slot))
            else:
                self.ring['dropped_ok'] += 1
        elif msg == BUFFERED_GET:
            self.ring['gets'] += 1
            if not r.fifo:
                self.ring_error('%s: get at %.3f us from an empty replay' % (label, us))
            else:
                pid, pslot, pus = r.fifo.popleft()
                if pslot != slot:
                    self.ring_error('%s: get at %.3f us read slot %d, its put (%.3f us) wrote slot %d'
                                    % (label, us, slot, pus, pslot))
                elif r.writer.get(slot) != pid:
                    self.ring_error('%s: get at %.3f us: slot %d was written again after its put (%.3f us)'
                                    % (label, us, slot, pus))
                else:
                    self.ring['gets_ok'] += 1
        if len(r.fifo) != r.count:
            self.ring_error('%s: %d elements replayed, library count %d at %.3f us' % (label, len(r.fifo), r.count, us))

    # ---------------------------------------------------------------- offers
    def offers(self, msg, vals):
        """The offers of a wait: ('read'|'write', channel), ('timeout', ticks),
        ('barrier'|'join', object) or ('unresolved', guard index)."""
        obj = vals[1]
        if msg == WAIT_CHANNEL:
            return [('write' if obj & 1 else 'read', obj & ~1)]
        if msg == WAIT_EMPTY:
            return [('read', obj)]
        if msg == WAIT_FULL:
            return [('write', obj)]
        if msg == WAIT_BARRIER:
            return [('barrier', obj)]
        if msg == WAIT_JOIN:
            return [('join', obj)]
        # WaitAlt: a context, b Alternative, c enabled mask, d number of guards
        mask, n = vals[2], vals[3]
        guards = self.alt_guards.get(obj, {})
        self.alt['waits'] += 1
        self.masks[(self.name(obj), self.guard_list(guards), mask, n)] += 1
        if mask & ~((1 << n) - 1):
            self.alt['mask_beyond_guards'] += 1
            self.alt_error('%s: mask 0x%x has bits beyond its %d guards' % (self.name(obj), mask, n))
        if len(guards) != n:
            self.alt['guard_count_differs'] += 1
            self.alt_error('%s: WaitAlt names %d guards, %d described' % (self.name(obj), n, len(guards)))
        out = []
        for i in range(32):
            if not mask >> i & 1:
                continue
            g = guards.get(i)
            self.alt['enabled_bits'] += 1
            if g is None:
                self.alt['unresolved_bits'] += 1
                out.append(('unresolved', i))
            elif g[0] == 'input':
                out.append(('read', g[1]))
            elif g[0] == 'output':
                out.append(('write', g[1]))
            elif g[0] == 'timeout':
                out.append(('timeout', g[1]))
            else:
                out.append(('other guard', i))
        return out

    def guard_list(self, guards):
        """The guards of an Alternative as text, in guard-list order: "c ?" input,
        "c !" output, "timeout N"."""
        out = []
        for i in sorted(guards):
            kind, target = guards[i]
            if kind in ('input', 'output'):
                out.append('%s %s' % (self.name(target), '?' if kind == 'input' else '!'))
            elif kind == 'timeout':
                out.append('timeout %d' % target)
            else:
                out.append('other')
        return ', '.join(out)

    def alt_error(self, text):
        self.alt['errors'] += 1
        if len(self.alt_errors) < MAX_ERRORS:
            self.alt_errors.append(text)

    # ---------------------------------------------------------- stable points
    def condition(self):
        if not self.live or any(t not in self.waits for t in self.live):
            return False
        if self.has_switch:
            return self.running in self.idle
        return True

    def update(self, us):
        now = self.condition()
        if now and self.current is not None and any(self.waits.get(t) != w for t, w in self.current['waits'].items()):
            self.close_point(us)          # a different set of waits: a new point
        if now and self.current is None:
            self.open_point(us)
        elif not now and self.current is not None:
            self.close_point(us)

    def open_point(self, us):
        """Classified when it begins (an interrupt may change a buffer during it)."""
        waits = {t: self.waits[t] for t in self.live}
        kind, reason, offers = self.classify(waits)
        if not self.has_switch:
            reason = 'stability unconfirmed (no RTOS switch events); ' + kind + ': ' + reason
            kind = 'stability unconfirmed'
        self.current = {'start_us': us, 'waits': waits, 'classification': kind, 'reason': reason, 'offers': offers}

    def close_point(self, us, open_at_end=False):
        p = self.current
        self.current = None
        self.stable.append({'start_us': p['start_us'], 'end_us': us, 'open_at_end': open_at_end,
                            'classification': p['classification'], 'reason': p['reason'], 'offers': p['offers']})

    def offer_text(self, o):
        if o[0] in ('read', 'write'):
            return '%s %s' % (self.name(o[1]), '?' if o[0] == 'read' else '!')
        if o[0] == 'timeout':
            return 'timeout %d ticks' % o[1]
        if o[0] in ('barrier', 'join'):
            return '%s %s' % (o[0], self.name(o[1]))
        return '%s %s' % o

    def classify(self, waits):
        offers, env, matches, others = [], [], [], []
        by_chan = collections.defaultdict(lambda: collections.defaultdict(set))
        for tid, (_, _, _, offs) in sorted(waits.items()):
            for o in offs:
                offers.append({'thread': tid, 'offer': self.offer_text(o), 'kind': o[0]})
                if o[0] in ('read', 'write'):
                    by_chan[o[1]][o[0]].add(tid)
                    if o[0] == 'read' and o[1] in self.isr_fed:
                        env.append('%s (interrupt-fed)' % self.name(o[1]))
                elif o[0] == 'timeout':
                    env.append('timeout %d ticks' % o[1])
                else:
                    others.append(self.offer_text(o))
        for chan, sides in by_chan.items():
            if chan in self.rings:            # buffered: the library's count decides
                r = self.rings[chan]
                if sides['read'] and r.count > 0:
                    matches.append('%s: get waits with %d elements' % (self.name(chan), r.count))
                if sides['write'] and r.capacity is not None and r.count < r.capacity:
                    matches.append('%s: put waits with %d of %d' % (self.name(chan), r.count, r.capacity))
            elif any(w != r for w in sides['write'] for r in sides['read']):
                matches.append('%s: reader and writer both offer' % self.name(chan))
        if matches:
            return 'other', 'matching offers while every process waits: ' + '; '.join(matches), offers
        if env:
            return 'awaiting environment', 'only ' + ', '.join(sorted(set(env))) + ' can end a wait', offers
        if others:
            return 'other', 'waits that are not channel offers: ' + ', '.join(sorted(set(others))), offers
        return 'deadlock', 'no two offers match and nothing awaits a timeout or an interrupt', offers

    # ---------------------------------------------------------------- report
    def results(self):
        return {
            'ring': dict(self.ring), 'ring_errors': self.ring_errors,
            'ring_channels': {self.name(c): {'capacity': r.capacity, 'isr_puts': r.isr_puts} for c, r in self.rings.items()},
            'alt': dict(self.alt), 'alt_errors': self.alt_errors,
            'alt_masks': [{'alt': a, 'guard_list': g, 'mask': m, 'guards': n, 'waits': k}
                          for (a, g, m, n), k in sorted(self.masks.items())],
            'stable_points': len(self.stable),
            'stable_classes': dict(collections.Counter(p['classification'] for p in self.stable)),
        }

    def ok(self):
        return not self.ring['errors'] and not self.alt['errors'] and not self.alt['unresolved_bits']
