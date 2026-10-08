#!/usr/bin/env python3
"""sched.py <pid> <seconds>: per thread over the interval: CPU time, time spent runnable but not running, voluntary and
involuntary context switches per second (from /proc, no privileges needed). Threads above 2% of a core."""
import os, sys, time
pid = sys.argv[1]; secs = float(sys.argv[2])
def snap():
    d = {}
    for t in os.listdir(f'/proc/{pid}/task'):
        try:
            run, wait, slices = map(int, open(f'/proc/{pid}/task/{t}/schedstat').read().split())
            st = open(f'/proc/{pid}/task/{t}/status').read()
            vol = int(st.split('voluntary_ctxt_switches:')[1].split()[0]); nvol = int(st.split('nonvoluntary_ctxt_switches:')[1].split()[0])
            d[t] = (run, wait, vol, nvol, open(f'/proc/{pid}/task/{t}/comm').read().strip())
        except (OSError, ValueError, IndexError): pass
    return d
a = snap(); time.sleep(secs); b = snap()
rows = []
for t, v in b.items():
    if t in a:
        rows.append(((v[0] - a[t][0]) / 1e9 / secs, (v[1] - a[t][1]) / 1e9 / secs, (v[2] - a[t][2]) / secs, (v[3] - a[t][3]) / secs, v[4], t))
print('%-18s %7s %9s %9s %9s' % ('thread', 'cores', 'runnable', 'vol/s', 'invol/s'))
for r in sorted(rows, reverse=True):
    if r[0] > 0.02: print('%-18s %7.2f %9.3f %9.0f %9.0f' % (r[4], r[0], r[1], r[2], r[3]))
print('all threads: %.2f cores, runnable-waiting %.2f, voluntary switches %.0f/s' % (sum(r[0] for r in rows), sum(r[1] for r in rows), sum(r[2] for r in rows)))
