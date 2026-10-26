#!/usr/bin/env python3
"""Conformance of a recorded CSP4CMSIS trace against docs/formal/*.csp.

    csp_trace.py EVENTS.csv[.xz] --out DIR [--stable STABLE.csv] [--mutate] [--models DIR]
    csp_trace.py --prove DIR [DIR ...] [--jobs N] [--cache DIR] [--models DIR] [--prob-mem GB]

EVENTS.csv: tools/trace/decode.py -o; STABLE.csv: decode.py --stable. The
first form writes DIR/report.json (projections, matching, checks, coverage)
and DIR/match.txt; --prove checks the assertions of one or more such
directories with probcli (PROBCLI=), batching the trace checks of each
model configuration into one assertion (the recorded traces as one external
choice), and writes the verdicts back.

1. Projections: one Alternative object (an AltGuard with index 0 starts a
   new one), one buffered channel, one rendezvous channel, with the events
   of the threads that use it (mappings/<model>.json). AltGuard and
   AltSelected name their Alternative and thread; buffered events their
   channel's policy.
2. Instance matching against each model's configurations; a projection that
   matches none is listed with the reason. An Alternative with several
   select()s is checked per select() (the cut: each select() from the
   model's initial state, see trace/README.md).
3. Trace checks: MODEL \\ H [T= RECORDED.
4. Stable failures (--stable): at a stable point t of a checked projection,
   X = its observable events except the environment events the point
   awaits; MODEL \\ H composed with an environment that performs t, then a
   marker, then offers only X (escapes before the marker) must deadlock
   right after the marker.
5. Mutations (--mutate): one fault per model on copies of the decoded rows
   (swap, drop-rv, alt-guard, get-id, resume), and per stable point one
   offer removed. Each must fail.
6. Coverage: the share of the recorded CSP events that a checked
   projection's trace accounts for, per event kind.
No channel element values are involved: the trace has none.
"""
import argparse, bisect, collections, concurrent.futures, csv, glob, hashlib, json, lzma, os, re, resource, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROB = os.environ.get('PROBCLI', 'probcli')
TIMEOUT = 7200
MEM_GB = None                       # probcli's memory limit (GB), set by main(): --prob-mem
CGROUP = None                       # systemd-run --scope usable? (checked once, when a limit is set)
MEMERR = r'Cannot allocate memory|insufficient memory|[Rr]esource error[^\n]*memory|[Oo]ut of (global )?memory|memory exhausted'
CHUNK = 250                         # events per CSPm definition of a recorded trace

ATD, BUF, OWR, RVP = 'alt_timeout_deadline', 'buffered_channel_v2', 'alt_one_winner', 'rendezvous_plain'
BUF_EVENTS = ('BufferedPut', 'PutFromISR', 'BufferedGet', 'DroppedNew', 'OverwroteOldest')
DMAX, CMAX, KMAX, MAXR = 15, 3, 6, 6
# buffered capacities up to KMAX - 2 = 4 (identities mod capacity + 2 < KMAX): the largest capacity whose
# range assertions were checked in ProB (capacity 6 does not finish); a larger channel is not matched
CAP_WHY = 'capacity %d (the parameterised models are checked up to capacity %d)'


# ------------------------------------------------------------------ parsing
def split_value(v):
    """'k=v, k=v' at bracket depth 0 (names contain ', ' inside () and <>)."""
    out, depth, cur, i = [], 0, '', 0
    while i < len(v):
        c = v[i]
        if c in '(<[':
            depth += 1
        elif c in ')>]':
            depth -= 1
        if depth == 0 and v.startswith(', ', i):
            out.append(cur)
            cur, i = '', i + 2
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
    __slots__ = ('i', 't', 'event', 'thread', 'd')

    def __init__(self, i, t, event, thread, d):
        self.i, self.t, self.event, self.thread, self.d = i, t, event, thread, d


def read_rows(path):
    """The CSP4CMSIS rows of a decoded trace."""
    f = lzma.open(path, 'rt') if path.endswith('.xz') else open(path, newline='')
    rows = []
    for r in csv.DictReader(f):
        if r['component'] != 'CSP':
            continue
        d = split_value(r['value'])
        if r['event'] == 'AltSelected' and 'alt' not in d:
            sys.exit('%s: AltSelected without alt=: a trace recorded before the AltSelected/AltGuard/policy '
                     'payloads (csp4cmsis_trace.h); re-record it' % path)
        rows.append(Row(len(rows), float(r['model_time_s']), r['event'], tid(r['thread']) or r['thread'], d))
    return rows


def tick_diff(a, b):
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d & 0x80000000 else d


def short(name):
    return re.sub(r'\(anonymous namespace\)::', '', name or '')


# --------------------------------------------------------------- structures
class Select:
    def __init__(self, start, lo):
        self.start = start            # row index of its first record (None: none before AltSelected)
        self.lo = lo                  # row index before its window (the previous AltSelected, or AltGuard - 1)
        self.waits = []               # {'deadline': (now, dl) | None, 'wait': row, 'resume': (event, tick|None, row) | None}
        self.pending = None           # (now, dl, row) of an AltDeadline not yet followed by WaitAlt
        self.selected = None
        self.end = None               # AltSelected row
        self.take = None              # transfer row of the selected channel guard
        self.rows = []
        self.strays = []              # Resume/ResumeTimeout naming the Alternative with no open wait


class AltInst:
    def __init__(self, n, alt, thread, row):
        self.id = 'alt%d' % n
        self.alt, self.thread, self.first = alt, thread, row.i
        self.guards = {}              # index -> (kind, target)
        self.guard_rows = []
        self.selects = []
        self.open = None
        self.last_end = row.i - 1

    def glist(self):
        return [self.guards[i] for i in sorted(self.guards)]

    def all_selects(self):
        return self.selects + ([self.open] if self.open is not None else [])


class Chan:
    def __init__(self, n, name):
        self.id = 'buf%d' % n
        self.name = name
        self.cap = None
        self.pol = None
        self.items = []               # (kind, row, extra) in record order
        self.ev = collections.Counter()
        self.n = 0                    # writes so far
        self.slot = {}                # slot -> write index
        self.writers = collections.defaultdict(collections.Counter)   # thread -> {role: count}
        self.readers = collections.defaultdict(collections.Counter)


class RvChan:
    def __init__(self, n, name):
        self.id = 'rv%d' % n
        self.name = name
        self.items = []               # ('wait'|'res', row, dir) / ('done', row, by_reader)
        self.writers = collections.Counter()
        self.readers = collections.Counter()


