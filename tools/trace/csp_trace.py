#!/usr/bin/env python3
"""Conformance of a recorded CSP4CMSIS trace against docs/formal/*.csp.

    csp_trace.py EVENTS.csv[.xz] --out DIR [--stable STABLE.csv] [--run]
                 [--jobs N] [--cache DIR] [--mutate KIND] [--prob PROBCLI]

EVENTS.csv: tools/trace/decode.py -o; STABLE.csv: decode.py --stable.

1. Projections. One Alternative object (AltGuard index 0 starts a new one),
   or one buffered channel, or one rendezvous channel, with the events of
   the threads that use it (mappings/<model>.json says which).
2. Instance matching. The models are fixed configurations (one or two
   select()s, capacities 1 and 2, given partners). Every projection is
   matched against each model's configurations; a projection that matches
   none is listed with the reason.
3. Trace checks. For every matched projection a CSPm file includes the model
   and asserts  MODEL \\ H [T= RECORDED,  RECORDED being the projection's
   trace renamed into the model's alphabet (mappings/<model>.json).
4. Stable failures (--stable). At a stable point t of a matched projection,
   X = the projection's observable events except the environment events the
   point awaits. The check composes MODEL \\ H with an environment that
   performs t, then a marker, then offers only X; before the marker it can
   always escape (so no deadlock is possible there). A deadlock exists iff
   the model can be stable after t refusing X: the assertion
   ":[deadlock free [F]]" must FAIL, with a counterexample ending in the
   marker.
5. Mutations (--mutate swap|drop-rv|alt-guard|get-id|resume): one fault per
   applicable model, injected into a copy of the decoded rows (get-id: into
   the identity derived from the slot replay); each mutant must FAIL.

Writes DIR/report.json, DIR/match.txt and DIR/checks/*.csp; with --run
probcli checks each distinct assertion once (--cache: across runs).
No channel element values are involved: the trace has none.
"""
import argparse, collections, concurrent.futures, csv, hashlib, json, lzma, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROB = os.environ.get('PROBCLI', 'probcli')
TIMEOUT = 3600

ATD, BUF, OWR, OWX = 'alt_timeout_deadline', 'buffered_channel_v2', 'alt_one_winner', 'alt_owrv_extended'
GC = 'GC'
BUF_EVENTS = ('BufferedPut', 'PutFromISR', 'BufferedGet', 'DroppedNew', 'OverwroteOldest')


# ------------------------------------------------------------------ parsing
def split_value(v):
    """'k=v, k=v' at bracket depth 0 (names contain ', ' inside () and <>)."""
    out, depth, cur = [], 0, ''
    i = 0
    while i < len(v):
        c = v[i]
        if c in '(<[':
            depth += 1
        elif c in ')>]':
            depth -= 1
        if depth == 0 and v.startswith(', ', i):
            out.append(cur)
            cur = ''
            i += 2
            continue
        cur += c
        i += 1
    if cur:
        out.append(cur)
    d = {}
    for part in out:
        k, _, val = part.partition('=')
        d[k.strip()] = val
    return d


def tid(text):
    m = re.match(r'(0x[0-9a-fA-F]+)', text or '')
    return m.group(1).lower() if m else (text or '')


class Row:
    __slots__ = ('i', 't', 'event', 'thread', 'd', 'over')

    def __init__(self, i, t, event, thread, d):
        self.i, self.t, self.event, self.thread, self.d, self.over = i, t, event, thread, d, {}


SWITCH_IN = {('RTX Thread', 'ThreadSwitched'), ('FreeRTOS Tasks', 'TaskSwitchedIn')}


def read_rows(path):
    """The CSP4CMSIS rows. AltGuard has no context value: its thread is the
    running thread from the RTOS switch events (RTX5, FreeRTOS 11.3); without
    them (ST's FreeRTOS) Trace.build infers it (row.over['inferred'])."""
    f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
    rows, running = [], ''
    for r in csv.DictReader(f):
        if (r['component'], r['event']) in SWITCH_IN:
            running = tid(r['thread'])
            continue
        if r['component'] != 'CSP':
            continue
        th = tid(r['thread']) or r['thread']
        if r['event'] == 'AltGuard':
            th = running
        rows.append(Row(len(rows), float(r['model_time_s']), r['event'], th, split_value(r['value'])))
    return rows


def tick_diff(a, b):
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d & 0x80000000 else d


# --------------------------------------------------------------- structures
class Select:
    def __init__(self, start):
        self.start = start            # row index of its first record (None: no record before AltSelected)
        self.waits = []               # {'deadline': (now, dl) | None, 'wait': row, 'resume': (event, tick|None, row) | None}
        self.pending = None           # AltDeadline not yet followed by WaitAlt
        self.selected = None
        self.end = None               # AltSelected row
        self.take = None              # transfer row of the selected channel guard
        self.rows = []


class AltInst:
    def __init__(self, n, alt, thread, row):
        self.id = 'alt%d' % n
        self.alt, self.thread, self.first = alt, thread, row.i
        self.guards = {}              # index -> (kind, target)
        self.selects = []
        self.cur = None
        self.strays = []              # Resume/ResumeTimeout naming it with no open wait: (row, event)

    def glist(self):
        return [self.guards[i] for i in sorted(self.guards)]


class Chan:
    def __init__(self, n, name):
        self.id = 'buf%d' % n
        self.name = name
        self.cap = None
        self.items = []               # (kind, row, extra) in record order
        self.ev = collections.Counter()
        self.full_waits = 0
        self.n = 0                    # writes so far
        self.slot = {}                # slot -> write index
        self.writers = collections.defaultdict(collections.Counter)   # thread -> {role: count}
        self.readers = collections.defaultdict(collections.Counter)


class RvChan:
    def __init__(self, n, name):
        self.id = 'rv%d' % n
        self.name = name
        self.done = []
        self.writers = collections.Counter()
        self.readers = collections.Counter()


