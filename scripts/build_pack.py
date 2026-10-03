#!/usr/bin/env python3
"""Build OliverFaust.CSP4CMSIS.<version>.pack from the .pdsc's own file list.

usage: scripts/build_pack.py <output-dir>
The archive contains the .pdsc, LICENSE and every <file> the .pdsc lists (the
include directory entry as a directory), with repository-relative paths, like
the 1.0.0 pack. The version is the first <release version="..."> entry.
Validate the result with packchk (see docs/known-issues.md).

Reproducible: two builds of the same commit are byte-identical, from any
checkout (git clone, git archive export) and at any time. Every entry gets the
same timestamp -- SOURCE_DATE_EPOCH if set, else the commit time of HEAD
(git log -1 --format=%ct) -- and fixed permissions (files 0644, directories
0755, Unix host); entries are sorted by name and compressed at a fixed level.
Only the file contents can make two packs differ. (The compressed bytes also
depend on the zlib version: the same Python/zlib gives the same archive.)
"""
import os, re, subprocess, sys, time, zipfile

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

if 'SOURCE_DATE_EPOCH' in os.environ:
    epoch = int(os.environ['SOURCE_DATE_EPOCH'])
else:
    try:
        epoch = int(subprocess.check_output(['git', '-C', root, 'log', '-1', '--format=%ct'],
                                            text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        sys.exit('no git commit time: set SOURCE_DATE_EPOCH (e.g. the commit time of the release tag)')
    dirty = subprocess.run(['git', '-C', root, 'status', '--porcelain', '--untracked-files=no', '--'] + files,
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        print("warning: packed files differ from HEAD; the timestamp is HEAD's commit time:\n" + dirty,
              file=sys.stderr)
stamp = time.gmtime(max(epoch, 315532800))[:6]   # ZIP time: UTC, not before 1980-01-01

def entry(name, is_dir):
    zi = zipfile.ZipInfo(name, date_time=stamp)
    zi.create_system = 3                          # Unix
    zi.external_attr = ((0o40755 if is_dir else 0o100644) << 16) | (0x10 if is_dir else 0)
    zi.compress_type = zipfile.ZIP_STORED if is_dir else zipfile.ZIP_DEFLATED
    return zi

out = os.path.join(sys.argv[1], f'OliverFaust.CSP4CMSIS.{version}.pack')
items = sorted([(f, False) for f in files] + [(d, True) for d in dirs])
with zipfile.ZipFile(out, 'w') as z:
    for name, is_dir in items:
        data = b'' if is_dir else open(os.path.join(root, name), 'rb').read()
        z.writestr(entry(name, is_dir), data, compresslevel=None if is_dir else 9)
print(f'{out}: {len(files)} files, timestamp {time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(max(epoch, 315532800)))} UTC')