class Trace:
    """Everything the generator derives from the decoded rows."""
    def __init__(self, rows):
        self.rows = rows
        self.alts, self.bufs, self.rvs = [], {}, {}
        self.names = {}               # thread -> [(row index, process name)]: thread ids are reused
        self.build()

    def index(self, chan, kind):
        """(sorted row indices, {row index: thread}) of a channel's items of
        one kind ('write', 'get'; rendezvous: 'wait_write', 'wait_read')."""
        key = (chan, kind)
        if not hasattr(self, '_ix'):
            self._ix = {}
        if key not in self._ix:
            if chan in self.bufs:
                its = [(row.i, row.thread) for k, row, x in self.bufs[chan].items if k == kind]
            else:
                its = [(row.i, row.thread) for k, row, x in self.rvs[chan].items if k == 'wait' and 'wait_' + x == kind]
            self._ix[key] = ([i for i, _ in its], dict(its))
        return self._ix[key]

    def row_at(self, t):
        """The last row recorded at or before time t (0 if none)."""
        if not hasattr(self, '_times'):
            self._times = [r.t for r in self.rows]
        return max(bisect.bisect_right(self._times, t) - 1, 0)

    def name_at(self, thread, i):
        """The process name of a thread at row i (its latest ProcessName)."""
        cur = None
        for j, n in self.names.get(thread, ()):
            if j > i:
                break
            cur = n
        return cur

    def chan_of(self, name):
        if name not in self.bufs:
            self.bufs[name] = Chan(len(self.bufs), name)
        return self.bufs[name]

    def rv_of(self, name):
        if name not in self.rvs:
            self.rvs[name] = RvChan(len(self.rvs), name)
        return self.rvs[name]

    def build(self):
        cur = {}                      # Alternative name -> its current object
        last_ev = {}                  # thread -> (event, object) of its last CSP record
        last_wc = {}                  # thread -> (channel, dir) of its last WaitChannel
        xfer = collections.defaultdict(list)     # thread -> [(row, channel, 'read'|'write')] (non-blocking transfers)
        alt_chans = collections.defaultdict(set)  # thread -> {(channel, kind)} of its Alternatives' guards
        last_start = None
        for row in self.rows:
            ev, th, d = row.event, row.thread, row.d
            if ev == 'ProcessStart':
                last_start = th
            elif ev == 'ProcessName' and last_start:
                self.names.setdefault(last_start, []).append((row.i, d.get('name', '')))
            elif ev == 'AltGuard':
                a, idx = d['alt'], int(d['guard'])
                if idx == 0 or a not in cur:
                    inst = AltInst(len(self.alts), a, th, row)
                    self.alts.append(inst)
                    cur[a] = inst
                    inst.open = Select(row.i, row.i - 1)
                inst = cur[a]
                target = d.get('channel') or d.get('ticks') or d.get('target')
                inst.guards[idx] = (d['kind'], int(target) if d['kind'] == 'timeout' else target)
                inst.guard_rows.append(row)
                if d['kind'] in ('input', 'output'):
                    alt_chans[th].add((target, d['kind']))
            elif ev in ('AltDeadline', 'WaitAlt') and d.get('alt') in cur:
                inst = cur[d['alt']]
                if inst.open is None:
                    inst.open = Select(row.i, inst.last_end)
                s = inst.open
                s.rows.append(row)
                if ev == 'AltDeadline':
                    s.pending = (int(d['now']), int(d['deadline']), row)
                else:
                    s.waits.append({'deadline': s.pending, 'wait': row, 'resume': None, 'mask': int(d['enabled'], 16)})
                    s.pending = None
            elif ev in ('Resume', 'ResumeTimeout') and d.get('object') in cur and cur[d['object']].thread == th:
                inst = cur[d['object']]
                s = inst.open
                if s is not None and s.waits and s.waits[-1]['resume'] is None:
                    s.rows.append(row)
                    s.waits[-1]['resume'] = (ev, int(d['tick']) if 'tick' in d else None, row)
                else:                 # kept in the trace at its position (a fault the model must refuse)
                    if inst.open is None:
                        inst.open = Select(None, inst.last_end)
                    inst.open.strays.append((row, ev))
            elif ev == 'AltSelected' and d.get('alt') in cur:
                inst = cur[d['alt']]
                if inst.open is None:
                    inst.open = Select(None, inst.last_end)
                s = inst.open
                s.rows.append(row)
                s.selected, s.end = int(d['guard']), row
                g = inst.guards.get(s.selected)
                if g and g[0] in ('input', 'output'):
                    role = 'read' if g[0] == 'input' else 'write'
                    cands = [r for (r, c, ro) in xfer[th] if c == g[1] and ro == role and r.i > s.lo]
                    s.take = cands[-1] if cands else None
                    if s.take is not None:
                        s.rows.append(s.take)
                        s.rows.sort(key=lambda r: r.i)
                inst.selects.append(s)
                inst.open = None
                inst.last_end = row.i
            if ev in BUF_EVENTS:
                self.buffered(row, last_ev, xfer, alt_chans)
            elif ev in ('WaitBufferedEmpty', 'WaitBufferedFull'):
                self.chan_of(d['channel']).items.append(('wait_' + ('empty' if ev == 'WaitBufferedEmpty' else 'full'), row, None))
            elif ev in ('Resume', 'ResumeTimeout') and last_ev.get(th, (None, None))[0] in ('WaitBufferedEmpty', 'WaitBufferedFull') \
                    and last_ev[th][1] == d.get('object'):
                self.chan_of(d['object']).items.append(('acq_item' if last_ev[th][0] == 'WaitBufferedEmpty' else 'acq_space', row, None))
            elif ev == 'WaitChannel':
                c, dr = d['channel'], d.get('dir', 'read')
                self.rv_of(c).items.append(('wait', row, dr))
                (self.rvs[c].writers if dr == 'write' else self.rvs[c].readers)[th] += 0
                last_wc[th] = (c, dr)
            elif ev in ('Resume', 'ResumeTimeout') and last_ev.get(th, (None, None))[0] == 'WaitChannel' and th in last_wc:
                c, dr = last_wc[th]
                self.rv_of(c).items.append(('res', row, dr))
            elif ev == 'RendezvousDone':
                c = d['channel']
                w, r = tid(d['writer']), tid(d['reader'])
                rv = self.rv_of(c)
                rv.items.append(('done', row, d.get('by_reader') == '1'))
                rv.writers[w] += 1
                rv.readers[r] += 1
                xfer[w].append((row, c, 'write'))
                xfer[r].append((row, c, 'read'))
            if th and ev != 'ProcessName':
                last_ev[th] = (ev, d.get('object') or d.get('channel') or d.get('alt'))

    def buffered(self, row, last_ev, xfer, alt_chans):
        ev, th, d = row.event, row.thread, row.d
        c = self.chan_of(d['channel'])
        c.cap = int(d['capacity'])
        c.pol = c.pol or d.get('policy') or 'unknown'
        c.ev[ev] += 1
        prev = last_ev.get(th, (None, None))
        blocking = prev[0] in ('Resume', 'ResumeTimeout') and prev[1] == c.name
        if ev in ('BufferedPut', 'PutFromISR', 'OverwroteOldest', 'DroppedNew'):
            n = c.n
            c.n += 1
            slot = int(d['written']) if ev == 'OverwroteOldest' else (int(d['slot']) if d.get('slot', '').isdigit() else None)
            if ev != 'DroppedNew' and slot is not None:
                c.slot[slot] = n
            if ev == 'PutFromISR' or th.startswith('irq'):
                role = 'isr'
            elif blocking:
                role = 'blocking'
            elif (c.name, 'output') in alt_chans[th] and ev == 'BufferedPut':
                role = 'alt'
            else:
                role = 'offer'
            c.writers[th][role] += 1
            c.items.append(('write', row, {'n': n, 'role': role}))
            if role in ('alt', 'offer'):
                xfer[th].append((row, c.name, 'write'))
        else:
            ident = c.slot.get(int(d['slot']))
            if blocking:
                role = 'blocking'
            elif (c.name, 'input') in alt_chans[th]:
                role = 'alt'
            else:
                role = 'other'
            c.readers[th][role] += 1
            c.items.append(('get', row, {'id': ident, 'role': role}))
            if role != 'blocking':
                xfer[th].append((row, c.name, 'read'))


# ----------------------------------------------------------- the checks
class Models:
    """The model files a run is checked against (docs/formal, or drafts)."""
    def __init__(self, path):
        self.dir = path
        self.text = {}

    def src(self, model):
        if model not in self.text:
            p = os.path.join(self.dir, model + '.csp')
            self.text[model] = open(p).read() if os.path.exists(p) else ''
        return self.text[model]

    def has(self, model, name):
        return re.search(r'^%s\(' % re.escape(name), self.src(model), re.M) is not None


def check(model, config, decls, assertion, pairs, kind='trace', **kw):
    """A check: CSPm declarations (they define M_P), the recorded trace as
    (row index | None, event) pairs, and the assertion (REC_P stands for the
    recorded trace)."""
    c = {'model': model, 'config': config, 'decls': decls, 'assert': assertion, 'kind': kind,
         'rec': [e for _, e in pairs], 'rows': [i for i, _ in pairs if i is not None], 'length': len(pairs)}
    c.update(kw)
    return c


def rec_defs(name, events):
    """name = e1 -> ... -> STOP, in chunks (long traces)."""
    if not events:
        return ['%s = STOP' % name]
    out, parts = [], [events[i:i + CHUNK] for i in range(0, len(events), CHUNK)]
    for k, part in enumerate(parts):
        nxt = '%s_%d' % (name, k + 1) if k + 1 < len(parts) else 'STOP'
        out.append('%s = %s -> %s' % (name if k == 0 else '%s_%d' % (name, k), ' -> '.join(part), nxt))
    return out


def csp_text(chk, recs=None):
    """The check file; recs: several recorded traces (a batch) for REC_P."""
    lines = ['include "%s.csp"' % chk['model'], ''] + chk['decls']
    if chk['kind'] == 'trace':
        if recs is None:
            recs = [chk['rec']]
        names = []
        for k, r in enumerate(recs):
            names.append('REC_%d' % k)
            lines += rec_defs('REC_%d' % k, r)
        lines.append('REC_P = ' + ' [] '.join(names))
    lines += ['', 'assert ' + chk['assert'], '']
    return '\n'.join(lines)


def group_key(chk):
    return hashlib.sha256(('\0'.join([chk['model']] + chk['decls'] + [chk['assert']])).encode()).hexdigest()[:16]


# ------------------------------------------------- alt_timeout_deadline
def atd_config(inst, models):
    """(gl, sys expression maker, why-not) of an Alternative for the timeout model."""
    gl, why, k = [], [], 0
    for kind, target in inst.glist():
        if kind in ('input', 'output'):
            gl.append('GC' if k == 0 else 'GC+%d' % k)
            k += 1
        elif kind == 'timeout':
            if target > DMAX:
                why.append('timeout %d ticks (the model\'s delays are 0..%d)' % (target, DMAX))
            gl.append(str(target))
        else:
            why.append('a guard of kind %s' % kind)
    if k > CMAX:
        why.append('%d channel guards (the model has up to %d)' % (k, CMAX))
    if len(inst.guards) != max(inst.guards, default=-1) + 1:
        why.append('guard list incomplete')
    delays = [int(g) for g in gl if not g.startswith('GC')]
    md = 8 if max(delays, default=0) <= 3 else (16 if max(delays) <= 11 else 32)
    if (md > 8 or k > 1) and not models.has(ATD, 'SYSN'):
        why.append('needs the parameterised model (SYSN)')
    return gl, md, why


