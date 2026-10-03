#!/usr/bin/env python3
"""nucleo_run.py <elf> <log> [timeout_s]: flash a NUCLEO-G474RE over its ST-LINK and
capture the VCP (115200 8N1) until EOT (0x04) or timeout. The port is opened BEFORE
programming (programming ends with a reset), so the log starts at the first byte.
Only this process reads the port."""
import glob, hashlib, os, subprocess, sys, time
import serial

elf, log = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 900
CLI = os.path.expanduser('~/st/STM32CubeProgrammer/bin/STM32_Programmer_CLI')

ports = sorted(glob.glob('/dev/serial/by-id/*STLINK*') + glob.glob('/dev/serial/by-id/*STMicroelectronics*'))
if not ports:
    sys.exit('no ST-LINK VCP under /dev/serial/by-id')
port = ports[0]
sha = hashlib.sha256(open(elf, 'rb').read()).hexdigest()

with serial.Serial(port, 115200, timeout=0.2) as ser, open(log, 'wb') as out:
    out.write(('# elf: %s sha256=%s\n# port: %s\n# start: %s\n' % (elf, sha, port, time.strftime('%F %T'))).encode())
    ser.reset_input_buffer()
    prog = subprocess.run([CLI, '-c', 'port=SWD', 'mode=UR', '-w', elf, '-v', '-rst'],
                          capture_output=True, text=True)
    open(log + '.flash', 'w').write(prog.stdout + prog.stderr)
    if prog.returncode != 0 or 'Download verified successfully' not in prog.stdout:
        sys.exit('programming failed, see %s.flash' % log)
    t0 = time.time()
    while time.time() - t0 < timeout:
        b = ser.read(4096)
        if b:
            out.write(b); out.flush()
            if b'\x04' in b:
                break
    else:
        out.write(b'\n# TIMEOUT after %d s\n' % timeout)
    out.write(('\n# end: %s (%.0f s)\n' % (time.strftime('%F %T'), time.time() - t0)).encode())
print(open(log, 'rb').read().decode(errors='replace').count('\n'), 'lines ->', log)
