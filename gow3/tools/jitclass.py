#!/usr/bin/env python3
"""jitclass.py <perf.data>: what kind of host instructions the SPU threads' samples in recompiled code fall on.
Uses the cache's ASMJIT/.objects of the same run. Samples are counted on the instruction before the sampled address
(the one that was executing) as well as on it."""
import mmap, struct, sys, pathlib, subprocess, tempfile, collections, re, bisect
cache = pathlib.Path(__file__).resolve().parent.parent / 'cache'
funcs = []
f = open(cache / 'rpcs3/ASMJIT/.objects', 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
for pos in range(0, 0x10000000, 16):
    addr, size, off = struct.unpack_from('<QII', m, pos)
    if not addr: break
    end = m.find(b'\0', off + size, min(off + size + 4096, len(m)))
    funcs.append((addr, size, off, m[off + size:end].decode('utf8', 'replace')))
funcs.sort(); starts = [x[0] for x in funcs]
ips = collections.Counter()
for l in subprocess.run(['perf', 'script', '-i', sys.argv[1], '-F', 'ip'], capture_output=True, text=True).stdout.split():
    try: ips[int(l, 16)] += 1
    except ValueError: pass
byf = collections.defaultdict(dict); other = 0
for ip, c in ips.items():
    i = bisect.bisect_right(starts, ip) - 1
    if i >= 0 and ip < funcs[i][0] + funcs[i][1] and funcs[i][3].startswith('__spu'): byf[i][ip] = c
    else: other += c
def cls(t):
    op = t.split()[0]
    if 'rsp' in t and op.startswith(('vmov', 'mov')): return 'stack spill or reload'
    if '[r13' in t: return 'guest register in thread context (load)' if re.search(r',.*\[r13', t) else 'guest register in thread context (store)'
    if op.startswith('vmov') and '[' not in t: return 'register to register move'
    if op.startswith('vmov') and '[rip' in t: return 'constant load'
    if op in ('vfixupimmps', 'vcmpneqps', 'vrangeps', 'vcmpunordps', 'vcmpordps') or (op == 'vpternlogd' and False): return 'float fix-up'
    if op == 'vpshufb': return 'byte shuffle'
    if op.startswith(('vmul', 'vfmadd', 'vfnmadd', 'vfmsub', 'vadd', 'vsub', 'vdiv', 'vrcp', 'vrsqrt', 'vcvt', 'vcmp', 'vmax', 'vmin')) and op.endswith(('ps', 'pd', 'dq', 'ss')): return 'float arithmetic'
    if '[rbp' in t or '[r15' in t: return 'local store access'
    if op.startswith(('j', 'call', 'ret', 'cmp', 'test')): return 'compare and branch'
    if op.startswith('v') or 'xmm' in t: return 'other vector'
    return 'integer'
at = collections.Counter(); prev = collections.Counter(); ops = collections.Counter(); tot = 0
static = collections.Counter()
for i, d in sorted(byf.items(), key=lambda kv: -sum(kv[1].values()))[:400]:
    addr, size, off, name = funcs[i]
    with tempfile.NamedTemporaryFile(suffix='.bin') as t:
        t.write(m[off:off + size]); t.flush()
        dis = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--adjust-vma', hex(addr), t.name], capture_output=True, text=True).stdout
    lines = []
    for l in dis.splitlines():
        mm = re.match(r'\s*([0-9a-f]+):\t[0-9a-f ]+\t(.*)', l)
        if mm: lines.append((int(mm[1], 16), mm[2]))
    idx = {a: k for k, (a, _) in enumerate(lines)}
    for ip, c in d.items():
        k = idx.get(ip)
        if k is None: continue
        if cls(lines[max(k - 1, 0)][1]).endswith('(store)'): static[name] += c
        static['all ' + name] += c
        tot += c; at[cls(lines[k][1])] += c; prev[cls(lines[max(k - 1, 0)][1])] += c; ops[lines[max(k - 1, 0)][1].split()[0]] += c
jit = sum(sum(d.values()) for d in byf.values())
print('samples', sum(ips.values()), 'in recompiled SPU code', jit, 'classified', tot)
for k, c in prev.most_common(): print('%-24s %5.1f%% (on the sampled address itself: %5.1f%%)' % (k, c * 100 / tot, at[k] * 100 / tot))
print(' '.join('%s:%.1f%%' % (o, c * 100 / tot) for o, c in ops.most_common(30)))

for n, c in [x for x in static.most_common(200) if not x[0].startswith('all ')][:25]: print('%-30s context stores %5d of %5d samples in it (%.0f%%)' % (n, c, static['all ' + n], c * 100 / static['all ' + n]))