class Trace:
    """Everything the generator derives from the decoded rows."""
    def __init__(self, rows):
        self.rows = rows
        self.alts, self.bufs, self.rvs = [], {}, {}
        self.names = {}               # thread -> process name
        self.build()

    def chan_of(self, name):
        if name not in self.bufs:
            self.bufs[name] = Chan(len(self.bufs), name)
        return self.bufs[name]

    def infer_alt_threads(self):
        """AltGuard rows without a thread (no RTOS switch events): the thread
        of the next row naming the Alternative (AltDeadline, WaitAlt), else of
        the next AltSelected, among the threads not blocked in an open wait
        at the AltGuard row (a blocked thread records nothing)."""
        self.inferred = 0
        blocked = set()
        pending = []                          # AltGuard rows to resolve: (row, candidates excluded)
        for k, row in enumerate(self.rows):
            ev, th = row.event, row.thread
            if ev == 'AltGuard' and not th:
                pending.append((row, set(blocked)))
            elif pending and ev in ('AltDeadline', 'WaitAlt', 'AltSelected'):
                for item in list(pending):
                    g, excl = item
                    if th in excl or th.startswith('irq'):
                        continue
                    if ev == 'AltSelected' or row.d.get('alt') == g.d['alt']:
                        g.thread = th
                        g.over['inferred'] = True
                        self.inferred += 1
                        pending.remove(item)
            if ev.startswith('Wait'):
                blocked.add(th)
            elif ev in ('Resume', 'ResumeTimeout'):
                blocked.discard(th)

    def build(self):
        self.infer_alt_threads()
        cur_by_alt, thread_cur = {}, {}
        last_ev = {}                  # thread -> (event, object) of its last CSP record
        window = collections.defaultdict(list)   # thread -> transfers since its last AltSelected
        last_start = None
        for row in self.rows:
            ev, th, d = row.event, row.thread, row.d
            if ev == 'ProcessStart':
                last_start = th
            elif ev == 'ProcessName' and last_start:
                self.names[last_start] = d.get('name', '')
            elif ev == 'AltGuard':
                a, idx = d['alt'], int(d['guard'])
                if idx == 0 or a not in cur_by_alt:
                    inst = AltInst(len(self.alts), a, th, row)
                    self.alts.append(inst)
                    cur_by_alt[a] = inst
                    inst.cur = Select(row.i)
                    window[th] = []
                inst = cur_by_alt[a]
                target = d.get('channel') or d.get('ticks') or d.get('target')
                inst.guards[idx] = (d['kind'], int(target) if d['kind'] == 'timeout' else target)
                inst.cur and inst.cur.rows.append(row)
                thread_cur[th] = inst
            elif ev in ('AltDeadline', 'WaitAlt'):
                inst = cur_by_alt.get(d['alt'])
                if inst is None:
                    continue
                thread_cur[th] = inst
                if inst.cur is None:
                    inst.cur = Select(row.i)
                s = inst.cur
                s.rows.append(row)
                if ev == 'AltDeadline':
                    s.pending = (int(d['now']), int(d['deadline']))
                else:
                    s.waits.append({'deadline': s.pending, 'wait': row, 'resume': None,
                                    'mask': int(d['enabled'], 16)})
                    s.pending = None
            elif ev in ('Resume', 'ResumeTimeout') and d.get('object') in cur_by_alt \
                    and cur_by_alt[d['object']].thread == th and cur_by_alt[d['object']].cur \
                    and cur_by_alt[d['object']].cur.waits and cur_by_alt[d['object']].cur.waits[-1]['resume'] is None:
                s = cur_by_alt[d['object']].cur
                s.rows.append(row)
                tick = int(d['tick']) if 'tick' in d else None
                s.waits[-1]['resume'] = (ev, tick, row)
            elif ev in ('Resume', 'ResumeTimeout') and d.get('object') in cur_by_alt \
                    and cur_by_alt[d['object']].thread == th:
                cur_by_alt[d['object']].strays.append((row, ev))   # kept in the trace at its position
            elif ev == 'AltSelected':
                inst = thread_cur.get(th)
                if inst is None or inst.thread != th:
                    continue
                if inst.cur is None:
                    inst.cur = Select(None)
                s = inst.cur
                s.rows.append(row)
                s.selected, s.end = int(d['guard']), row
                g = inst.guards.get(s.selected)
                if g and g[0] in ('input', 'output'):
                    lo = s.start if s.start is not None else -1
                    cands = [r for (r, kind, chan, role) in window[th]
                             if chan == g[1] and r.i > lo and role == ('read' if g[0] == 'input' else 'write')]
                    s.take = cands[-1] if cands else None
                    if s.take is not None:
                        s.rows.append(s.take)
                        s.rows.sort(key=lambda r: r.i)
                inst.selects.append(s)
                inst.cur = None
                window[th] = []
            if ev in BUF_EVENTS:
                self.buffered(row, last_ev, window, thread_cur)
            elif ev in ('WaitBufferedEmpty', 'WaitBufferedFull'):
                c = self.chan_of(d['channel'])
                c.items.append(('wait_' + ('empty' if ev == 'WaitBufferedEmpty' else 'full'), row, None))
                if ev == 'WaitBufferedFull':
                    c.full_waits += 1
            elif ev in ('Resume', 'ResumeTimeout') and last_ev.get(th, (None, None))[0] in ('WaitBufferedEmpty', 'WaitBufferedFull') \
                    and last_ev[th][1] == d.get('object'):
                c = self.chan_of(d['object'])
                c.items.append(('acq_item' if last_ev[th][0] == 'WaitBufferedEmpty' else 'acq_space', row, None))
            elif ev == 'RendezvousDone':
                c = d['channel']
                if c not in self.rvs:
                    self.rvs[c] = RvChan(len(self.rvs), c)
                w, r = tid(d['writer']), tid(d['reader'])
                self.rvs[c].done.append(row)
                self.rvs[c].writers[w] += 1
                self.rvs[c].readers[r] += 1
                window[w].append((row, 'rv', c, 'write'))
                window[r].append((row, 'rv', c, 'read'))
            if th and ev not in ('ProcessName',):
                last_ev[th] = (ev, d.get('object') or d.get('channel') or d.get('alt'))

    def buffered(self, row, last_ev, window, thread_cur):
        ev, th, d = row.event, row.thread, row.d
        c = self.chan_of(d['channel'])
        c.cap = int(d['capacity'])
        c.ev[ev] += 1
        prev = last_ev.get(th, (None, None))
        inst = thread_cur.get(th)
        alt_kind = None
        if inst is not None:
            for kind, target in inst.guards.values():
                if target == c.name and kind in ('input', 'output'):
                    alt_kind = alt_kind or set()
                    alt_kind.add(kind)
        if ev in ('BufferedPut', 'PutFromISR', 'OverwroteOldest', 'DroppedNew'):
            n = c.n
            c.n += 1
            slot = int(d['slot']) if d.get('slot', '').isdigit() else None
            if ev == 'OverwroteOldest':
                slot = int(d['written'])
            if ev != 'DroppedNew' and slot is not None:
                c.slot[slot] = n
            if ev == 'PutFromISR' or th.startswith('irq'):
                role = 'isr'
            elif prev[0] == 'Resume' and prev[1] == c.name:
                role = 'blocking'
            elif alt_kind and 'output' in alt_kind and ev == 'BufferedPut':
                role = 'alt'
            else:
                role = 'offer'
            c.writers[th][role] += 1
            c.items.append(('write', row, {'n': n, 'role': role}))
            if ev == 'BufferedPut' and role == 'alt':
                window[th].append((row, 'buf', c.name, 'write'))
        else:
            slot = int(d['slot'])
            ident = c.slot.get(slot)
            if prev[0] == 'Resume' and prev[1] == c.name:
                role = 'blocking'
            elif alt_kind and 'input' in alt_kind:
                role = 'alt'
            else:
                role = 'other'
            c.readers[th][role] += 1
            c.items.append(('get', row, {'id': ident, 'role': role}))
            if role == 'alt':
                window[th].append((row, 'buf', c.name, 'read'))