def atd_sys(gl, md, p):
    if md == 8 and sum(1 for g in gl if g.startswith('GC')) <= 1:
        return 'SYS(OK, <%s>, %s, STALE, 0)' % (', '.join(gl), p)
    return 'SYSN(%d, OK, <%s>, %s, STALE, 0)' % (md, ', '.join(gl), p)


def atd_trace(inst, sel, gl, indexed=True):
    """The select's events (alphabet of the check: asel = renamed a_sel)."""
    delays = [int(g) for g in gl if not g.startswith('GC')]
    has_t = bool(delays)
    out, prev, notes, aux = [], None, [], []
    for w in sel.waits:
        if w['deadline'] is not None:
            now, dl, drow = w['deadline']
            aux.append(drow.i)
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
        if sel.selected >= len(gl):
            notes.append('AltSelected guard %d beyond the %d guards' % (sel.selected, len(gl)))
        else:
            g = gl[sel.selected]
            if g.startswith('GC') and sel.take is not None:
                out.append((sel.take.i, 'a_take.%d' % (int(g[3:]) if '+' in g else 0) if indexed else 'a_take'))
            out.append((sel.end.i, 'asel.%s' % g))
    for row, ev in sel.strays:
        notes.append('%s row %d without an open wait' % (ev, row.i))
        e = (row.i, 'a_wake.%s' % ('true' if ev == 'ResumeTimeout' else 'false'))
        pos = next((j for j, (ri, _) in enumerate(out) if ri is not None and ri > row.i), len(out))
        out.insert(pos, e)
    if not has_t:
        out = [x for x in out if x[1] != 'tick']
    return out, has_t, notes, aux


def atd_decls(sysx, has_t):
    obs = '{| a_block, a_wake, a_sel, tick, a_take |}' if has_t else '{| a_block, a_wake, a_sel, a_take |}'
    return ['channel asel : GS', 'OBS_P = %s' % obs,
            'M_P = (%s \\ diff(Events, OBS_P)) [[ a_sel.g.e.b <- asel.g | g <- GS, e <- TT, b <- Bool ]]' % sysx]


