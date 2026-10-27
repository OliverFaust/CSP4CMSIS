"""Event names and value texts from .scvd files, without eventlist.

events(scvd_files, records) yields, for each Event Recorder record (as
decode.read_log yields them), the dict eventlist would give decode.py:
time (seconds: time stamp / the frequency of Event Recorder's init record
0xFF00), eventProperty and value. Values follow the .scvd value template;
%d is decimal, %x and %S hexadecimal (0x%08X: decode.py resolves %S
addresses to symbols itself), %t the text of the expression (data records:
their text). Expressions: C operators on val1..val4, numbers and strings.
"""
import re
import xml.etree.ElementTree as ET

TOKEN = re.compile(r'\s*(?:(0[xX][0-9A-Fa-f]+|\d+)|(val[1-4])|"((?:[^"\\]|\\.)*)"|'
                   r'(<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!?:()<>]))')
BINARY = {'||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6, '<': 7, '<=': 7, '>': 7, '>=': 7,
          '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10}
FIELD = re.compile(r'%([a-zA-Z])\[([^\]]*)\]')


def tokens(text):
    pos, out = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            if text[pos:].strip() == '':
                break
            raise ValueError('scvd expression: cannot parse %r' % text[pos:])
        num, var, string, op = m.groups()
        out.append(('n', int(num, 0)) if num else ('v', var) if var else
                   ('s', string) if string is not None else ('o', op))
        pos = m.end()
    return out


class Expr:
    def __init__(self, text):
        self.toks, self.i = tokens(text), 0
        self.tree = self.ternary()
        if self.i != len(self.toks):
            raise ValueError('scvd expression: trailing tokens in %r' % text)

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self, op=None):
        t = self.peek()
        if op is not None and t != ('o', op):
            raise ValueError('scvd expression: expected %r' % op)
        self.i += 1
        return t

    def ternary(self):
        cond = self.binary(1)
        if self.peek() == ('o', '?'):
            self.take('?')
            a = self.ternary()
            self.take(':')
            b = self.ternary()
            return ('?', cond, a, b)
        return cond

    def binary(self, level):
        left = self.unary()
        while True:
            kind, op = self.peek()
            if kind != 'o' or op not in BINARY or BINARY[op] < level:
                return left
            self.take()
            left = (op, left, self.binary(BINARY[op] + 1))

    def unary(self):
        kind, v = self.take()
        if kind == 'o' and v in '-~!':
            return ('u' + v, self.unary())
        if kind == 'o' and v == '(':
            e = self.ternary()
            self.take(')')
            return e
        if kind in ('n', 'v', 's'):
            return (kind, v)
        raise ValueError('scvd expression: unexpected %r' % (v,))

    def eval(self, vals, node=None):
        node = self.tree if node is None else node
        op = node[0]
        if op == 'n' or op == 's':
            return node[1]
        if op == 'v':
            i = int(node[1][3]) - 1
            return vals[i] if vals is not None and i < len(vals) else 0
        if op == '?':
            return self.eval(vals, node[2]) if self.eval(vals, node[1]) else self.eval(vals, node[3])
        if op[0] == 'u':
            x = self.eval(vals, node[1])
            return {'-': -x, '~': ~x & 0xFFFFFFFF, '!': int(not x)}[op[1]]
        a, b = self.eval(vals, node[1]), self.eval(vals, node[2])
        r = {'||': lambda: int(bool(a) or bool(b)), '&&': lambda: int(bool(a) and bool(b)),
             '|': lambda: a | b, '^': lambda: a ^ b, '&': lambda: a & b, '==': lambda: int(a == b),
             '!=': lambda: int(a != b), '<': lambda: int(a < b), '<=': lambda: int(a <= b),
             '>': lambda: int(a > b), '>=': lambda: int(a >= b), '<<': lambda: a << b, '>>': lambda: a >> b,
             '+': lambda: a + b, '-': lambda: a - b, '*': lambda: a * b, '/': lambda: a // b if b else 0,
             '%': lambda: a % b if b else 0}[op]()
        return r & 0xFFFFFFFF if isinstance(r, int) else r


class Template:
    def __init__(self, text):
        self.parts = []
        pos = 0
        for m in FIELD.finditer(text):
            self.parts.append(text[pos:m.start()])
            self.parts.append((m.group(1), Expr(m.group(2))))
            pos = m.end()
        self.parts.append(text[pos:])

    def format(self, vals, payload):
        out = []
        for p in self.parts:
            if isinstance(p, str):
                out.append(p)
                continue
            spec, e = p
            if payload is not None and spec == 't':
                out.append(payload.split(b'\0')[0].decode('ascii', 'replace'))
                continue
            v = e.eval(vals)
            if isinstance(v, str):
                out.append(v)
            elif spec == 'd':
                out.append('%d' % v)
            else:
                out.append('0x%08X' % v)
        return ''.join(out)


def definitions(scvd_files):
    """{event id: (property, Template)} of the .scvd files."""
    defs = {}
    for f in scvd_files:
        for ev in ET.parse(f).getroot().iter('event'):
            if ev.get('id') is None:
                continue
            eid = Expr(ev.get('id')).eval(None)
            defs[eid & 0xFFFF] = (ev.get('property', ''), Template(ev.get('value', '')))
    return defs


def events(scvd_files, records):
    defs = definitions(scvd_files)
    freq = None
    for eid, ts, irq, vals, payload in records:
        if eid == 0xFF00 and vals and freq is None:
            freq = vals[1]
        prop, tmpl = defs.get(eid, ('', None))
        value = tmpl.format(vals, payload) if tmpl else ''
        yield {'time': '%.9f' % (ts / freq if freq else 0.0), 'eventProperty': prop, 'value': value}