# ------------------------------------------------------------ the policies
def policy(c):
    if c.ev['OverwroteOldest']:
        return 'KeepNewest'
    if c.ev['DroppedNew']:
        return 'KeepOldest'
    if c.full_waits:
        return 'Block'
    if any(r == 'offer' for w in c.writers.values() for r in w):
        return 'KeepNewest or KeepOldest'
    return 'unknown'


def short(name):
    return re.sub(r'\(anonymous namespace\)::', '', name)


# --------------------------------------------------------- ATD: one select
def atd_match(inst):
    gl, why = [], []
    chans = [g for g in inst.glist() if g[0] in ('input', 'output')]
    for kind, target in inst.glist():
        if kind in ('input', 'output'):
            gl.append(GC)
        elif kind == 'timeout':
            if target > 3:
                why.append('timeout %d ticks (the model\'s delays are 0..3)' % target)
            gl.append(str(target))
        else:
            why.append('a guard of kind %s' % kind)
    if len(chans) > 1:
        why.append('%d channel guards (the model has one, GC)' % len(chans))
    nsel = len(inst.selects) + (1 if inst.cur is not None and (inst.cur.waits or inst.cur.start is not None) else 0)
    if nsel != 1:
        why.append('%d select()s (the model is one select())' % nsel)
    if len(inst.guards) != max(inst.guards, default=-1) + 1:
        why.append('guard list incomplete')
    return gl, why


def atd_select(inst):
    return inst.selects[0] if inst.selects else inst.cur


def atd_trace(inst, sel, gl):
    """The select's events in the model's alphabet (asel = renamed a_sel)."""
    delays = [int(g) for g in gl if g != GC]
    has_t = bool(delays)
    out, prev, notes = [], None, []        # out: (row index | None for an inferred tick, event)
    for w in sel.waits:
        if w['deadline'] is not None:
            now, dl = w['deadline']
            r = tick_diff(dl, now)
            e = (min(delays) - r) if prev is None else tick_diff(now, prev)
            if e < 0:
                notes.append('negative tick gap %d' % e)
                e = 0
            out += [(None, 'tick')] * e
            out.append((w['wait'].i, 'a_block.%d' % r))
        else:
            if has_t:
                notes.append('a wait without AltDeadline in a select with timeout guards')
            out.append((w['wait'].i, 'a_block.-1'))
            r = None
        if w['resume'] is None:
            break                                   # the log ends in this wait
        kind, tick, rrow = w['resume']
        wake = (rrow.i, 'a_wake.%s' % ('true' if kind == 'ResumeTimeout' else 'false'))
        if r is not None and tick is not None:
            k = tick_diff(tick, now)
            before = min(max(k, 0), r)
            out += [(None, 'tick')] * before + [wake] + [(None, 'tick')] * max(k - before, 0)
            prev = tick
        else:
            out.append(wake)
    if sel.selected is not None:
        if sel.take is not None:
            out.append((sel.take.i, 'a_take'))
        if sel.selected >= len(gl):
            notes.append('AltSelected guard %d beyond the %d guards' % (sel.selected, len(gl)))
        else:
            out.append((sel.end.i, 'asel.%s' % gl[sel.selected]))
    for row, ev in inst.strays:
        notes.append('%s row %d without an open wait' % (ev, row.i))
        e = (row.i, 'a_wake.%s' % ('true' if ev == 'ResumeTimeout' else 'false'))
        pos = next((j for j, (ri, _) in enumerate(out) if ri is not None and ri > row.i), len(out))
        out.insert(pos, e)
    if not has_t:
        out = [x for x in out if x[1] != 'tick']
    return out, has_t, notes


def atd_partner(gl, sel):
    return 1 if GC in gl else 0


def atd_check(gl, p, has_t, trace, name):
    obs = '{| a_block, a_wake, a_sel, tick, a_take |}' if has_t else '{| a_block, a_wake, a_sel, a_take |}'
    sysx = 'SYS(OK, <%s>, %d, STALE, 0)' % (', '.join(gl), p)
    body = ['channel asel : GS',
            'OBS_P = %s' % obs,
            'M_P = (%s \\ diff(Events, OBS_P)) [[ a_sel.g.e.b <- asel.g | g <- GS, e <- TT, b <- Bool ]]' % sysx,
            'REC_P = %s' % chain(trace)]
    return {'model': ATD, 'config': sysx, 'decls': body, 'assert': 'M_P [T= REC_P', 'length': len(trace), 'name': name}


def chain(events):
    return ' -> '.join(events + ['STOP']) if events else 'STOP'