def partner_offers(tr, inst, sel):
    """The partners' offers per channel guard in the select's window: puts
    (input guard) or gets (output guard) of a buffered channel by others,
    plain operations (WaitChannel of the opposite direction, first of a
    pair) on a rendezvous channel."""
    lo, hi = sel.lo, sel.end.i if sel.end is not None else len(tr.rows)
    out = {}
    for kind, c in inst.glist():
        if kind not in ('input', 'output'):
            continue
        if c in tr.bufs:
            ix = tr.index(c, 'write' if kind == 'input' else 'get')
        elif c in tr.rvs:
            ix = tr.index(c, 'wait_write' if kind == 'input' else 'wait_read')
        else:
            continue
        ps = ix[0][bisect.bisect_right(ix[0], lo):bisect.bisect_right(ix[0], hi)]
        per = collections.Counter(ix[1][p] for p in ps if ix[1][p] != inst.thread)
        n = sum(per.values()) if c in tr.bufs else sum((v + 1) // 2 for v in per.values())   # rendezvous: serial + pair wait
        out[short(c)] = max([0] + list(per.values())) if c in tr.bufs else n
        out[short(c)] = n
    return out


# ----------------------------------------------- alt_one_winner per select
def owr_config(inst, tr):
    gl = inst.glist()
    if not (len(gl) == 2 and all(k == 'input' and t in tr.rvs for k, t in gl)):
        return None, 'guards %s (the model: two rendezvous input guards)' % guard_text(gl, tr)
    for k, t in gl:
        rv = tr.rvs[t]
        ws = [w for w in rv.writers if rv.writers[w] or any(x == 'write' and row.thread == w for kk, row, x in rv.items if kk == 'wait')]
        if len(ws) != 1:
            return None, '%s has %d writers (the model: one per channel)' % (short(t), len(ws))
        if any(a is not inst and any(gt == t for gk, gt in a.guards.values()) for a in tr.alts):
            return None, '%s is used by another Alternative (the model: plain writers)' % short(t)
        if set(rv.readers) - {inst.thread}:
            return None, '%s has another reader' % short(t)
    return 'SYSM(OWRV, PW, PW, true, 1, 1)', None


def owr_trace(inst, sel, tr):
    gl = inst.glist()
    lo, hi = sel.lo, sel.end.i if sel.end is not None else len(tr.rows)
    out, aux = [], []
    for w in sel.waits:
        out.append((w['wait'].i, 'atw'))
        if w['resume'] is not None:
            out.append((w['resume'][2].i, 'awt'))
    if sel.selected is None:
        return out, aux
    took = {}
    for g, (k, c) in enumerate(gl):
        dn = [row for kk, row, x in tr.rvs[c].items if kk == 'done' and lo < row.i <= hi and tid(row.d['reader']) == inst.thread]
        took[g] = dn[-1] if dn else None
        aux += [r.i for r in dn]
    g = sel.selected
    out.append((sel.end.i, 'a_sel.%d.%s.%s' % (g, 'true' if took[g] else 'false', 'true' if took[1 - g] else 'false')))
    if took[g] is not None:
        w = tid(took[g].d['writer'])
        res = [row for kk, row, x in tr.rvs[gl[g][1]].items if kk == 'res' and row.thread == w and row.i > took[g].i]
        if res:
            out.append((res[0].i, 'w_done.%d.true' % g))
    out.sort(key=lambda p: p[0])
    return out, aux


OWR_DECLS = ['channel atw, awt',
             'OBS_P = union({| a_sel, w_done, a_wait, w_towait, w_wait |}, {a_towait.true})',
             'M_P = ((%s \\ diff(Events, OBS_P)) [[ a_wait.g <- awt | g <- I ]]) [[ a_towait.true <- atw ]]']


# ------------------------------------------------------- rendezvous_plain
def rvp_config(rv, tr, models):
    if not models.src(RVP):
        return None, 'model not available'
    users = [a.id for a in tr.alts if any(t == rv.name for k, t in a.guards.values())]
    if users:
        return None, 'used by Alternatives (%s): plain operations only' % ', '.join(users)
    ws = {row.thread for k, row, x in rv.items if k == 'wait' and x == 'write'} | {w for w in rv.writers if rv.writers[w]}
    rs = {row.thread for k, row, x in rv.items if k == 'wait' and x == 'read'} | {r for r in rv.readers if rv.readers[r]}
    if len(ws) != 1 or len(rs) != 1 or ws == rs:
        return None, '%d writer and %d reader threads (the model: one plain writer, one plain reader)' % (len(ws), len(rs))
    return 'SYS(OK, true, true)', None


def rvp_trace(rv):
    out = []
    for k, row, x in rv.items:
        if k == 'wait':
            out.append((row.i, 'wait_w' if x == 'write' else 'wait_r'))
        elif k == 'res':
            out.append((row.i, 'res_w' if x == 'write' else 'res_r'))
        else:
            out.append((row.i, 'done.%s' % ('true' if x else 'false')))
    return out


RVP_DECLS = ['M_P = %s \\ H']


# ------------------------------------------------- buffered: configurations
BUF_FIXED = {'Part 1c SYS_C': ('SYS_C', ['done_alt', 'done_blk', 'wait_flag'], 2),
             'Part 1c SYS_C2': ('SYS_C2', ['done_alt', 'done_blk', 'wait_flag'], 2),
             'Part 3 SYS_BUF': ('SYS_BUF', ['bin', 'bout', 'acq_space', 'acq_item'], 1),
             'Part 5 SYS5R': ('SYS5R(false)', ['get5', 'isr_put', 'wait_r'], 2),
             'Part 5 SYS5W': ('SYS5W', ['put5', 'get5', 'b_acq', 'wait_w'], 2),
             'Part 5 SYS5RW': ('SYS5RW(false)', ['put5', 'get5', 'wait_r', 'wait_w'], 2)}


def buf_roles(c, tr):
    w, r = collections.Counter(), collections.Counter()
    for t, x in c.writers.items():
        for role, n in x.items():
            if n:
                w[role] += n
    for t, x in c.readers.items():
        for role, n in x.items():
            if n:
                r[role] += n
    # ALT ends per thread: a thread's Alternative objects on the channel are
    # used one after the other (one select() at a time), i.e. one ALT end
    by_r, by_w = collections.OrderedDict(), collections.OrderedDict()
    for a in tr.alts:
        for k, t in a.guards.values():
            if t == c.name and k in ('input', 'output'):
                (by_r if k == 'input' else by_w).setdefault(a.thread, []).append(a)
    alt_r = [AltEnd(v) for v in by_r.values()]
    alt_w = [AltEnd(v) for v in by_w.values()]
    wb = len([t for t, x in c.writers.items() if x['blocking']])
    rb = len([t for t, x in c.readers.items() if x['blocking']])
    return w, r, alt_r, alt_w, wb, rb


class AltEnd:
    """The Alternative objects of one thread on one channel."""
    def __init__(self, objs):
        self.objs = objs
        self.thread = objs[0].thread
        self.guards = max((o.guards for o in objs), key=len)
        self.selects = [s for o in objs for s in o.selects]

    def all_selects(self):
        return [s for o in self.objs for s in o.all_selects()]


def buf_match(c, tr, models):
    """{configuration: None (matched) | why not}, configurations in preference order."""
    pol = c.pol
    w, r, alt_r, alt_w, wb, rb = buf_roles(c, tr)
    writes, gets = sum(w.values()), sum(r.values())
    res = collections.OrderedDict()
    multi = [o for a in alt_r + alt_w for o in a.objs if len(o.guards) > 1]
    # Part 1c
    why = []
    if c.ev['OverwroteOldest'] or c.ev['DroppedNew']:
        why.append('overwrites/drops')
    if w['isr']:
        why.append('ISR writes (the model\'s writer is a task)')
    if len(alt_r) != 1:
        why.append('%d ALT readers (the model has one)' % len(alt_r))
    elif any(len(o.guards) != 1 for o in alt_r[0].objs) or len(alt_r[0].selects) != 1:
        why.append('the ALT reader has %d guards and %d select()s (the model: one each)' % (len(alt_r[0].guards), len(alt_r[0].selects)))
    if alt_w:
        why.append('an ALT writer')
    if not why and writes == 1 and r['alt'] == 1 and not r['blocking']:
        res['Part 1c SYS_C'] = None
    elif not why and writes == 2 and len([t for t, x in c.writers.items() if sum(x.values())]) == 1 and r['alt'] == 1 \
            and r['blocking'] == 1 and rb == 1:
        res['Part 1c SYS_C2'] = None
    else:
        res['Part 1c SYS_C'] = '; '.join(why) or '%d writes, %d ALT and %d blocking gets (SYS_C: 1/1/0, SYS_C2: 2/1/1)' % (writes, r['alt'], r['blocking'])
    # Part 3 (fixed) and 3N
    why = []
    if pol != 'Block':
        why.append('policy %s' % pol)
    if set(w) - {'blocking'}:
        why.append('writes by %s (Part 3: blocking puts)' % ', '.join(sorted(set(w) - {'blocking'})))
    if set(r) - {'blocking'}:
        why.append('gets by %s (Part 3: blocking gets)' % ', '.join(sorted(set(r) - {'blocking'})))
    if rb > 1:
        why.append('%d reader threads (Part 3: one)' % rb)
    if not why and c.cap == 1 and wb <= 2 and all(sum(x.values()) <= 2 for x in c.writers.values()):
        res['Part 3 SYS_BUF'] = None
    elif not why and c.cap <= KMAX - 2 and models.has(BUF, 'SYS_BUFN'):
        res['Part 3N SYS_BUFN(%d, %d)' % (c.cap, wb)] = None
    else:
        res['Part 3'] = '; '.join(why) or CAP_WHY % (c.cap, KMAX - 2)
    # Part 5 (fixed) and 5G
    why = []
    if pol != 'Block':
        why.append('policy %s' % pol)
    if multi:
        why.append('an ALT with %d guards (Part 5: one guard)' % max(len(o.guards) for o in multi))
    if len(alt_r) > 1 or len(alt_w) > 1:
        why.append('%d ALT readers, %d ALT writers (Part 5: one each)' % (len(alt_r), len(alt_w)))
    if wb > 2 or rb > 2:
        why.append('%d blocking writer and %d blocking reader threads (Part 5G: up to 2 each)' % (wb, rb))
    if w['offer']:
        why.append('offers')
    ra, wa, wi = len(alt_r) == 1, len(alt_w) == 1, bool(w['isr'])
    if not why and c.cap == 2 and ra and not wa and wi and not wb and not rb:
        res['Part 5 SYS5R'] = None
    elif not why and c.cap == 2 and wa and not ra and not wi and not wb and rb == 1:
        res['Part 5 SYS5W'] = None
    elif not why and c.cap == 2 and ra and wa and not wi and not wb and not rb:
        res['Part 5 SYS5RW'] = None
    elif not why and c.cap <= KMAX - 2 and (ra or wa or wi) and models.has(BUF, 'SYS5G'):
        res['Part 5G SYS5G(%d, %s, %s, %d, %d, %s, false)' % (c.cap, str(ra).lower(), str(wa).lower(), wb, rb,
                                                             str(wi).lower())] = None
    elif not why and c.cap > KMAX - 2 and (ra or wa or wi):
        res['Part 5'] = CAP_WHY % (c.cap, KMAX - 2)
    else:
        res['Part 5'] = '; '.join(why) or 'roles (Part 5G: ALT reader, ALT writer, ISR, or blocking ends with one of them)'
    # Parts 2N / 2bN
    for part, want, name in (('Part 2N', 'KeepNewest', 'SYS_KNE'), ('Part 2bN', 'KeepOldest', 'SYS_KOE')):
        if pol != want:
            res[part] = 'policy %s (%s: %s)' % (pol, part, want)
        elif c.cap > KMAX - 2:
            res[part] = CAP_WHY % (c.cap, KMAX - 2)
        elif not models.has(BUF, name):
            res[part] = 'needs the parameterised model (%s)' % name
        else:
            res['%s %s(%d, -1, -1)' % (part, name, c.cap)] = None
    return res


def buf_cfg(cfg):
    """(system, sigma, capacity) of a matched buffered configuration."""
    if cfg in BUF_FIXED:
        return BUF_FIXED[cfg]
    sysx = re.search(r'(SYS\w*\(.*\))$', cfg).group(1)
    cap = int(re.search(r'\((\d+)', sysx).group(1))
    if cfg.startswith('Part 3N'):
        return sysx, ['bin', 'bout', 'acq_space', 'acq_item'], cap
    if cfg.startswith('Part 5G'):
        return sysx, ['put5', 'get5', 'isr_put', 'b_acq', 'b_wacq', 'wait_r', 'wait_w'], cap
    if cfg.startswith('Part 2N'):
        return sysx, ['t_new', 'isr_new', 'rd'], cap
    return sysx, ['kput', 'kdrop', 'rd'], cap          # Part 2bN


def buf_trace(c, cfg, tr, id_shift=None):
    sysx, sigma, cap = buf_cfg(cfg)
    k = cap + 2
    out, gets, aux = [], 0, []
    w, r, alt_r, alt_w, wb, rb = buf_roles(c, tr)
    if cfg.startswith('Part 1c'):
        a = alt_r[0]
        for s in a.all_selects():
            out += [(x['resume'][2].i, 'wait_flag') for x in s.waits if x['resume']]
            out += [(row.i, 'wait_flag') for row, _ in s.strays]
            if s.end:
                out.append((s.end.i, 'done_alt'))
        out += [(it[1].i, 'done_blk') for it in c.items if it[0] == 'get' and it[2]['role'] == 'blocking']
        return sorted(out), aux
    for kind, row, x in c.items:
        if kind.startswith('wait_'):
            aux.append(row.i)
        elif kind == 'acq_space':
            if 'acq_space' in sigma:
                out.append((row.i, 'acq_space'))
            elif 'b_wacq' in sigma:
                out.append((row.i, 'b_wacq'))
        elif kind == 'acq_item':
            if 'acq_item' in sigma:
                out.append((row.i, 'acq_item'))
            elif 'b_acq' in sigma:
                out.append((row.i, 'b_acq'))
        elif kind == 'write':
            n = x['n'] % k
            isr = x['role'] == 'isr'
            if 't_new' in sigma:
                e = 'isr_new' if isr else 't_new'
            elif 'kput' in sigma:
                e = '%s.%d.%d' % ('kdrop' if row.event == 'DroppedNew' else 'kput', 1 if isr else 0, n)
            elif 'bin' in sigma:
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
            out.append((row.i, ('rd' if 'rd' in sigma else 'bout' if 'bout' in sigma else 'get5') + '.%s' % t))
    if 'wait_r' in sigma:
        for a, ev in [(a, 'wait_r') for a in alt_r] + [(a, 'wait_w') for a in alt_w]:
            for s in a.all_selects():
                out += [(x['resume'][2].i, ev) for x in s.waits if x['resume']]
                out += [(row.i, ev) for row, _ in s.strays]
                aux += [x['wait'].i for x in s.waits]
    return sorted(out), aux


def buf_decls(cfg):
    sysx, sigma, cap = buf_cfg(cfg)
    return ['OBS_P = {| %s |}' % ', '.join(sigma), 'M_P = %s \\ diff(Events, OBS_P)' % sysx]


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
        offers = []
        for o in r['offers'].split('; '):
            who, _, what = o.partition(': ')
            offers.append((who, what))
        end, _, note = r['end_us'].partition(' ')     # "T (end of log)": the point lasts to the end
        out.append({'start': float(r['start_us']) / 1e6, 'end': float(end) / 1e6, 'open_at_end': bool(note),
                    'classification': r['classification'], 'reason': r['reason'], 'offers': offers})
    return out


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


def failure_check(model, config, decls, obsr, x, prefix, free_tick, **kw):
    body = decls + ['channel mark_t, esc_t, loop_t',
                    'OBSR_P = {| %s |}' % ', '.join(obsr),
                    'X_P = {| %s |}' % ', '.join(x),
                    'LOOP_P = loop_t -> LOOP_P',
                    'OFFX_P = [] x : X_P @ x -> LOOP_P'] + envt(prefix, free_tick) + ['F_P = M_P [| OBSR_P |] ENV0']
    return check(model, config, body, 'F_P :[deadlock free [F]]', [(None, e) for e in prefix], kind='failures',
                 X=x, **kw)


def offers_of(point, name):
    return [what for who, what in point['offers'] if who == name]


def chan_in(text, name):
    s = short(name)
    m = re.match(r'(.*) [?!]$', text)
    return bool(m) and (s.endswith(m.group(1)) or m.group(1).endswith(s.split('::')[-1]))


def stable_checks(tr, point, pi, cand, drop=None):
    """Stable-failure checks of the checked projections at one point. cand:
    (kind, object, config, pairs...) of the checked projections. drop: an
    (owner, offer) removed from the point (mutation)."""
    pt = dict(point)
    pt['offers'] = [o for o in point['offers'] if o != drop]
    out = []
    for c in cand:
        f = None
        if c['what'] == 'atd':
            f = atd_failures(tr, c, pt)
        elif c['what'] == 'buf':
            f = buf_failures(tr, c, pt, point)
        elif c['what'] == 'rvp':
            f = rvp_failures(tr, c, pt)
        if f is not None:
            f.update({'projection': c['projection'], 'point': pi, 'point_offers': [list(o) for o in pt['offers']]})
            out.append(f)
    return out


def atd_failures(tr, c, pt):
    inst, sel, gl, md, pairs, has_t = c['inst'], c['sel'], c['gl'], c['md'], c['pairs'], c['has_t']
    name = tr.name_at(inst.thread, inst.first)
    offs = offers_of(pt, name) if name else []
    if not offs and not any(who == name for who, _ in pt['offers']):
        return None
    open_w = [w for w in sel.waits if w['wait'].t <= pt['start'] and (w['resume'] is None or w['resume'][2].t > pt['start'])]
    if not open_w:
        return None
    k = sel.waits.index(open_w[-1])
    prefix, seen = [], -1
    for _, e in pairs:
        prefix.append(e)
        if e.startswith('a_block.'):
            seen += 1
            if seen == k:
                break
    awaits_tick = any(o.startswith('timeout ') for o in offs)
    p = 1 if any(e.startswith('a_take') for e in prefix) else 0
    sysx = atd_sys(gl, md, p)
    obsr = ['a_block', 'a_wake', 'asel', 'tick', 'a_take']
    x = [o for o in obsr if not (o == 'tick' and (awaits_tick or not has_t))]
    # tick stays visible (a hidden free-running clock diverges): in a select
    # without timeout guards it may occur freely during the prefix and is
    # blocked after the marker, without being part of X
    return failure_check(ATD, sysx, atd_decls(sysx, True), obsr, x, prefix, not has_t)


def buf_failures(tr, c, pt, orig):
    """orig: the point as recorded (which users wait decides whether the
    check applies); pt: the point checked (an offer removed: mutation)."""
    ch, cfg, pairs = c['chan'], c['config'], c['pairs']
    if not cfg.startswith(('Part 5', 'Part 3N', 'Part 2')):
        return None
    threads = {row.thread for k, row, x in ch.items if row.t <= orig['start'] and not row.thread.startswith(('irq', 'pre'))}
    threads |= {a.thread for a in tr.alts if any(t == ch.name for k, t in a.guards.values()) and tr.rows[a.first].t <= orig['start']}
    at = tr.row_at(orig['start'])                 # names at the point (thread ids are reused)
    names = [tr.name_at(t, at) for t in threads]
    if not names or None in names:
        return None
    waiting = {}
    for n in names:
        if not any(chan_in(o, ch.name) for o in offers_of(orig, n)):
            return None                   # a user of the channel is not waiting on it: outside the model
        waiting[n] = offers_of(pt, n)
    if cfg.startswith(('Part 2N', 'Part 2bN')):
        # every task end waits on the channel here, and a task's put to a
        # KeepNewest/KeepOldest channel never waits: the model gets no task
        # writer (tw = 0); the interrupt's writes stay unbounded
        cfg = re.sub(r'\((\d+), -1, -1\)$', r'(\1, 0, -1)', cfg)
    sysx, sigma, cap = buf_cfg(cfg)
    reads = any(o.endswith('?') and chan_in(o, ch.name) for offs in waiting.values() for o in offs)
    # an interrupt-fed channel that a read waits on: the interrupt's puts are
    # awaited from the environment, not refused (Part 5 isr_put, Part 2
    # isr_new, Parts 2b/2bN kput.1 and kdrop.1)
    isr_fed = reads and ch.writers and any(x['isr'] for x in ch.writers.values())
    x = [o for o in sigma if not (isr_fed and o in ('isr_put', 'isr_new'))]
    if isr_fed:
        x = [o + '.0' if o in ('kput', 'kdrop') else o for o in x]
    prefix = [e for i, e in pairs if i is not None and tr.rows[i].t <= pt['start']]
    return failure_check(BUF, cfg, buf_decls(cfg), sigma, x, prefix, False)


def rvp_failures(tr, c, pt):
    rv, pairs = c['rv'], c['pairs']
    threads = {row.thread for k, row, x in rv.items if row.t <= pt['start']}
    at = tr.row_at(pt['start'])
    names = [tr.name_at(t, at) for t in threads]
    if not any(any(chan_in(o, rv.name) for o in offers_of(pt, n)) for n in names if n):
        return None
    sigma = ['wait_w', 'res_w', 'wait_r', 'res_r', 'done']
    prefix = [e for i, e in pairs if tr.rows[i].t <= pt['start']]
    return failure_check(RVP, 'SYS(OK, true, true)', [d % 'SYS(OK, true, true)' for d in RVP_DECLS], sigma, sigma,
                         prefix, False)


# ------------------------------------------------------------- mutations
MUTATIONS = ('swap', 'drop-rv', 'alt-guard', 'get-id', 'resume')


def mutate_rows(rows, kind, target_rows, trace_rows=()):
    """A copy of rows with one fault in target_rows (row indices of one
    projection); swap: the first two rows of trace_rows (the rows the check's
    events come from) with different events. Returns (rows, text) or (None, why)."""
    rows = [Row(r.i, r.t, r.event, r.thread, dict(r.d)) for r in rows]
    tset = sorted(target_rows)
    if kind == 'swap':
        mapped = sorted(set(i for i in trace_rows if i is not None))
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


def verdict(chk):
    """A trace check holds iff ProB passes it; a stable-failure check holds
    iff ProB finds a deadlock right after the marker. Mutants: caught iff FAIL.
    ProB's UNKNOWN (and NOT RUN) stays the verdict: never PASS, never caught."""
    r = chk.get('prob', {})
    if r.get('result') not in ('PASS', 'FAIL'):
        return r.get('result', 'NOT RUN')
    if chk['kind'] == 'failures':
        if r['result'] == 'FAIL' and re.search(r'mark_t,\s*DEADLOCKS', r.get('counterexample', '')):
            return 'PASS'
        return 'FAIL' if r['result'] == 'PASS' else 'ERROR (deadlock not after the marker)'
    return r['result']


# --------------------------------------------------------------- analysis
def analyse(tr, models, stable=None, only=None, forced=None, id_shift=None):
    """Projections (with matching), checks, and the stable-failure
    candidates. only: projection ids to generate; forced: {id: config}."""
    projections, checks, cands = [], [], []
    for a in tr.alts:
        gl, md, why = atd_config(a, models)
        sels = a.selects + ([a.open] if a.open is not None and (a.open.waits or a.open.strays) else [])
        rec = {'id': a.id, 'kind': 'Alternative', 'name': short(a.alt), 'thread': tr.name_at(a.thread, a.first) or a.thread,
               'guards': guard_text(a.glist(), tr), 'selects': len(a.selects), 'match': {}}
        if not sels:
            why = why + ['no model events: the select() neither waited nor completed (only AltGuard records)']
        cut = len(sels) > 1
        owcfg, owwhy = owr_config(a, tr)
        if forced and a.id in forced:
            why = []
        rec['match'][ATD] = ('%s%s' % (atd_sys(gl, md, 'p'), ' per select (%d)' % len(sels) if cut else '')) if not why \
            else 'unmatched: ' + '; '.join(why)
        rec['match'][OWR] = ('%s per select (%d)' % (owcfg, len(sels))) if owcfg else 'unmatched: ' + owwhy
        rec['partner_outside'] = 0
        rec['owr_rounds_outside'] = 0
        if only is not None and a.id not in only:
            projections.append(rec)
            continue
        for si, s in enumerate(sels):
            if not why and (forced is None or forced.get(a.id, ATD) == ATD):
                pairs, has_t, notes, aux = atd_trace(a, s, gl, models.has(ATD, 'SYSN'))
                po = partner_offers(tr, a, s)
                outside = any(v > 1 for v in po.values())
                rec['partner_outside'] += outside
                sysx = atd_sys(gl, md, 1 if any(g.startswith('GC') for g in gl) else 0)
                chk = check(ATD, sysx, atd_decls(sysx, has_t), 'M_P [T= REC_P', pairs, projection=a.id, select=si,
                            notes=notes, partner_outside=outside, aux=aux + [r.i for r in a.guard_rows] + [r.i for r in s.rows])
                checks.append(chk)
                cands.append({'what': 'atd', 'projection': a.id, 'inst': a, 'sel': s, 'gl': gl, 'md': md,
                              'pairs': pairs, 'has_t': has_t})
            if owcfg and models.has(OWR, 'SYSM') and (forced is None or forced.get(a.id) == OWR):
                rounds = len(s.waits) + 1
                if rounds > MAXR - 1:
                    rec['owr_rounds_outside'] += 1
                    continue
                pairs, aux = owr_trace(a, s, tr)
                checks.append(check(OWR, owcfg, [OWR_DECLS[0], OWR_DECLS[1], OWR_DECLS[2] % owcfg], 'M_P [T= REC_P', pairs,
                                    projection=a.id, select=si, aux=aux + [r.i for r in a.guard_rows] + [r.i for r in s.rows]))
        projections.append(rec)
    for c in tr.bufs.values():
        if not any(it[0] in ('write', 'get') for it in c.items):
            continue
        w, r, alt_r, alt_w, wb, rb = buf_roles(c, tr)
        rec = {'id': c.id, 'kind': 'buffered channel', 'name': short(c.name), 'capacity': c.cap, 'policy': c.pol,
               'writes': dict(w), 'gets': dict(r), 'events': len(c.items), 'match': {}}
        res = buf_match(c, tr, models)
        for k, v in res.items():
            rec['match'][BUF + ' ' + k] = 'matched' if v is None else 'unmatched: ' + v
        matched = [k for k, v in res.items() if v is None]
        if forced and c.id in forced:
            matched = [forced[c.id]]
        if only is None or c.id in only:
            for cfg in matched[:1]:               # the first (most specific) matching configuration
                pairs, aux = buf_trace(c, cfg, tr, id_shift=id_shift)
                checks.append(check(BUF, cfg, buf_decls(cfg), 'M_P [T= REC_P', pairs, projection=c.id, aux=aux))
                cands.append({'what': 'buf', 'projection': c.id, 'chan': c, 'config': cfg, 'pairs': pairs})
                rec['checked'] = cfg
        projections.append(rec)
    for rv in tr.rvs.values():
        cfg, why = rvp_config(rv, tr, models)
        rec = {'id': rv.id, 'kind': 'rendezvous channel', 'name': short(rv.name),
               'messages': sum(1 for k, _, _ in rv.items if k == 'done'), 'events': len(rv.items), 'match': {}}
        rec['match'][RVP] = cfg if cfg else 'unmatched: ' + why
        if cfg and (only is None or rv.id in only):
            pairs = rvp_trace(rv)
            checks.append(check(RVP, cfg, [d % cfg for d in RVP_DECLS], 'M_P [T= REC_P', pairs, projection=rv.id))
            cands.append({'what': 'rvp', 'projection': rv.id, 'rv': rv, 'pairs': pairs})
        projections.append(rec)
    if stable:
        for pi, pt in enumerate(read_stable(stable)):
            if pt['classification'] == 'stability unconfirmed':
                continue
            checks += stable_checks(tr, pt, pi, cands)
    return projections, checks, cands


def projection_rows(tr, pid):
    for a in tr.alts:
        if a.id == pid:
            return sorted(set([r.i for s in a.all_selects() for r in s.rows] + [r.i for r in a.guard_rows]))
    for c in tr.bufs.values():
        if c.id == pid:
            out = [row.i for _, row, _ in c.items]
            for a in tr.alts:
                if any(t == c.name for k, t in a.guards.values()):
                    out += [r.i for s in a.all_selects() for r in s.rows]
            return sorted(set(out))
    for rv in tr.rvs.values():
        if rv.id == pid:
            return [row.i for _, row, _ in rv.items]
    return []


def applicable(tr, kind, prow, crows):
    evs = [tr.rows[i].event for i in prow]
    if kind == 'swap':
        m = sorted(set(crows))
        return any(tr.rows[x].event != tr.rows[y].event for x, y in zip(m, m[1:]))
    if kind == 'drop-rv':
        return 'RendezvousDone' in evs
    if kind == 'alt-guard':
        return 'AltSelected' in evs and evs.count('AltGuard') >= 2
    if kind == 'resume':
        return any(tr.rows[i].event == 'ResumeTimeout' and 'deadline' in tr.rows[i].d for i in prow)
    return True


def sub_trace(tr, lo, hi):
    """Rows lo..hi as a trace of their own (renumbered from 0)."""
    rows = [Row(k, r.t, r.event, r.thread, dict(r.d)) for k, r in enumerate(tr.rows[lo:hi + 1])]
    return rows


def ranked_checks(cs, span):
    """The checks cs of one family in the order mutants() tries them: the
    smallest row span of the projection first (span(pid) = (first row, last
    row, ...) or None: not tried); equal spans: the check with the earliest
    row, then its place in cs. No two checks tie, so the order is the same
    in every run."""
    ranked = [(s[1] - s[0], min(c['rows'], default=-1), j, c)
              for j, c in enumerate(cs) for s in [span(c['projection'])] if s]
    ranked.sort(key=lambda x: x[:3])
    return [x[3] for x in ranked]


def mutants(tr, models, base):
    """One trace mutant per (mutation, model configuration family), on the
    checked projection with the smallest row span where it applies and
    changes the trace (equal spans: the check with the earliest row, so the
    choice is the same in every run). The mutant is built from that span of
    rows alone (and used only if the span reproduces the original check)."""
    out = []
    fam = collections.OrderedDict()
    for c in base:
        if c['kind'] == 'trace' and not c.get('mutation'):
            key = c['model'] if c['model'] != BUF else ' '.join(c['config'].split(' ')[:2])
            fam.setdefault(key, []).append(c)
    spans = {}

    def span(pid):
        if pid not in spans:
            pr = projection_rows(tr, pid)
            spans[pid] = (pr[0], pr[-1], pr) if pr else None
        return spans[pid]

    for kind in MUTATIONS:
        for model, cs in fam.items():
            for c in ranked_checks(cs, span)[:20]:
                pid = c['projection']
                lo, hi, prow = spans[pid]
                own = sorted(set(c['rows']) | set(c.get('aux', ())))      # the check's own rows (its select)
                if kind == 'get-id':
                    if c['model'] != BUF or not any(e.startswith(('bout.', 'rd.', 'get5.')) for e in c['rec']):
                        continue
                elif not applicable(tr, kind, own, c['rows']):
                    continue
                lo = max(0, lo - 200)
                forced = {pid: c['config'] if c['model'] == BUF else c['model']}
                rows = sub_trace(tr, lo, hi)
                t0 = Trace(rows)
                p0 = [p for p in t0.alts + list(t0.bufs.values()) + list(t0.rvs.values())]
                # the projection's id in the sub-trace: the one with the same rows
                want = [i - lo for i in prow]
                sid = next((x.id for x in p0 if projection_rows(t0, x.id) == want), None)
                if sid is None:
                    continue
                f0 = {sid: forced[pid]}
                _, chk0, _ = analyse(t0, models, only={sid}, forced=f0)
                same = [x for x in chk0 if x['kind'] == 'trace' and x['config'] == c['config'] and x['rec'] == c['rec']
                        and x.get('select') == c.get('select')]
                if not same:
                    continue
                if kind == 'get-id':
                    _, chks, _ = analyse(t0, models, only={sid}, forced=f0, id_shift=(0, 1))
                    desc = 'first get: identity + 1'
                else:
                    mrows, desc = mutate_rows(rows, kind, [i - lo for i in own if i >= lo], [i - lo for i in c['rows']])
                    if mrows is None:
                        continue
                    _, chks, _ = analyse(Trace(mrows), models, only={sid}, forced=f0)
                    desc = desc + ' (rows from %d)' % lo
                chks = [x for x in chks if x['kind'] == 'trace' and x['model'] == c['model'] and x['config'] == c['config']
                        and x.get('select') == same[0].get('select')]
                if not chks or chks[0]['rec'] == c['rec'] or any('UNKNOWN' in e for e in chks[0]['rec']):
                    continue                    # unchanged, or a get moved before its put (no identity)
                m = chks[0]
                m.update({'mutation': kind, 'mutation_desc': desc, 'mutation_model': model, 'projection': pid,
                          'rows': [], 'aux': []})
                out.append(m)
                break
    return out


def stable_mutants(tr, stable, cands, base):
    """Per stable point: remove the first offer whose removal changes the
    refusal set of one of the point's stable-failure checks; each changed
    check must fail."""
    out = []
    points = read_stable(stable)
    for pi in sorted({c['point'] for c in base if c['kind'] == 'failures' and not c.get('mutation')}):
        pt = points[pi]
        orig = {(c['projection'], c['config']): c['X'] for c in base if c['kind'] == 'failures' and c.get('point') == pi}
        for off in pt['offers']:
            changed = [m for m in stable_checks(tr, pt, pi, cands, drop=off)
                       if (m['projection'], m['config']) in orig and m['X'] != orig[(m['projection'], m['config'])]]
            if changed:
                for m in changed:
                    m.update({'mutation': 'stable-offer', 'mutation_desc': 'offer "%s: %s" removed at stable point %d'
                              % (off[0], off[1], pi), 'mutation_model': m['model']})
                out += changed
                break
    return out


# --------------------------------------------------------------- coverage
KINDS = {'RendezvousDone': 'RendezvousDone', 'AltSelected': 'AltSelected'}


def kind_of(ev):
    if ev in KINDS:
        return KINDS[ev]
    if ev in BUF_EVENTS:
        return 'buffered'
    if ev.startswith('Wait') or ev.startswith('Resume'):
        return 'waits/resumes'
    return 'other'


def covered_rows(tr, checks):
    """The rows of the projections that have a trace check (mutants and
    stable-failure checks do not count)."""
    used = set()
    for pid in {c['projection'] for c in checks if c['kind'] == 'trace' and not c.get('mutation')}:
        used.update(projection_rows(tr, pid))
    return used


def coverage(tr, checks):
    """The share of the recorded CSP events inside a checked projection, per kind."""
    used = covered_rows(tr, checks)
    tot, cov = collections.Counter(), collections.Counter()
    for row in tr.rows:
        k = kind_of(row.event)
        tot[k] += 1
        tot['total'] += 1
        if row.i in used:
            cov[k] += 1
            cov['total'] += 1
    return {k: {'events': tot[k], 'covered': cov[k], 'share': round(cov[k] / tot[k], 4) if tot[k] else None} for k in tot}


def uncovered_groups(tr, checks, top=12):
    """The largest groups of uncovered events: (kind, object)."""
    used = covered_rows(tr, checks)
    g = collections.Counter()
    for row in tr.rows:
        if row.i not in used:
            obj = row.d.get('channel') or row.d.get('alt') or row.d.get('object') or row.d.get('barrier') or \
                row.d.get('join') or row.d.get('process') or ''
            g[(kind_of(row.event), short(obj) or row.event)] += 1
    return [{'kind': k, 'object': o, 'events': n} for (k, o), n in g.most_common(top)]


# ------------------------------------------------------------------- ProB
def key_of(chk, models):
    src = models.src(chk['model'])
    return hashlib.sha256((src + '\0' + csp_text(chk)).encode()).hexdigest()[:20]


def usable_mem_gb():
    """85 % of the machine's memory (GB); None where /proc/meminfo is missing."""
    try:
        kb = int(re.search(r'MemTotal:\s+(\d+)', open('/proc/meminfo').read()).group(1))
    except (OSError, AttributeError):
        return None
    return kb / 1024 / 1024 * 0.85


def default_mem_gb(jobs):
    """85 % of the machine's memory shared by the parallel probcli runs; None
    (no limit) where /proc/meminfo is missing."""
    gb = usable_mem_gb()
    return round(gb / max(jobs, 1), 1) if gb else None


RETRY_FACTOR = 3                    # a run stopped at the memory limit is rerun once with this many times the limit


def retry_plan(mem_gb, jobs, usable=None):
    """(limit, parallel runs) of the retries: RETRY_FACTOR x mem_gb (at most
    85 % of the memory), with as many runs at once as fit in 85 % of the
    memory (at least one, at most jobs)."""
    usable = usable_mem_gb() if usable is None else usable
    gb = mem_gb * RETRY_FACTOR
    if not usable:
        return gb, max(1, jobs // RETRY_FACTOR)
    gb = round(min(gb, usable), 1)
    return gb, max(1, min(jobs, int(usable // gb)))


def cgroup_works():
    try:
        return subprocess.run(['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=64M', 'true'],
                              capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def prob_command(args, mem_gb, cgroup):
    """(argv, preexec_fn): args under a memory limit of mem_gb, in a cgroup of
    its own (systemd-run --scope, no swap: probcli is killed at the limit) or
    with RLIMIT_AS (an allocation fails at the limit)."""
    if not mem_gb:
        return args, None
    b = int(mem_gb * 1024 ** 3)
    if cgroup:
        return ['systemd-run', '--user', '--scope', '-q', '-p', 'MemoryMax=%d' % b, '-p', 'MemorySwapMax=0'] + args, None
    return args, lambda: resource.setrlimit(resource.RLIMIT_AS, (b, b))


def run_prob(model, text, assertion, models, mem_gb=None):
    global CGROUP
    mem_gb = MEM_GB if mem_gb is None else mem_gb
    if mem_gb and CGROUP is None:
        CGROUP = cgroup_works()
    d = tempfile.mkdtemp()
    try:
        shutil.copy(os.path.join(models.dir, model + '.csp'), d)
        open(os.path.join(d, 'check.csp'), 'w').write(text)
        argv, pre = prob_command([PROB, 'check.csp', '-csp_assertion', assertion, '-c'], mem_gb, CGROUP)
        try:
            p = subprocess.run(argv, cwd=d, preexec_fn=pre, capture_output=True, text=True, timeout=TIMEOUT)
            txt = p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            return {'result': 'UNKNOWN', 'why': 'probcli did not finish within %d s' % TIMEOUT}
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return prob_result(txt, p.returncode, mem_gb)


def prob_result(txt, status=0, mem_gb=None):
    """ProB's verdict from probcli's output and exit status: PASS (the
    assertion holds), FAIL (it does not: a counterexample), or UNKNOWN (ProB
    gave up -- a node or the check timed out --, probcli ran out of memory or
    was killed, or it printed no verdict). UNKNOWN is never PASS, and a
    mutant is caught only by FAIL."""
    lines = txt.splitlines()
    lim = ' (limit %s GB)' % mem_gb if mem_gb else ''
    mem = status in (-9, 137) or bool(re.search(MEMERR, txt))
    if status in (-9, 137):
        res, why = 'UNKNOWN', 'probcli was killed (SIGKILL): out of memory' + lim
    elif re.search(MEMERR, txt):
        res, why = 'UNKNOWN', 'probcli ran out of memory%s: %s' % (lim, re.search(MEMERR, txt).group(0))
    elif re.search(r'\*\*\* UNKNOWN \*\*\*|time_out_for_node|may \*not\* be', txt):
        res, why = 'UNKNOWN', 'ProB gave up (a node timed out)'
    elif any(l.strip() == 'OK' for l in lines):
        res, why = 'PASS', None
    elif '*** FALSE ***' in txt:
        res, why = 'FAIL', None
    else:
        res, why = 'UNKNOWN', 'no verdict in the output (exit status %d)' % status
    cpu = re.search(r'Runtime for csp_assertion_check: (\d+) ms', txt)
    st = re.search(r'^STATES (\d+)', txt, re.M)
    ce = re.search(r'Result trace for \w+: (.*)', txt) or re.search(r'Refinement Check Counter-Example: \*\*\*\n(.*)', txt)
    out = {'result': res, 'cpu_ms': int(cpu.group(1)) if cpu else None, 'states': int(st.group(1)) if st else None}
    if ce and res == 'FAIL':
        out['counterexample'] = ce.group(1)[:400]
    if mem:
        out['memory'] = True
    if why:
        out['why'] = why
        out['errors'] = [l for l in lines if l.startswith('!')][:6]
    return out


BATCH_EVENTS = 40000                # a batch of more recorded events is split (parallel runs)


def prove(dirs, models, jobs, cache, skip=None):
    """Check the assertions of all report.json in dirs: trace checks batched
    per configuration (one ProB run each; a failing batch is split), the
    others one by one; results cached by check. A run stopped at the memory
    limit is not split: it is rerun once, after all others, with
    RETRY_FACTOR x the limit and fewer runs at once (retry_plan); still
    UNKNOWN then, it stays UNKNOWN, and its checks keep both attempts."""
    reports = {d: json.load(open(os.path.join(d, 'report.json'))) for d in dirs}
    allc = [c for r in reports.values() for c in r['checks']]
    for c in allc:
        c['key'] = key_of(c, models)
    results = {}
    if cache:
        os.makedirs(cache, exist_ok=True)
        for c in allc:
            f = os.path.join(cache, c['key'] + '.json')
            if c['key'] not in results and os.path.exists(f):
                results[c['key']] = dict(json.load(open(f)), cached=True)
    todo = {}
    for c in allc:
        if c['key'] not in results:
            todo.setdefault(c['key'], c)
    batches = collections.defaultdict(list)
    singles = []
    for k, c in todo.items():
        if skip and re.search(skip, c['config']):
            results[k] = {'result': 'NOT RUN', 'why': 'skipped (--skip)'}
        elif c['kind'] == 'trace' and not c.get('mutation'):
            batches[group_key(c)].append(c)
        else:
            singles.append(c)
    split = []
    for cs in batches.values():
        cur, n = [], 0
        for c in sorted(cs, key=lambda c: c['length']):
            if cur and n + c['length'] > BATCH_EVENTS:
                split.append(cur)
                cur, n = [], 0
            cur.append(c)
            n += c['length']
        split.append(cur)
    runs, retry, first = [], [], {}

    def attempt(r, mem, n):
        return {'mem_gb': mem, 'batch': n, **{k: r[k] for k in ('result', 'why', 'cpu_ms', 'states') if k in r}}

    def done(cs, r, mem, again):
        """True: r is final for cs; False: cs waits for the retry."""
        if r.get('memory') and not again and MEM_GB:
            retry.append(cs)
            for c in cs:
                first[c['key']] = attempt(r, mem, len(cs))
            return False
        if again or len(cs) == 1 or r['result'] == 'PASS':
            for c in cs:
                results[c['key']] = dict(r, batch=len(cs), **({'attempts': [first[c['key']], attempt(r, mem, len(cs))]}
                                                              if c['key'] in first else {}))
        return True

    def run_batch(cs, mem=None, again=False):
        text = csp_text(cs[0], [c['rec'] for c in cs])
        r = run_prob(cs[0]['model'], text, cs[0]['assert'], models, mem)
        runs.append({'model': cs[0]['model'], 'config': cs[0]['config'], 'traces': len(cs),
                     'events': sum(c['length'] for c in cs), **{k: r.get(k) for k in ('result', 'cpu_ms', 'states')},
                     'mem_gb': mem or MEM_GB, **({'retry': True} if again else {}), **({'why': r['why']} if 'why' in r else {})})
        if not done(cs, r, mem or MEM_GB, again) or r['result'] == 'PASS' or len(cs) == 1:
            return
        if again and r.get('memory'):                # the retry ran out of memory too: UNKNOWN, not split
            return
        if again:                                    # a FAIL or ProB's UNKNOWN in the retry: split as in the first pass
            for c in cs:
                results.pop(c['key'], None)
        half = len(cs) // 2
        run_batch(cs[:half], mem, again)
        run_batch(cs[half:], mem, again)

    def run_single(c, mem=None, again=False):
        r = run_prob(c['model'], csp_text(c), c['assert'], models, mem)
        runs.append({'model': c['model'], 'config': c['config'], 'traces': 1, 'events': c['length'],
                     'kind': c.get('mutation') or c['kind'], **{k: r.get(k) for k in ('result', 'cpu_ms', 'states')},
                     'mem_gb': mem or MEM_GB, **({'retry': True} if again else {}), **({'why': r['why']} if 'why' in r else {})})
        done([c], r, mem or MEM_GB, again)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(run_batch, split))
        list(ex.map(run_single, singles))
    stats = {'runs': len(runs), 'mem_gb': MEM_GB, 'retried': len(retry), 'unknown_after_retry': 0}
    if retry:
        gb, rj = retry_plan(MEM_GB, jobs)
        stats.update(retry_mem_gb=gb, retry_jobs=rj)
        with concurrent.futures.ThreadPoolExecutor(max_workers=rj) as ex:
            list(ex.map(lambda cs: run_batch(cs, gb, True) if cs[0]['kind'] == 'trace' and not cs[0].get('mutation')
                        else run_single(cs[0], gb, True), retry))
        stats['runs'] = len(runs)
        stats['unknown_after_retry'] = sum(1 for cs in retry if any(results[c['key']]['result'] == 'UNKNOWN' for c in cs))
    if cache:
        for k, r in results.items():
            if not r.get('cached') and r.get('result') in ('PASS', 'FAIL'):
                json.dump(r, open(os.path.join(cache, k + '.json'), 'w'))
    for d, rep in reports.items():
        for c in rep['checks']:
            c['prob'] = results.get(c['key'], {'result': 'NOT RUN'})
            c['verdict'] = verdict(c)
            if c.get('mutation'):
                c['caught'] = c['verdict'] == 'FAIL'
        rep['verdicts'] = {'%s %s' % k: n for k, n in collections.Counter(
            (c.get('mutation') or c['kind'], c['verdict']) for c in rep['checks']).items()}
        rep['prob'] = stats
        json.dump(rep, open(os.path.join(d, 'report.json'), 'w'), indent=1)
    return runs


# ------------------------------------------------------------------- main
def main():
    global MEM_GB
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('events', nargs='?')
    ap.add_argument('--out')
    ap.add_argument('--stable')
    ap.add_argument('--mutate', action='store_true')
    ap.add_argument('--models', default=os.path.join(REPO, 'docs', 'formal'))
    ap.add_argument('--prove', nargs='+', metavar='DIR')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--cache')
    ap.add_argument('--prob-mem', type=float, metavar='GB',
                    help='--prove: memory limit of each probcli run (default PROB_MEM_GB, else 85%% of the memory / '
                         '--jobs; 0: none); a run stopped at the limit is UNKNOWN')
    ap.add_argument('--skip', help='--prove: leave the checks of configurations matching this regex unchecked')
    a = ap.parse_args()
    models = Models(a.models)
    if a.prove:
        MEM_GB = (a.prob_mem if a.prob_mem is not None else
                  float(os.environ['PROB_MEM_GB']) if os.environ.get('PROB_MEM_GB') else default_mem_gb(a.jobs))
        runs = prove(a.prove, models, a.jobs, a.cache, a.skip)
        json.dump(runs, open(os.path.join(os.path.dirname(os.path.abspath(a.prove[0])), 'prob_runs.json'), 'w'), indent=1)
        n = collections.Counter(r['result'] for r in runs)
        rt = [r for r in runs if r.get('retry')]
        print('%d ProB runs (%s), %.1f s CPU; memory limit per run: %s' % (
            len(runs), dict(n), sum((r.get('cpu_ms') or 0) for r in runs) / 1000,
            '%s GB (%s)' % (MEM_GB, 'cgroup' if CGROUP else 'RLIMIT_AS') if MEM_GB else 'none'))
        if rt:
            st = json.load(open(os.path.join(a.prove[0], 'report.json')))['prob']
            print('%d runs stopped at the memory limit, rerun at %s GB (%d at once): %d still UNKNOWN' % (
                st['retried'], st['retry_mem_gb'], st['retry_jobs'], st['unknown_after_retry']))
        return
    if not (a.events and a.out):
        ap.error('EVENTS and --out are required (or --prove)')
    tr = Trace(read_rows(a.events))
    projections, checks, cands = analyse(tr, models, stable=a.stable)
    if a.mutate:
        checks += mutants(tr, models, checks)
        if a.stable:
            checks += stable_mutants(tr, a.stable, cands, checks)
    os.makedirs(os.path.join(a.out, 'checks'), exist_ok=True)
    seen = set()
    for i, c in enumerate(checks):        # files: every stable-failure check and mutant, one trace check per configuration
        if c['kind'] == 'trace' and not c.get('mutation'):
            if group_key(c) in seen:
                continue
            seen.add(group_key(c))
        c['file'] = 'checks/%05d_%s_%s.csp' % (i, c['projection'], c.get('mutation') or c['kind'])
        open(os.path.join(a.out, c['file']), 'w').write(csp_text(c))
    cov = coverage(tr, checks)
    with open(os.path.join(a.out, 'match.txt'), 'w') as f:
        for p in projections:
            f.write('%s %s %s %s\n' % (p['id'], p['kind'], p['name'], {k: v for k, v in p.items()
                                                                        if k not in ('id', 'kind', 'name', 'match')}))
            for k, v in p['match'].items():
                f.write('    %-44s %s\n' % (k, v))
    json.dump({'events_file': os.path.basename(a.events), 'models': a.models, 'projections': projections,
               'checks': checks, 'coverage': cov, 'uncovered': uncovered_groups(tr, checks)},
              open(os.path.join(a.out, 'report.json'), 'w'), indent=1)
    n = collections.Counter((c.get('mutation') or c['kind']) for c in checks)
    print('%s: %d projections, %d checks %s, coverage %.1f %%' % (os.path.basename(a.events), len(projections), len(checks),
                                                                  dict(n), 100 * cov['total']['share']))


if __name__ == '__main__':
    main()
