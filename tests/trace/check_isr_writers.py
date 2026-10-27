#!/usr/bin/env python3
"""Regression: the stable-failure check of a KeepNewest channel that no
interrupt writes (tools/trace/csp_trace.py buf_failures).

    PROBCLI=probcli check_isr_writers.py

A synthetic trace: the reader of a KeepNewest channel of capacity 1 waits
on the empty channel at a stable point; its writer, a task, has not
written yet (it puts after the point). No interrupt writes the channel, so
the network refuses every event of the channel there (t_new, isr_new, rd)
and the check must PASS. csp_trace.py once gave every KeepNewest/KeepOldest
channel an interrupt writing forever (SYS_KNE(c, 0, -1)), whose isr_new the
model could always do: the check FAILed although the trace is correct (the
STM32N6570-DK pose application's pp_to_disp_chan). The control runs that
old configuration on the same check file and must FAIL. With
--isr-writer chan=1 the interrupt's puts are awaited from the environment:
isr_new leaves X and the check PASSes as well.

Exit status 0 if all three hold.
"""
import json, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', '..', 'tools', 'trace')
PROB = os.environ.get('PROBCLI', 'probcli')

EVENTS = '''time_s,component,event,thread,value
0.00000100,CSP,ProcessStart,0x20000100,"context=0x20000100, process=reader_proc"
0.00000200,CSP,ProcessName,,name=reader
0.00000300,CSP,ProcessStart,0x20000200,"context=0x20000200, process=writer_proc"
0.00000400,CSP,ProcessName,,name=writer
0.00001000,CSP,WaitBufferedEmpty,0x20000100,"context=0x20000100, channel=chan"
0.00003000,CSP,BufferedPut,0x20000200,"context=0x20000200, channel=chan, count=1, capacity=1, policy=KeepNewest, slot=0"
0.00003100,CSP,Resume,0x20000100,"context=0x20000100, object=chan"
0.00003200,CSP,BufferedGet,0x20000100,"context=0x20000100, channel=chan, count=0, capacity=1, policy=KeepNewest, slot=0"
0.00004000,CSP,WaitBufferedEmpty,0x20000100,"context=0x20000100, channel=chan"
'''
STABLE = '''start_us,end_us,classification,reason,offers
11.000,20.000,deadlock,no wait can end,reader: chan ?
'''


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s failed:\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def failures(out, extra=()):
    run(sys.executable, '-I', os.path.join(TOOLS, 'csp_trace.py'), os.path.join(out, 'events.csv'),
        '--stable', os.path.join(out, 'stable.csv'), '--out', os.path.join(out, 'conf'), *extra)
    run(sys.executable, '-I', os.path.join(TOOLS, 'csp_trace.py'), '--prove', os.path.join(out, 'conf'), '--jobs', '1')
    r = json.load(open(os.path.join(out, 'conf', 'report.json')))
    f = [c for c in r['checks'] if c['kind'] == 'failures' and not c.get('mutation')]
    if len(f) != 1:
        sys.exit('expected one stable-failure check, got %d' % len(f))
    return f[0], os.path.join(out, 'conf')


def main():
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, text in (('events.csv', EVENTS), ('stable.csv', STABLE)):
            open(os.path.join(tmp, name), 'w').write(text)
        c, conf = failures(tmp)
        good = c['verdict'] == 'PASS' and c['config'].endswith('(1, 0, 0)') and c.get('isr_writers') == 0 \
            and c.get('isr_writers_source') == 'default 0' and 'isr_new' in c['X']
        print('no ISR writer (default 0): %s, X %s, ISR writers %d from %s: %s'
              % (c['config'], c['X'], c.get('isr_writers', -1), c.get('isr_writers_source', 'none'), c['verdict']))
        ok &= good
        # control: the old configuration (an interrupt writing forever) on the same check file
        path = os.path.join(conf, c['file'])
        old = re.sub(r'SYS_KNE\(1, 0, 0\)', 'SYS_KNE(1, 0, -1)', open(path).read())
        open(path, 'w').write(old)
        shutil.copy(os.path.join(HERE, '..', '..', 'docs', 'formal', c['model'] + '.csp'), os.path.dirname(path))
        r = subprocess.run([PROB, path, '-csp_assertion', 'F_P :[deadlock free [F]]'], capture_output=True, text=True)
        deadlocks = 'reaches a deadlock' in r.stdout
        print('control, old configuration SYS_KNE(1, 0, -1): %s (expected FAIL)'
              % ('PASS' if deadlocks else 'FAIL' if 'is deadlock free' in r.stdout else 'no verdict'))
        ok &= not deadlocks and 'is deadlock free' in r.stdout
        c, _ = failures(tmp, ('--isr-writer', 'chan=1'))
        good = c['verdict'] == 'PASS' and c['config'].endswith('(1, 0, -1)') and 'isr_new' not in c['X']
        print('one ISR writer (--isr-writer chan=1): %s, X %s: %s' % (c['config'], c['X'], c['verdict']))
        ok &= good
    print('isr writers: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
