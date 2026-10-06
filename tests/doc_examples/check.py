#!/usr/bin/env python3
"""Compile check for the code examples of the CSP4CMSIS API reference (api.md).

usage:
  check.py extract <api.md>
      Writes every ```cpp block of the page to examples/NN_<section>.cpp (examples)
      or examples/NN_<section>.synopsis (blocks preceded by <!-- synopsis ... -->).
  check.py run [--page <api.md>] [--st <CubeIDE project>] [--st-gxx <arm-none-eabi-g++>]
      --page   first verifies that examples/ is what `extract` would write now;
      --st     compiles every example against ST's CMSIS-RTOS2 wrapper with the STM32G4 headers
               and FreeRTOS of an STM32CubeIDE project (the book's NUCLEO-G474RE projects).
      Every synopsis line must occur in a csp4cmsis/inc header (whitespace, comments,
      `inline` and function bodies ignored).

An example passes if it compiles with -Wall -Wextra and no warning.
The CMSIS-pack configuration is the csolution project in pack/ (cbuild; see README.md).
"""
import glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
INC = os.path.join(REPO, 'csp4cmsis', 'inc')
EXAMPLES = os.path.join(HERE, 'examples')


def page_blocks(page):
    """(name, kind, code) for every ```cpp block, numbered in page order."""
    text = open(page, encoding='utf-8').read()
    section, out = 'intro', []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        m = re.match(r'##\s+(\d+[a-z]?)\.', lines[i])
        if m:
            section = 's' + m.group(1)
        if lines[i].strip() == '```cpp':
            j = i + 1
            while lines[j].strip() != '```':
                j += 1
            prev = next((l for l in reversed(lines[:i]) if l.strip()), '')
            kind = 'synopsis' if prev.startswith('<!-- synopsis') else 'cpp'
            out.append((f'{len(out) + 1:02d}_{section}', kind, '\n'.join(lines[i + 1:j]) + '\n'))
            i = j
        i += 1
    return out


def extract(page):
    os.makedirs(EXAMPLES, exist_ok=True)
    for f in glob.glob(os.path.join(EXAMPLES, '*')):
        os.remove(f)
    for name, kind, code in page_blocks(page):
        with open(os.path.join(EXAMPLES, f'{name}.{kind}'), 'w', encoding='utf-8') as f:
            f.write(code)
    print(f'extracted {len(page_blocks(page))} blocks to {os.path.relpath(EXAMPLES, REPO)}/')


def norm(line):
    line = re.sub(r'//.*', '', line)
    line = re.sub(r'\{.*\}\s*$', ';', line)          # one-line body -> declaration
    line = re.sub(r'\)\s*\{\s*$', ');', line)         # opening brace -> declaration
    line = re.sub(r'^\s*inline\s+', '', line)
    return re.sub(r'\s+;', ';', ' '.join(line.split()))


def check_synopsis(path):
    header_lines = set()
    for h in glob.glob(os.path.join(INC, 'csp', '*.h')):
        header_lines.update(norm(l) for l in open(h, encoding='utf-8'))
    missing = [l for l in open(path, encoding='utf-8') if norm(l) and norm(l) not in header_lines]
    return missing


def st_command(project, gxx):
    p = lambda *a: '-I' + os.path.join(project, *a)
    fr = ('Middlewares', 'Third_Party', 'FreeRTOS', 'Source')
    return [gxx, '-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
            '-std=gnu++17', '-O0', '-fno-exceptions', '-fno-rtti', '-fno-use-cxa-atexit',
            '-DUSE_HAL_DRIVER', '-DSTM32G474xx', '-DUSE_NUCLEO_64',
            '-DCSP4CMSIS_MAX_SYSCALL_INTERRUPT_PRIORITY=5', '-DCSP4CMSIS_DEVICE_HEADER="stm32g4xx.h"',
            p('Core', 'Inc'), p('Drivers', 'STM32G4xx_HAL_Driver', 'Inc'),
            p('Drivers', 'STM32G4xx_HAL_Driver', 'Inc', 'Legacy'),
            p('Drivers', 'CMSIS', 'Device', 'ST', 'STM32G4xx', 'Include'), p('Drivers', 'CMSIS', 'Include'),
            p('Drivers', 'BSP', 'STM32G4xx_Nucleo'), p(*fr, 'include'), p(*fr, 'CMSIS_RTOS_V2'),
            p(*fr, 'portable', 'GCC', 'ARM_CM4F'), '-I' + INC]


def run(args):
    fails = 0
    if '--page' in args:
        want = {f'{n}.{k}': c for n, k, c in page_blocks(args[args.index('--page') + 1])}
        have = {os.path.basename(f): open(f, encoding='utf-8').read()
                for f in glob.glob(os.path.join(EXAMPLES, '*'))}
        same = want == have
        fails += 0 if same else 1
        print(f"{'ok  ' if same else 'FAIL'} examples/ matches the page ({len(want)} blocks)")
    configs = []
    if '--st' in args:
        gxx = args[args.index('--st-gxx') + 1] if '--st-gxx' in args else 'arm-none-eabi-g++'
        configs.append(('st-g4', st_command(args[args.index('--st') + 1], gxx)))
    for f in sorted(glob.glob(os.path.join(EXAMPLES, '*.synopsis'))):
        missing = check_synopsis(f)
        fails += 1 if missing else 0
        print(f"{'FAIL' if missing else 'ok  '} [headers] {os.path.basename(f)}")
        for l in missing:
            print('      not in the headers: ' + l.rstrip())
    for ctx, cmd in configs:
        for f in sorted(glob.glob(os.path.join(EXAMPLES, '*.cpp'))):
            r = subprocess.run(cmd + ['-Wall', '-Wextra', '-c', f, '-o', os.devnull],
                               capture_output=True, text=True)
            ok = r.returncode == 0 and 'warning:' not in r.stderr
            fails += 0 if ok else 1
            print(f"{'ok  ' if ok else 'FAIL'} [{ctx}] {os.path.basename(f)}")
            if not ok:
                print('\n'.join('      ' + l for l in r.stderr.splitlines()[:12]))
    print(f"doc examples: {'all passed' if fails == 0 else str(fails) + ' failed'}")
    return 1 if fails else 0


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'extract':
        extract(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == 'run':
        sys.exit(run(sys.argv[2:]))
    else:
        print(__doc__)
        sys.exit(2)
