#!/usr/bin/env python3
"""Compile-time checks for CSP4CMSIS.

usage: run_checks.py <compile_commands.json of a harness build> [...]
Each probe is compiled with the exact flags of the harness's bc_tests.cpp
(-fsyntax-only). First line: EXPECT-ERROR probes must fail with the given text
in the diagnostics; EXPECT-WARNING probes must compile, with the given text in
a warning; EXPECT-OK probes must compile without warnings. `a || b` accepts
either text (compilers word their errors differently).
Optional directives on the following lines:
  // CONTEXT: <text>      run only for compile_commands.json paths containing <text>
  // EXTRA-FLAGS: <flags> added after the harness flags; {here} = this directory
"""
import json, os, re, shlex, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
fails = 0
for cc_path in sys.argv[1:]:
    entry = [e for e in json.load(open(cc_path)) if e['file'].endswith('bc_tests.cpp')][0]
    args, cmd, skip = shlex.split(entry['command']), [], False
    for a in args:
        if skip: skip = False; continue
        if a in ('-o', '-MF', '-MT'): skip = True; continue
        if a in ('-c', '-MD', '-MMD') or a.endswith('bc_tests.cpp'): continue
        cmd.append(a)
    ctx = '/'.join(cc_path.split('/')[-4:-1])
    for probe in sorted(f for f in os.listdir(here) if f.endswith('.cpp')):
        lines = open(os.path.join(here, probe)).read().split('\n')
        m = re.match(r'// EXPECT-(ERROR|WARNING|OK):\s*(.*)', lines[0])
        ctx_only = [l.split(':', 1)[1].strip() for l in lines[1:6] if l.startswith('// CONTEXT:')]
        extra = [f for l in lines[1:6] if l.startswith('// EXTRA-FLAGS:')
                 for f in shlex.split(l.split(':', 1)[1].replace('{here}', here))]
        if ctx_only and not any(c in cc_path for c in ctx_only):
            continue
        r = subprocess.run(cmd + extra + ['-fsyntax-only', '-Wall', '-Wextra', os.path.join(here, probe)],
                           capture_output=True, text=True)
        diag = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', r.stderr)
        want = [w.strip() for w in m.group(2).split('||')]
        if m.group(1) == 'ERROR':
            ok = r.returncode != 0 and any(w in diag for w in want)
        elif m.group(1) == 'WARNING':
            ok = r.returncode == 0 and 'warning:' in diag and any(w in diag for w in want)
        else:
            ok = r.returncode == 0 and 'warning:' not in diag
        fails += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} [{ctx}] {probe}: expect {m.group(1)} {m.group(2)!r}")
        if not ok:
            print('\n'.join('      ' + l for l in diag.splitlines()[:8]))
print(f"compile checks: {'all passed' if fails == 0 else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