# ------------------------------------------------- buffered: configurations
def buf_match(c, alts_on):
    """(config name, why-not) for the buffered model's configurations."""
    pol = policy(c)
    wroles = collections.Counter()
    for w in c.writers.values():
        wroles.update(w)
    rroles = collections.Counter()
    for r in c.readers.values():
        rroles.update(r)
    writes = sum(wroles.values())
    gets = sum(rroles.values())
    alt_r = [a for a in alts_on if any(k == 'input' and t == c.name for k, t in a.guards.values())]
    alt_w = [a for a in alts_on if any(k == 'output' and t == c.name for k, t in a.guards.values())]
    multi = [a for a in alt_r + alt_w if len(a.guards) > 1]
    reasons = {}
    # Part 1c
    why = []
    if c.ev['OverwroteOldest'] or c.ev['DroppedNew']:
        why.append('overwrites/drops (%s)' % pol)
    if wroles['isr']:
        why.append('ISR writes (the model\'s writer is a task)')
    if len(alt_r) != 1:
        why.append('%d ALT readers (the model has one)' % len(alt_r))
    elif len(alt_r[0].guards) != 1:
        why.append('the ALT reader has %d guards (the model: one)' % len(alt_r[0].guards))
    elif len(alt_r[0].selects) != 1:
        why.append('the ALT reader makes %d select()s (the model: one)' % len(alt_r[0].selects))
    if alt_w:
        why.append('an ALT writer')
    writer_threads = [t for t, w in c.writers.items() if sum(w.values())]
    if writes == 1 and rroles['alt'] == 1 and not rroles['blocking'] and not why:
        reasons['Part 1c SYS_C'] = None
    elif writes == 2 and len(writer_threads) == 1 and rroles['alt'] == 1 and rroles['blocking'] == 1 \
            and len([t for t, r in c.readers.items() if r['blocking']]) == 1 and not why:
        reasons['Part 1c SYS_C2'] = None
    else:
        why2 = list(why)
        if not (writes == 1 or writes == 2):
            why2.append('%d writes (SYS_C: one, SYS_C2: two)' % writes)
        if rroles['blocking'] > 1:
            why2.append('%d blocking gets (SYS_C2: one)' % rroles['blocking'])
        reasons['Part 1c SYS_C'] = '; '.join(why2) or 'roles differ (SYS_C: one write and one ALT get; SYS_C2: two writes by one task, one ALT get, one blocking get)'
    # Part 2 KeepNewest
    why = []
    if pol != 'KeepNewest':
        why.append('policy %s (Part 2: KeepNewest)' % pol)
    if c.cap != 2:
        why.append('capacity %s (Part 2: 2)' % c.cap)
    first = [it for it in c.items if it[0] in ('write', 'get')][:2]
    if len(first) < 2 or any(it[0] != 'write' or it[1].event != 'BufferedPut' for it in first):
        why.append('does not start with two BufferedPut (the initial state <0, 1>)')
    later = [it for it in c.items if it[0] == 'write'][2:]
    tw = sum(1 for it in later if it[2]['role'] != 'isr')
    iw = sum(1 for it in later if it[2]['role'] == 'isr')
    if tw > 1 or iw > 1:
        why.append('%d task and %d ISR writes after the initial state (Part 2: one each)' % (tw, iw))
    reasons['Part 2 SYS_KN_NEW'] = '; '.join(why) or None
    # Part 2b KeepOldest
    why = []
    if pol != 'KeepOldest':
        why.append('policy %s (Part 2b: KeepOldest)' % pol)
    if c.cap != 2:
        why.append('capacity %s (Part 2b: 2)' % c.cap)
    if wroles['isr'] > 1 or writes - wroles['isr'] > 1:
        why.append('%d task and %d ISR writes (Part 2b: one each)' % (writes - wroles['isr'], wroles['isr']))
    if gets > 2:
        why.append('%d gets (Part 2b: two)' % gets)
    reasons['Part 2b SYS_KO'] = '; '.join(why) or None
    # Part 3
    why = []
    if pol != 'Block':
        why.append('policy %s (Part 3: Block)' % pol)
    if c.cap != 1:
        why.append('capacity %s (Part 3: 1)' % c.cap)
    if set(wroles) - {'blocking'}:
        why.append('writes by %s (Part 3: blocking puts only)' % ', '.join(sorted(set(wroles) - {'blocking'})))
    if len(writer_threads) > 2 or any(sum(w.values()) > 2 for w in c.writers.values()):
        why.append('%d writer threads, up to %d writes each (Part 3: two writers, two writes each)'
                   % (len(writer_threads), max([sum(w.values()) for w in c.writers.values()] or [0])))
    if set(rroles) - {'blocking'}:
        why.append('gets by %s (Part 3: blocking gets only)' % ', '.join(sorted(set(rroles) - {'blocking'})))
    if len([t for t, r in c.readers.items() if sum(r.values())]) > 1:
        why.append('%d reader threads (Part 3: one)' % len(c.readers))
    reasons['Part 3 SYS_BUF'] = '; '.join(why) or None
    # Part 5
    why = []
    if pol != 'Block':
        why.append('policy %s (Part 5: Block)' % pol)
    if c.cap != 2:
        why.append('capacity %s (Part 5: 2)' % c.cap)
    if multi:
        why.append('an ALT with %d guards (Part 5: one guard)' % max(len(a.guards) for a in multi))
    name5 = None
    if len(alt_r) == 1 and not alt_w and wroles['isr'] and not wroles['blocking'] and not wroles['offer'] and not rroles['blocking']:
        name5 = 'Part 5 SYS5R'
    elif len(alt_w) == 1 and not alt_r and len([t for t, r in c.readers.items() if r['blocking']]) == 1 \
            and not wroles['isr'] and not wroles['blocking']:
        name5 = 'Part 5 SYS5W'
    elif len(alt_r) == 1 and len(alt_w) == 1 and not wroles['isr'] and not wroles['blocking'] and not rroles['blocking']:
        name5 = 'Part 5 SYS5RW'
    else:
        why.append('roles: ALT readers %d, ALT writers %d, ISR writes %d, blocking puts %d, blocking gets %d '
                   '(Part 5: ALT reader + ISR, ALT writer + blocking reader, or ALT reader + ALT writer)'
                   % (len(alt_r), len(alt_w), wroles['isr'], wroles['blocking'] + wroles['offer'], rroles['blocking']))
    reasons[name5 or 'Part 5'] = '; '.join(why) or None
    return reasons


