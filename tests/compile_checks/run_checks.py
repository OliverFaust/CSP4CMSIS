#!/usr/bin/env python3
"""Compile-time checks for CSP4CMSIS 2.0.

usage: run_checks.py <compile_commands.json of a harness build> [...]
Each probe is compiled with the exact flags of the harness's bc_tests.cpp
(-fsyntax-only). EXPECT-ERROR probes must fail with the given text in the
diagnostics; EXPECT-OK probes must compile without warnings.
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
    ctx = os.path.basename(os.path.dirname(cc_path))
    for probe in sorted(f for f in os.listdir(here) if f.endswith('.cpp')):
        first = open(os.path.join(here, probe)).readline()
        m = re.match(r'// EXPECT-(ERROR|OK):\s*(.*)', first)
        r = subprocess.run(cmd + ['-fsyntax-only', '-Wall', '-Wextra', os.path.join(here, probe)],
                           capture_output=True, text=True)
        diag = re.sub(r'\x1b\[[0-9;]*m', '', r.stderr)
        if m.group(1) == 'ERROR':
            ok = r.returncode != 0 and m.group(2) in diag
        else:
            ok = r.returncode == 0 and 'warning:' not in diag
        fails += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} [{ctx}] {probe}: expect {m.group(1)} {m.group(2)!r}")
        if not ok:
            print('\n'.join('      ' + l for l in diag.splitlines()[:8]))
print(f"compile checks: {'all passed' if fails == 0 else str(fails) + ' failed'}")
sys.exit(1 if fails else 0)
