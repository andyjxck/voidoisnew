#!/usr/bin/env python3
"""Worst-case stack depth from a function, statically: frame sizes from
-fstack-usage (.su files) and direct calls (bl / b.w tail calls) from the
disassembly. Indirect calls (blx rN) cannot be followed; they are counted
and reported so the result reads as a lower bound for those paths.

usage: stackdepth.py firmware.out su_root_dir root [root ...]
"""
import os, re, subprocess, sys
from functools import lru_cache

elf, sudir, roots = sys.argv[1], sys.argv[2], sys.argv[3:]

frame = {}
for dp, _, fs in os.walk(sudir):
    for f in fs:
        if f.endswith('.su'):
            for line in open(os.path.join(dp, f)):
                p = line.rstrip('\n').split('\t')
                if len(p) >= 2:
                    name = p[0].rsplit(':', 1)[-1]
                    frame[name] = max(frame.get(name, 0), int(p[1]))

calls, indirect = {}, {}
cur = None
fn_re = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
call_re = re.compile(r'\t(bl|b\.w|b)\s+[0-9a-f]+ <([^+>]+)>')
for line in subprocess.run(['arm-none-eabi-objdump', '-d', elf],
                           capture_output=True, text=True).stdout.splitlines():
    m = fn_re.match(line)
    if m:
        cur = m.group(1)
        calls.setdefault(cur, set())
        continue
    if cur is None:
        continue
    m = call_re.search(line)
    if m and m.group(2) != cur:
        calls[cur].add(m.group(2))
    elif '\tblx\tr' in line:
        indirect[cur] = indirect.get(cur, 0) + 1

@lru_cache(None)
def depth(f, stack=()):
    if f in stack:
        return 0, [f + ' (recursion)'], 0
    best, path, ind = 0, [], 0
    for c in calls.get(f, ()):
        d, p, i = depth(c, stack + (f,))
        if d > best:
            best, path, ind = d, p, i
    return frame.get(f, 0) + best, [f] + path, ind + indirect.get(f, 0)

for r in roots:
    d, p, i = depth(r)
    print(f"{r}: {d} B worst direct-call path, {i} indirect call sites on it")
    print('   ' + ' -> '.join(f"{x}({frame.get(x.split(' ')[0], 0)})" for x in p))