BUF_SYS = {'Part 1c SYS_C': ('SYS_C', ['done_alt', 'done_blk', 'wait_flag'], 2),
           'Part 1c SYS_C2': ('SYS_C2', ['done_alt', 'done_blk', 'wait_flag'], 2),
           'Part 2 SYS_KN_NEW': ('SYS_KN_NEW', ['t_new', 'isr_new', 'rd'], 2),
           'Part 2b SYS_KO': ('SYS_KO(<>)', ['kput', 'kdrop', 'rd'], 2),
           'Part 3 SYS_BUF': ('SYS_BUF', ['bin', 'bout', 'acq_space', 'acq_item'], 1),
           'Part 5 SYS5R': ('SYS5R(false)', ['get5', 'isr_put', 'wait_r'], 2),
           'Part 5 SYS5W': ('SYS5W', ['put5', 'get5', 'b_acq', 'wait_w'], 2),
           'Part 5 SYS5RW': ('SYS5RW(false)', ['put5', 'get5', 'wait_r', 'wait_w'], 2)}


def buf_trace(c, cfg, alts_on, id_shift=None):
    """The channel's events in the configuration's alphabet. id_shift: (get
    number, delta) adds delta to that get's identity (mutation get-id)."""
    sysx, sigma, cap = BUF_SYS[cfg]
    k = cap + 2
    out, gets, first2 = [], 0, 0           # out: (row index, event)
    if cfg.startswith('Part 1c'):
        a = [x for x in alts_on if any(kd == 'input' and t == c.name for kd, t in x.guards.values())][0]
        for s in a.selects:
            out += [(w['resume'][2].i, 'wait_flag') for w in s.waits if w['resume']]
            if s.end:
                out.append((s.end.i, 'done_alt'))
        out += [(r.i, 'wait_flag') for r, _ in a.strays]
        out += [(it[1].i, 'done_blk') for it in c.items if it[0] == 'get' and it[2]['role'] == 'blocking']
        return sorted(out)
    for kind, row, x in c.items:
        if kind == 'acq_space' and 'acq_space' in sigma:
            out.append((row.i, 'acq_space'))
        elif kind == 'acq_item' and ('acq_item' in sigma or 'b_acq' in sigma):
            out.append((row.i, 'acq_item' if 'acq_item' in sigma else 'b_acq'))
        elif kind == 'write':
            n = x['n'] % k
            isr = x['role'] == 'isr'
            if cfg == 'Part 2 SYS_KN_NEW':
                if first2 < 2:                 # the initial state <0, 1>
                    first2 += 1
                    continue
                e = 'isr_new' if isr else 't_new'
            elif cfg == 'Part 2b SYS_KO':
                e = '%s.%d.%d' % ('kdrop' if row.event == 'DroppedNew' else 'kput', 1 if isr else 0, n)
            elif cfg == 'Part 3 SYS_BUF':
                e = 'bin.%d' % n
            else:
                e = ('isr_put.%d' if isr else 'put5.%d') % n
            out.append((row.i, e))
        elif kind == 'get':
            ident = x['id']
            if id_shift and id_shift[0] == gets:
                ident = (ident or 0) + id_shift[1]
            gets += 1
            t = (ident % k) if ident is not None else 'UNKNOWN'
            out.append((row.i, {'Part 2 SYS_KN_NEW': 'rd', 'Part 2b SYS_KO': 'rd', 'Part 3 SYS_BUF': 'bout'}.get(cfg, 'get5') + '.%s' % t))
    if cfg.startswith('Part 5'):           # WaitAlt + Resume of the single-guard ALT reader / writer
        for a in alts_on:
            kd = [k2 for k2, t in a.guards.values() if t == c.name][0]
            for s in a.selects + ([a.cur] if a.cur else []):
                out += [(w['resume'][2].i, 'wait_r' if kd == 'input' else 'wait_w') for w in s.waits if w['resume']]
            out += [(r.i, 'wait_r' if kd == 'input' else 'wait_w') for r, _ in a.strays]
    return sorted(out)


def buf_check(c, cfg, trace, name):
    sysx, sigma, cap = BUF_SYS[cfg]
    body = ['OBS_P = {| %s |}' % ', '.join(sigma),
            'M_P = %s \\ diff(Events, OBS_P)' % sysx,
            'REC_P = %s' % chain(trace)]
    return {'model': BUF, 'config': '%s (%s)' % (cfg, sysx), 'decls': body, 'assert': 'M_P [T= REC_P',
            'length': len(trace), 'name': name}


# --------------------------------------------- rendezvous models: matching
def owr_match(inst, tr):
    """Why an Alternative does not match alt_one_winner / alt_owrv_extended
    (None if it matches)."""
    gl = inst.glist()
    rv_in = [t for k, t in gl if k == 'input' and t not in tr.bufs]
    rv_out = [t for k, t in gl if k == 'output' and t not in tr.bufs]
    buf_in = [t for k, t in gl if k == 'input' and t in tr.bufs]
    n = len(inst.selects)
    res = {}
    why = []
    if len(gl) != 2 or len(rv_in) != 2:
        why.append('guards %s (the model: two rendezvous input guards)' % guard_text(gl, tr))
    if n != 2:
        why.append('%d select()s (the model: two)' % n)
    for c in rv_in:
        m = len(tr.rvs[c].done) if c in tr.rvs else 0
        if m != 1:
            why.append('%s carries %d messages (the model: one per channel)' % (short(c), m))
    res['alt_one_winner SYS'] = '; '.join(why) or None
    why = []
    if not (len(gl) == 2 and len(rv_in) == 1 and len(buf_in) == 1):
        why.append('guards %s (Part B: one rendezvous and one buffered input guard)' % guard_text(gl, tr))
    else:
        b = tr.bufs[buf_in[0]]
        if b.ev['PutFromISR'] != 1 or b.ev['BufferedPut']:
            why.append('%s: %d PutFromISR, %d task writes (Part B: one PutFromISR)' % (short(b.name), b.ev['PutFromISR'], b.ev['BufferedPut']))
    if n != 2:
        why.append('%d select()s (Part B: two)' % n)
    res['alt_owrv_extended Part B'] = '; '.join(why) or None
    why = []
    if not (len(gl) == 2 and len(rv_out) == 2):
        why.append('guards %s (Part C: two rendezvous output guards)' % guard_text(gl, tr))
    if n != 2:
        why.append('%d select()s (Part C: two)' % n)
    res['alt_owrv_extended Part C'] = '; '.join(why) or None
    why = []
    if not (len(gl) == 2 and len(rv_in) == 1 and len(rv_out) == 1):
        why.append('guards %s (Part A: one rendezvous input and one rendezvous output guard)' % guard_text(gl, tr))
    else:
        why.append('Part A needs a partner Alternative with the crossed guards; matched by hand only')
    if n != 1:
        why.append('%d select()s (Part A: one)' % n)
    res['alt_owrv_extended Part A'] = '; '.join(why) or None
    return res


