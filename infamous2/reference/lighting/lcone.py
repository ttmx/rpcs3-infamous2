"""Expression viewer for a lighting trace.  lcone.show(T, node, y, x) prints the SSA cone with named leaves."""
import pickle, numpy as np, collections, struct
A_EA, B_EA, L_EA, P_EA = 0x37400b80, 0x37784b80, 0x37d6d690, 0x00a93c80
def fv(v): return float(np.array([v], dtype=np.uint32).view(np.float32)[0])
def load(tile): return pickle.load(open(f'trace-{tile}.pkl', 'rb'))
def leaf(T, i, y, x):
    op, args, val, ex = T['nodes'][i]
    if op == 'ls':
        ea = T['src'].get(ex)
        if ea is not None:
            if A_EA <= ea < A_EA + 3686400: o = (ea - A_EA) // 4; return f'A[{o//1280-y:+d},{o%1280-x:+d}]=0x{val:08x}'
            if B_EA <= ea < B_EA + 3686400: o = (ea - B_EA) // 4; return f'B[{o//1280-y:+d},{o%1280-x:+d}]=0x{val:08x}'
            if L_EA <= ea < L_EA + 384: o = ea - L_EA; return f'L{o//48}+{o%48:02x}={fv(val):.6g}/0x{val:08x}'
            if P_EA <= ea < P_EA + 256: return f'P+{ea-P_EA:02x}={fv(val):.6g}/0x{val:08x}'
            return f'M@{ea:08x}=0x{val:08x}'
        return f'K@{ex:05x}={fv(val):.6g}/0x{val:08x}'
    if op == 'const': return f'c({fv(val):.6g}/0x{val:x})'
    if op == 'reg0': return f'R{ex[0]}.{ex[1]}=0x{val:08x}'
    if op == 'chan': return f'chan=0x{val:x}'
def cone(T, root):
    N = T['nodes']; mark = set(); st = [root]
    while st:
        i = st.pop()
        if i in mark: continue
        mark.add(i); st.extend(N[i][1])
    return sorted(mark)
def show(T, root, y, x):
    N = T['nodes']; names = {}; out = []
    for i in cone(T, root):
        l = leaf(T, i, y, x)
        if l: names[i] = l; continue
        op, args, val, ex = N[i]; names[i] = f't{i}'
        out.append(f"t{i} = {op}{'' if ex is None else '<'+str(ex)+'>'}({', '.join(names.get(a, 't%d' % a) for a in args)})   ; {fv(val):.6g} / 0x{val:08x}")
    return out
def summary(T, root, y, x):
    N = T['nodes']; ids = cone(T, root); ops = collections.Counter(N[i][0] for i in ids)
    leaves = sorted({leaf(T, i, y, x).split('=')[0] for i in ids if leaf(T, i, y, x)})
    return len(ids), dict(ops.most_common()), leaves
