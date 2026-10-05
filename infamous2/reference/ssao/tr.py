import pickle, numpy as np, collections
T = pickle.load(open('ao-trace-tile.pkl', 'rb')); N = T['nodes']; lt = T['lt']
ZB, NB, OUT = 0x8280, 0x14f80, 0x31180
def fv(v): return float(np.array([v], dtype=np.uint32).view(np.float32)[0])
def leaf(i, py=0, px=0):
    op, args, val, ex = N[i]
    if op == 'ls':
        a = ex
        if ZB <= a < ZB + 63 * 416: r, c = divmod(a - ZB, 416); return f'Z[{r-9-py:+d},{c//4-12-px:+d}]={fv(val):.5g}'
        if NB <= a < NB + 90 * 640: r, c = divmod(a - NB, 640); return f'N[{r},{c//4}]=0x{val:08x}'
        return f'K@{a:05x}={fv(val):.6g}/0x{val:08x}'
    if op == 'const': return f'c({fv(val):.6g}/0x{val:x})'
    if op == 'reg0': return f'R{ex[0]}.{ex[1]}=0x{val:08x}'
def cone(root):
    mark = set(); st = [root]
    while st:
        i = st.pop()
        if i in mark: continue
        mark.add(i); st.extend(N[i][1])
    return sorted(mark)
def show(root, py=0, px=0):
    ids = cone(root); names = {}; out = []
    for i in ids:
        l = leaf(i, py, px)
        if l: names[i] = l; continue
        op, args, val, ex = N[i]; names[i] = f't{i}'
        out.append(f"t{i} = {op}{'' if ex is None else '<'+str(ex)+'>'}({', '.join(names.get(a, 't%d' % a) for a in args)})   ; {fv(val):.6g} / 0x{val:08x}")
    return out