def guard_text(gl, tr):
    out = []
    for k, t in gl:
        if k in ('input', 'output'):
            kind = 'buffered' if t in tr.bufs else 'rendezvous' if t in tr.rvs else 'channel'
            out.append('%s%s %s' % (kind, '?' if k == 'input' else '!', short(t)))
        else:
            out.append('%s %s' % (k, t))
    return '[' + ', '.join(out) + ']'


# -------------------------------------------------------- stable failures
def read_stable(path):
    out = []
    for r in csv.DictReader(open(path, newline='')):
        offers = collections.defaultdict(list)
        for o in r['offers'].split('; '):
            who, _, what = o.partition(': ')
            offers[who].append(what)
        out.append({'start': float(r['start_us']) / 1e6, 'end': float(r['end_us']) / 1e6,
                    'classification': r['classification'], 'offers': dict(offers)})
    return out


def atd_failures(tr, inst, gl, trace_full, has_t, point, drop_timeout=False):
    """A stable-failure check at point for an ATD projection, or (None, why)."""
    name = tr.names.get(inst.thread)
    offers = point['offers'].get(name) if name else None
    if offers is None:
        return None, 'its thread does not appear in the stable point'
    sel = atd_select(inst)
    open_w = [w for w in sel.waits if w['wait'].t <= point['start'] and
              (w['resume'] is None or w['resume'][2].t > point['start'])]
    if not open_w:
        return None, 'its select() is not waiting at the stable point'
    w = open_w[-1]
    awaits_tick = any(o.startswith('timeout ') for o in offers) and not drop_timeout
    # prefix: the trace up to and including this wait's a_block
    k = sel.waits.index(w)
    prefix, seen = [], -1
    for e in trace_full:
        prefix.append(e)
        if e.startswith('a_block.'):
            seen += 1
            if seen == k:
                break
    took = 'a_take' in prefix
    p = 1 if took else 0
    obs = ['a_block', 'a_wake', 'asel', 'tick', 'a_take'] if has_t else ['a_block', 'a_wake', 'asel', 'a_take']
    x = [o for o in obs if not (o == 'tick' and awaits_tick)]
    sysx = 'SYS(OK, <%s>, %d, STALE, 0)' % (', '.join(gl), p)
    obs_m = '{| a_block, a_wake, a_sel, tick, a_take |}' if has_t else '{| a_block, a_wake, a_sel, a_take |}'
    env = envt(prefix, free_tick=not has_t)
    body = ['channel asel : GS',
            'channel mark_t, esc_t, loop_t',
            'OBS_P = %s' % obs_m,
            'OBSR_P = {| %s |}' % ', '.join(['a_block', 'a_wake', 'asel', 'tick', 'a_take']),
            'M_P = (%s \\ diff(Events, OBS_P)) [[ a_sel.g.e.b <- asel.g | g <- GS, e <- TT, b <- Bool ]]' % sysx,
            'X_P = {| %s |}' % ', '.join(x),
            'LOOP_P = loop_t -> LOOP_P',
            'OFFX_P = [] x : X_P @ x -> LOOP_P'] + env + \
           ['F_P = M_P [| OBSR_P |] ENV0']
    return {'model': ATD, 'config': sysx, 'decls': body, 'assert': 'F_P :[deadlock free [F]]',
            'length': len(prefix), 'kind': 'failures', 'X': x, 'prefix': prefix}, None


def envt(prefix, free_tick):
    """ENV0..ENVn: the prefix with an escape at every step, then the marker."""
    out = []
    for i, e in enumerate(prefix):
        alts = ['%s -> ENV%d' % (e, i + 1), 'esc_t -> LOOP_P']
        if free_tick:
            alts.append('tick -> ENV%d' % i)
        out.append('ENV%d = %s' % (i, ' [] '.join(alts)))
    out.append('ENV%d = mark_t -> OFFX_P' % len(prefix))
    return out


# ------------------------------------------------------------------ ProB
def csp_text(model, chk):
    return 'include "%s.csp"\n\n%s\n\nassert %s\n' % (model, '\n'.join(chk['decls']), chk['assert'])


def key_of(chk):
    model_src = open(os.path.join(REPO, 'docs', 'formal', chk['model'] + '.csp')).read()
    return hashlib.sha256((model_src + '\0' + csp_text(chk['model'], chk)).encode()).hexdigest()[:20]


def run_prob(chk, tmp_root=None):
    d = tempfile.mkdtemp(dir=tmp_root)
    try:
        shutil.copy(os.path.join(REPO, 'docs', 'formal', chk['model'] + '.csp'), d)
        open(os.path.join(d, 'check.csp'), 'w').write(csp_text(chk['model'], chk))
        try:
            p = subprocess.run([PROB, 'check.csp', '-csp_assertion', chk['assert'], '-c'], cwd=d,
                               capture_output=True, text=True, timeout=TIMEOUT)
            txt = p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            return {'result': 'TIMEOUT'}
    finally:
        shutil.rmtree(d, ignore_errors=True)
    lines = txt.splitlines()
    if any(l.strip() == 'OK' for l in lines):
        res = 'PASS'
    elif re.search(r'\*\*\* FALSE \*\*\*', txt):
        res = 'FAIL'
    else:
        res = 'ERROR'
    cpu = re.search(r'Runtime for csp_assertion_check: (\d+) ms', txt)
    st = re.search(r'^STATES (\d+)', txt, re.M)
    ce = re.search(r'Result trace for \w+: (.*)', txt)
    out = {'result': res, 'cpu_ms': int(cpu.group(1)) if cpu else None, 'states': int(st.group(1)) if st else None}
    if ce:
        out['counterexample'] = ce.group(1)[:400]
    if res == 'ERROR':
        out['errors'] = [l for l in lines if l.startswith('!')][:6]
    return out


def run_all(checks, jobs, cache):
    todo = {}
    for c in checks:
        c['key'] = key_of(c)
        todo.setdefault(c['key'], c)
    results = {}
    if cache:
        os.makedirs(cache, exist_ok=True)
        for k in list(todo):
            f = os.path.join(cache, k + '.json')
            if os.path.exists(f):
                results[k] = dict(json.load(open(f)), cached=True)
                del todo[k]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for k, r in zip(todo, ex.map(run_prob, todo.values())):
            results[k] = r
            if cache and r.get('result') in ('PASS', 'FAIL'):
                json.dump(r, open(os.path.join(cache, k + '.json'), 'w'))
    for c in checks:
        c['prob'] = results[c['key']]
    return len(todo)


