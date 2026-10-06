#!/usr/bin/env python3
"""idle.py <fifo idle csv>: share of the render thread's time per FIFO state over the second half of an
RPCS3_FIFO_IDLE_TRACE=1 run (spans under 10 us are not rows; their total is in the last columns)."""
import sys, csv, collections
rows = [r for r in csv.DictReader(open(sys.argv[1])) if r['completed'] == '1']
half = rows[len(rows) // 2:]
span = int(half[-1]['cpu_end_ns']) - int(half[0]['cpu_begin_ns'])
d = collections.Counter(); n = collections.Counter()
for r in half:
    d[r['label']] += int(r['cpu_end_ns']) - int(r['cpu_begin_ns']); n[r['label']] += 1
print('span %.2f s, %d rows' % (span / 1e9, len(half)))
for k, v in d.most_common(): print('%-16s %5.1f%% of time, %d spans, mean %.0f us' % (k, v / span * 100, n[k], v / n[k] / 1e3))
short = int(half[-1]['short_duration_ns_total']) - int(half[0]['short_duration_ns_total'])
print('short spans: %.1f%% of time' % (short / span * 100), 'dropped', half[-1]['dropped_total'])
