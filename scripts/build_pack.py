#!/usr/bin/env python3
"""Build OliverFaust.CSP4CMSIS.<version>.pack from the .pdsc's own file list.

usage: scripts/build_pack.py <output-dir>
The archive contains the .pdsc, LICENSE and every <file> the .pdsc lists (the
include directory entry as a directory), with repository-relative paths, like
the 1.0.0 pack. The version is the first <release version="..."> entry.
Validate the result with packchk (see docs/known-issues.md).
"""
import os, re, sys, zipfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
pdsc = os.path.join(root, 'OliverFaust.CSP4CMSIS.pdsc')
text = open(pdsc, encoding='utf-8').read()
version = re.search(r'<release version="([^"]+)"', text).group(1)
names = [m.group(1) for m in re.finditer(r'<file category="[^"]+" name="([^"]+)"', text)]
files = ['OliverFaust.CSP4CMSIS.pdsc', 'LICENSE']
dirs = []
for n in names:
    if n.endswith('/'):
        dirs.append(n)
        continue
    if not os.path.isfile(os.path.join(root, n)):
        sys.exit(f'missing file listed in the pdsc: {n}')
    files.append(n)
out = os.path.join(sys.argv[1], f'OliverFaust.CSP4CMSIS.{version}.pack')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for f in files:
        z.write(os.path.join(root, f), f)
    for d in dirs:
        z.writestr(d, '')
print(f'{out}: {len(files)} files')