# ------------------------------------------------------------- mutations
MUTATIONS = ('swap', 'drop-rv', 'alt-guard', 'get-id', 'resume')


def verdict(chk):
    """Conformance verdict of a checked assertion: a trace check holds iff
    ProB passes it; a stable-failure check holds iff ProB finds a deadlock
    and the deadlock comes right after the marker. Mutants: 'caught' iff the
    verdict is FAIL."""
    r = chk.get('prob', {})
    if r.get('result') not in ('PASS', 'FAIL'):
        return r.get('result', 'NOT RUN')
    if chk.get('kind') == 'failures':
        ce = r.get('counterexample', '')
        if r['result'] == 'FAIL' and re.search(r'mark_t,\s*DEADLOCKS', ce):
            return 'PASS'
        return 'FAIL' if r['result'] == 'PASS' else 'ERROR (deadlock not after the marker)'
    return r['result']


def mutate_rows(rows, kind, target_rows, trace_rows=()):
    """A copy of rows with one fault in target_rows (row indices of one
    projection); swap: the first two rows of trace_rows (the rows the check's
    events come from) with different events. Returns (rows, text) or (None, why)."""
    rows = [Row(r.i, r.t, r.event, r.thread, dict(r.d)) for r in rows]
    tset = sorted(target_rows)
    if kind == 'swap':
        mapped = [i for i in trace_rows if i is not None]
        for a, b in zip(mapped, mapped[1:]):
            if rows[a].event != rows[b].event:
                rows[a], rows[b] = rows[b], rows[a]
                rows[a].i, rows[b].i = a, b
                return rows, 'swapped rows %d (%s) and %d (%s)' % (a, rows[b].event, b, rows[a].event)
        return None, 'no two different mapped events'
    if kind == 'drop-rv':
        for i in tset:
            if rows[i].event == 'RendezvousDone':
                del rows[i]
                for j, r in enumerate(rows):
                    r.i = j
                return rows, 'dropped RendezvousDone row %d' % i
        return None, 'no RendezvousDone'
    if kind == 'alt-guard':
        for i in tset:
            if rows[i].event == 'AltSelected':
                n = sum(1 for j in tset if rows[j].event == 'AltGuard')
                if n < 2:
                    return None, 'one guard only'
                g = int(rows[i].d['guard'])
                rows[i].d['guard'] = str((g + 1) % n)
                return rows, 'AltSelected row %d: guard %d -> %d' % (i, g, (g + 1) % n)
        return None, 'no AltSelected'
    if kind == 'resume':
        for i in tset:
            if rows[i].event == 'ResumeTimeout' and 'deadline' in rows[i].d:
                dl = int(rows[i].d['deadline'])
                rows[i].event = 'Resume'
                rows[i].d['tick'] = str(dl - 1)
                return rows, 'ResumeTimeout row %d -> Resume at tick %d (deadline %d)' % (i, dl - 1, dl)
        return None, 'no ResumeTimeout'
    return None, 'unknown mutation'


# --------------------------------------------------------------- analysis
def analyse(tr, stable=None, only=None, forced=None, id_shift=None, drop_timeout_at=None):
    """Projections, matching table and checks. only: projection ids to
    generate; forced: {projection id: config} (mutation runs keep the
    original configuration)."""
    projections, checks = [], []
    alts_by_chan = collections.defaultdict(list)
    for a in tr.alts:
        for k, t in a.guards.values():
            if k in ('input', 'output'):
                alts_by_chan[t].append(a)
    points = read_stable(stable) if stable else []
    for a in tr.alts:
        gl, why = atd_match(a)
        rec = {'id': a.id, 'kind': 'Alternative', 'name': short(a.alt), 'thread': tr.names.get(a.thread, a.thread),
               'guards': guard_text(a.glist(), tr), 'selects': len(a.selects), 'events': sum(len(s.rows) for s in a.selects),
               'match': {}}
        sel = atd_select(a)
        if not why and (sel is None or not (sel.waits or sel.selected is not None)):
            why.append('no model events: the select() neither waited nor completed (only AltGuard records)')
        cfg = None if why else 'SYS'
        if forced and a.id in forced:
            cfg = 'SYS'
        rec['match'][ATD] = 'SYS(OK, <%s>, p, STALE, 0)' % ', '.join(gl) if cfg else 'unmatched: ' + '; '.join(why)
        for k, v in owr_match(a, tr).items():
            rec['match'][k] = 'matched (generator not implemented)' if v is None else 'unmatched: ' + v
        if cfg and (only is None or a.id in only):
            sel = atd_select(a)
            pairs, has_t, notes = atd_trace(a, sel, gl)
            trace = [e for _, e in pairs]
            p = atd_partner(gl, sel)
            chk = atd_check(gl, p, has_t, trace, a.id)
            chk['rows'] = [i for i, _ in pairs]
            chk['projection'] = a.id
            chk['notes'] = notes
            checks.append(chk)
            rec['trace_length'] = len(trace)
            for pi, pt in enumerate(points):
                if pt['classification'] == 'stability unconfirmed':
                    continue
                f, fwhy = atd_failures(tr, a, gl, trace, has_t, pt, drop_timeout=(drop_timeout_at == (a.id, pi)))
                if f:
                    f['projection'] = a.id
                    f['point'] = pi
                    f['point_offers'] = pt['offers'].get(tr.names.get(a.thread))
                    checks.append(f)
        projections.append(rec)
    for c in tr.bufs.values():
        if not c.items or not any(it[0] in ('write', 'get') for it in c.items):
            continue
        alts_on = alts_by_chan.get(c.name, [])
        reasons = buf_match(c, alts_on)
        rec = {'id': c.id, 'kind': 'buffered channel', 'name': short(c.name), 'capacity': c.cap, 'policy': policy(c),
               'writes': dict(sum((collections.Counter(w) for w in c.writers.values()), collections.Counter())),
               'gets': dict(sum((collections.Counter(r) for r in c.readers.values()), collections.Counter())),
               'events': len(c.items), 'match': {}}
        for k, v in reasons.items():
            rec['match'][BUF + ' ' + k] = 'matched' if v is None else 'unmatched: ' + v
        matched = [k for k, v in reasons.items() if v is None and k in BUF_SYS]
        if forced and c.id in forced:
            matched = [forced[c.id]]
        if only is None or c.id in only:
            for cfg in matched:
                pairs = buf_trace(c, cfg, alts_on, id_shift=id_shift)
                trace = [e for _, e in pairs]
                chk = buf_check(c, cfg, trace, c.id)
                chk['rows'] = [i for i, _ in pairs]
                chk['projection'] = c.id
                checks.append(chk)
                rec['trace_length'] = len(trace)
        projections.append(rec)
    for c in tr.rvs.values():
        users = [a for a in alts_by_chan.get(c.name, [])]
        rec = {'id': c.id, 'kind': 'rendezvous channel', 'name': short(c.name), 'messages': len(c.done),
               'writers': len(c.writers), 'readers': len(c.readers),
               'alternatives': [a.id for a in users], 'events': len(c.done), 'match': {}}
        why = []
        if not users:
            why.append('no Alternative uses it (the rendezvous models are ALT protocols; plain rendezvous is not modelled)')
        else:
            why.append('checked through its Alternatives (%s)' % ', '.join(a.id for a in users))
        rec['match']['rendezvous models'] = 'unmatched: ' + '; '.join(why)
        projections.append(rec)
    return projections, checks


