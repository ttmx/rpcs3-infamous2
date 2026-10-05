import pickle, numpy as np, sys, collections
T = pickle.load(open('ao-trace.pkl', 'rb')); N = T['nodes']
def fv(v): return float(np.array([v], dtype=np.uint32).view(np.float32)[0])
ZBASE = 0x8280; NBASE = 0x14f80
def leaf(i):
    op, args, val, ex = N[i]
    if op == 'ls':
        a = ex
        if 0x8000 <= a < 0xe900: r, c = divmod(a - ZBASE, 416); return f'Z[{r},{c//4}]={fv(val):.6g}'
        if NBASE <= a < NBASE + 90 * 640: r, c = divmod(a - NBASE, 640); return f'N[{r},{c//4}]=0x{val:08x}'
        return f'K@{a:05x}=0x{val:08x}({fv(val):.6g})'
    if op == 'const': return f'c0x{val:08x}({fv(val):.6g})'
    if op == 'reg0': return f'R{ex[0]}.{ex[1]}=0x{val:08x}'
    return None
def cone(root, limit=100000):
    seen = []; mark = set(); st = [root]
    while st:
        i = st.pop()
        if i in mark: continue
        mark.add(i); seen.append(i)
        for a in N[i][1]: st.append(a)
    return sorted(mark)
def show(root, maxlines=400):
    ids = cone(root); names = {}
    lines = []
    for i in ids:
        l = leaf(i)
        if l is not None: names[i] = l; continue
        op, args, val, ex = N[i]; names[i] = f't{i}'
        lines.append(f"t{i} = {op}{'' if ex is None else '<'+str(ex)+'>'}({', '.join(names.get(a, 't%d' % a) for a in args)})    ; 0x{val:08x} {fv(val):.6g}")
    return ids, lines
if __name__ == '__main__':
    lt = T['lt']; a = int(sys.argv[1], 0)
    for k in range(4):
        ids, lines = show(lt[a + 4 * k])
        ops = collections.Counter(N[i][0] for i in ids)
        print(f'word {k}: cone {len(ids)} nodes; ops {dict(ops.most_common(14))}')