def projection_rows(tr, pid):
    for a in tr.alts:
        if a.id == pid:
            return [r.i for s in a.selects + ([a.cur] if a.cur else []) for r in s.rows]
    for c in tr.bufs.values():
        if c.id == pid:
            out = [row.i for _, row, _ in c.items]
            for a in tr.alts:
                if any(t == c.name for k, t in a.guards.values()):
                    out += [r.i for s in a.selects + ([a.cur] if a.cur else []) for r in s.rows]
            return sorted(set(out))
    return []


def mutants(tr, base_checks, stable):
    """One mutant per (mutation, model) where applicable, on the first matched
    projection of that model on which the mutation applies."""
    out = []
    by_model = collections.OrderedDict()
    for c in base_checks:
        if c.get('kind') == 'failures':
            continue
        by_model.setdefault(c['config'].split(' (')[0] if c['model'] == BUF else c['model'], []).append(c)
    for kind in MUTATIONS:
        for model, cs in by_model.items():
            for c in cs:
                pid = c['projection']
                if kind == 'get-id':
                    if c['model'] != BUF or not any(e.startswith(('bout.', 'rd.', 'get5.')) for e in rec_events(c).split(' -> ')):
                        continue
                    forced = {pid: c['config'].split(' (')[0]}
                    _, chks = analyse(tr, only={pid}, forced=forced, id_shift=(0, 1))
                    desc = 'first get: identity + 1'
                else:
                    rows, desc = mutate_rows(tr.rows, kind, projection_rows(tr, pid), c.get('rows', ()))
                    if rows is None:
                        continue
                    t2 = Trace(rows)
                    forced = {pid: c['config'].split(' (')[0] if c['model'] == BUF else 'SYS'}
                    _, chks = analyse(t2, only={pid}, forced=forced)
                chks = [x for x in chks if x.get('kind') != 'failures' and x['model'] == c['model']]
                if not chks or rec_events(chks[0]) == rec_events(c):
                    continue                              # the fault does not change the projection's trace
                m = chks[0]
                m.update({'mutation': kind, 'mutation_desc': desc, 'mutation_model': model})
                out.append(m)
                break
    return out


def rec_events(chk):
    for d in chk['decls']:
        if d.startswith('REC_P = '):
            return d[len('REC_P = '):]
    return ''


def stable_mutants(tr, stable):
    """Remove the timeout offer at each stable point of a matched ATD projection."""
    out = []
    _, base = analyse(tr, stable=stable)
    for c in base:
        if c.get('kind') == 'failures' and 'tick' not in c['X']:
            _, chks = analyse(tr, stable=stable, only={c['projection']},
                              drop_timeout_at=(c['projection'], c['point']))
            for m in chks:
                if m.get('kind') == 'failures' and m['point'] == c['point']:
                    m.update({'mutation': 'stable-offer', 'mutation_desc': 'timeout offer removed at the stable point',
                              'mutation_model': ATD})
                    out.append(m)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('events')
    ap.add_argument('--out', required=True)
    ap.add_argument('--stable')
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--cache')
    ap.add_argument('--mutate', action='store_true', help='also generate (and with --run check) the mutants')
    a = ap.parse_args()
    tr = Trace(read_rows(a.events))
    projections, checks = analyse(tr, stable=a.stable)
    if a.mutate:
        checks += mutants(tr, checks, a.stable)
        if a.stable:
            checks += stable_mutants(tr, a.stable)
    os.makedirs(os.path.join(a.out, 'checks'), exist_ok=True)
    for i, c in enumerate(checks):
        c['file'] = 'checks/%03d_%s_%s.csp' % (i, c['projection'], c.get('mutation') or c.get('kind', 'trace'))
        open(os.path.join(a.out, c['file']), 'w').write(csp_text(c['model'], c))
    ran = run_all(checks, a.jobs, a.cache) if a.run else 0
    for c in checks:
        c['verdict'] = verdict(c) if a.run else 'NOT RUN'
        if c.get('mutation'):
            c['caught'] = c['verdict'] == 'FAIL'
    with open(os.path.join(a.out, 'match.txt'), 'w') as f:
        for p in projections:
            f.write('%s %s %s %s\n' % (p['id'], p['kind'], p['name'], {k: v for k, v in p.items()
                                                                        if k not in ('id', 'kind', 'name', 'match')}))
            for k, v in p['match'].items():
                f.write('    %-36s %s\n' % (k, v))
    json.dump({'events_file': os.path.basename(a.events), 'projections': projections,
               'checks': [{k: v for k, v in c.items() if k != 'decls'} for c in checks], 'prob_runs': ran},
              open(os.path.join(a.out, 'report.json'), 'w'), indent=1)
    n = collections.Counter((c.get('mutation') or c.get('kind', 'trace'), c['verdict']) for c in checks)
    print('%s: %d projections, %d checks (%s), %d ProB runs' % (os.path.basename(a.events), len(projections), len(checks),
                                                                 dict(n), ran))


if __name__ == '__main__':
    main()
