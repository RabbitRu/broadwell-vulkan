"""Frame timeline report for HASVK_WDDM_TIMELINE=<file.csv> (anv_wddm_profile.c).

Answers where a frame's time goes when neither the CPU nor the GPU is busy:
GPU busy and idle time from the GPU's own timestamps of every kernel
submission, each idle gap split by what the threads were doing during it, the
application thread's time inside blocking Vulkan calls and its CPU time, the
queue thread's work and the completion thread's latency.

  python tools/timeline-report.py timeline.csv [--talos-log Talos.log] [--from S --to S]

--talos-log takes the measured window from the game's log ("Starting automated
benchmark", 30 s); otherwise --from/--to are seconds after the first event.
"""
import argparse
import bisect
import collections
import csv
import datetime
import re
import statistics
import sys

CALL_BEGIN, CALL_END, SUBMIT_BEGIN, SUBMIT_WAITED, KERNEL_BEGIN, KERNEL_END, GPU_DONE, SIGNALED, CALIBRATE = range(1, 10)
CALLS = {1: 'vkQueueSubmit', 2: 'vkQueuePresentKHR', 3: 'vkAcquireNextImageKHR', 4: 'vkWaitForFences', 5: 'vkGetFenceStatus',
         6: 'vkGetQueryPoolResults', 7: 'vkQueueWaitIdle', 8: 'vkDeviceWaitIdle', 9: 'vkAllocateMemory', 10: 'vkMapMemory',
         11: 'vkResetFences', 12: 'vkWaitSemaphores', 13: 'vkCreate*Pipelines', 14: 'vkFreeMemory', 15: 'vkResetCommandPool'}
PRESENT = 2
GPU_TICK_NS = 80.0   # Gen8 TIMESTAMP: 12.5 MHz


def filetime_to_datetime(value):
    return datetime.datetime(1601, 1, 1) + datetime.timedelta(microseconds=value / 10)


def load(path):
    with open(path, newline='') as f:
        rows = list(csv.reader(f))
    return [(int(r[0]), int(r[1]), int(r[2]), int(r[3]), int(r[4])) for r in rows[1:] if r]


def fit_line(points):
    """Least squares y = a + b x."""
    n = len(points)
    if n < 2:
        return (points[0][1] if points else 0.0), 0.0
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    sxx = sum((p[0] - mx) ** 2 for p in points)
    b = sum((p[0] - mx) * (p[1] - my) for p in points) / sxx if sxx else 0.0
    return my - b * mx, b


def intervals_of(events, thread, begin_type, end_type, key=None):
    """(start, end, kind) for begin/end pairs of one thread (nesting by kind)."""
    out, open_ = [], {}
    for ns, t, th, a, b in events:
        if th != thread:
            continue
        if t == begin_type and (key is None or key(a)):
            open_.setdefault(a, []).append(ns)
        elif t == end_type and a in open_ and open_[a]:
            out.append((open_[a].pop(), ns, a))
    out.sort()
    return out


def overlap(intervals, starts, lo, hi):
    """Sum of overlap of sorted, non-overlapping-ish intervals with [lo, hi], by kind."""
    total = collections.Counter()
    i = max(0, bisect.bisect_right(starts, lo) - 1)
    while i < len(intervals) and intervals[i][0] < hi:
        s, e, k = intervals[i]
        o = min(e, hi) - max(s, lo)
        if o > 0:
            total[k] += o
        i += 1
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--talos-log')
    ap.add_argument('--from', dest='t0', type=float)
    ap.add_argument('--to', dest='t1', type=float)
    args = ap.parse_args()
    ev = load(args.csv)
    ev.sort(key=lambda e: e[0])
    cal = [e for e in ev if e[1] == CALIBRATE]
    first_ns = ev[0][0]
    tsc_hz = (cal[-1][3] - cal[0][3]) / ((cal[-1][0] - cal[0][0]) / 1e9) if len(cal) >= 2 else 2.7e9
    wall0 = filetime_to_datetime(cal[0][4]) - datetime.timedelta(microseconds=(cal[0][0] - first_ns) / 1e3)

    if args.talos_log:
        text = open(args.talos_log, encoding='utf-8', errors='replace').read()
        m = re.search(r'^(\d\d):(\d\d):(\d\d) INF:\s+Starting automated benchmark', text, re.M)
        if not m:
            sys.exit('no "Starting automated benchmark" in the log')
        start = wall0.replace(hour=int(m.group(1)), minute=int(m.group(2)), second=int(m.group(3)), microsecond=0)
        if start < wall0 - datetime.timedelta(hours=12):
            start += datetime.timedelta(days=1)
        t0 = (start - wall0).total_seconds() + 2.0   # the log has whole seconds
        t1 = t0 + 26.0
    else:
        t0 = args.t0 if args.t0 is not None else 0.0
        t1 = args.t1 if args.t1 is not None else (ev[-1][0] - first_ns) / 1e9
    lo, hi = first_ns + int(t0 * 1e9), first_ns + int(t1 * 1e9)

    presents = collections.Counter(e[2] for e in ev if e[1] == CALL_BEGIN and e[3] == PRESENT)
    if not presents:
        sys.exit('no presents in the timeline')
    app = presents.most_common(1)[0][0]
    submit_thread = collections.Counter(e[2] for e in ev if e[1] == KERNEL_BEGIN).most_common(1)[0][0]
    completion_thread = collections.Counter(e[2] for e in ev if e[1] == GPU_DONE).most_common(1)[0][0]

    # Kernel submissions by serial.
    sub = {}
    for ns, t, th, a, b in ev:
        if t == KERNEL_BEGIN:
            sub.setdefault(a, {})['kb'] = ns; sub[a]['present'] = b
        elif t == KERNEL_END:
            sub.setdefault(a, {})['ke'] = ns; sub[a]['ke_cycles'] = b
        elif t == GPU_DONE:
            sub.setdefault(a, {})['done'] = ns; sub[a]['gs'] = b & 0xffffffff; sub[a]['ge'] = b >> 32
        elif t == SIGNALED:
            sub.setdefault(a, {})['sig'] = ns
    serials = sorted(s for s, v in sub.items() if all(k in v for k in ('kb', 'ke', 'done', 'gs', 'ge')))
    # Unwrap the 32-bit tick counter (wraps every 343 s at 12.5 MHz).
    wrap, prev = 0, None
    for s in serials:
        v = sub[s]
        if prev is not None and v['gs'] + wrap < prev - (1 << 31):
            wrap += 1 << 32
        v['gs_ns'] = (v['gs'] + wrap) * GPU_TICK_NS
        e = v['ge'] + wrap
        if e < v['gs'] + wrap - (1 << 31):
            e += 1 << 32
        v['ge_ns'] = e * GPU_TICK_NS
        prev = v['gs'] + wrap
    # GPU -> CPU clock: cpu = gpu + a + b * gpu. Upper bound per submission:
    # the completion thread saw the end after it happened; lower bound: the GPU
    # cannot start before the kernel call began. Fit the 1-s bucket extremes.
    upper = collections.defaultdict(lambda: float('inf'))
    lower = collections.defaultdict(lambda: -float('inf'))
    for s in serials:
        v = sub[s]
        bucket = int((v['done'] - first_ns) // 1e9)
        upper[bucket] = min(upper[bucket], v['done'] - v['ge_ns'])
        lower[bucket] = max(lower[bucket], v['kb'] - v['gs_ns'])
    def mid_gpu(bucket):
        return statistics.median(sub[s]['ge_ns'] for s in serials if int((sub[s]['done'] - first_ns) // 1e9) == bucket)
    up_line = fit_line([(mid_gpu(k), u) for k, u in upper.items()])
    lo_line = fit_line([(mid_gpu(k), l) for k, l in lower.items()])
    def to_cpu(g, line=up_line):
        return g + line[0] + line[1] * g
    for s in serials:
        v = sub[s]
        v['gs_cpu'] = to_cpu(v['gs_ns']); v['ge_cpu'] = to_cpu(v['ge_ns'])
    spread = statistics.median(to_cpu(sub[s]['ge_ns']) - to_cpu(sub[s]['ge_ns'], lo_line) for s in serials)

    win = [s for s in serials if lo <= sub[s]['gs_cpu'] < hi]
    frames = [e[0] for e in ev if e[2] == app and e[1] == CALL_BEGIN and e[3] == PRESENT and lo <= e[0] < hi]
    frame_cycles = [(e[0], e[4]) for e in ev if e[2] == app and e[1] == CALL_BEGIN and e[3] == PRESENT and lo <= e[0] < hi]
    if len(frames) < 3 or len(win) < 3:
        sys.exit(f'window {t0:.1f}-{t1:.1f} s holds {len(frames)} frames and {len(win)} submissions')
    span = frames[-1] - frames[0]
    nframes = len(frames) - 1
    per_frame = lambda ns: ns / nframes / 1e6

    print(f'window {t0:.1f}-{t1:.1f} s after the first event ({wall0 + datetime.timedelta(seconds=t0):%H:%M:%S}), '
          f'{nframes} frames, {nframes / (span / 1e9):.1f} FPS, frame {span / nframes / 1e6:.2f} ms')
    ft = [(b - a) / 1e6 for a, b in zip(frames, frames[1:])]
    print(f'frame time: median {statistics.median(ft):.2f} ms, p90 {sorted(ft)[int(0.9 * len(ft))]:.2f}, '
          f'p99 {sorted(ft)[int(0.99 * len(ft))]:.2f}, max {max(ft):.1f}')
    print(f'threads: application {app}, queue {submit_thread}, completion {completion_thread}; TSC {tsc_hz / 1e9:.3f} GHz; '
          f'GPU clock fit drift {up_line[1] * 1e6:+.1f} ppm, completion-vs-submit bound spread {spread / 1e3:.0f} us')

    # GPU busy and gaps (submissions run in order on the one engine).
    busy = sum(sub[s]['ge_cpu'] - sub[s]['gs_cpu'] for s in win)
    n_present = sum(1 for s in win if sub[s]['present'])
    print(f'\nGPU: busy {per_frame(busy):.2f} ms/frame ({busy / span * 100:.1f} %), '
          f'{len(win) / nframes:.2f} kernel submissions/frame ({n_present / nframes:.2f} presents)')

    app_calls = intervals_of(ev, app, CALL_BEGIN, CALL_END)
    app_starts = [i[0] for i in app_calls]
    sem_waits = []   # queue thread: SUBMIT_BEGIN..SUBMIT_WAITED
    q_busy = []      # queue thread: SUBMIT_BEGIN..KERNEL_END
    last_begin = None
    for ns, t, th, a, b in ev:
        if th != submit_thread:
            continue
        if t == SUBMIT_BEGIN:
            last_begin = ns
        elif t == SUBMIT_WAITED and last_begin is not None:
            sem_waits.append((last_begin, ns, 'semaphore wait'))
        elif t == KERNEL_BEGIN:
            q_busy.append([ns, None, 'kernel submission'])
        elif t == KERNEL_END and q_busy and q_busy[-1][1] is None:
            q_busy[-1][1] = ns
    q_busy = [tuple(x) for x in q_busy if x[1] is not None]
    q_all = sorted(sem_waits + q_busy)
    q_starts = [i[0] for i in q_all]

    gap_cause = collections.Counter()
    gap_app = collections.Counter()
    gap_hist = collections.Counter()
    gaps_total = 0.0
    for prev_s, next_s in zip(win, win[1:]):
        p, n = sub[prev_s], sub[next_s]
        g0, g1 = p['ge_cpu'], n['gs_cpu']
        if g1 <= g0:
            continue
        gaps_total += g1 - g0
        gap_hist['<50 us' if g1 - g0 < 5e4 else '50-200 us' if g1 - g0 < 2e5 else '0.2-1 ms' if g1 - g0 < 1e6 else '>1 ms'] += g1 - g0
        # Before the next kernel call began: nothing was submitted.
        late_end = min(g1, n['kb'])
        if late_end > g0:
            q = overlap(q_all, q_starts, g0, late_end)
            q_time = sum(q.values())
            for k, v in q.items():
                gap_cause[f'queue thread: {k}'] += v
            idle = (late_end - g0) - q_time
            if idle > 0:
                gap_cause['queue thread idle (no vkQueueSubmit yet)'] += idle
                # what the application thread did meanwhile (approximate: whole late span)
                a_ = overlap(app_calls, app_starts, g0, late_end)
                in_calls = sum(a_.values())
                frac = idle / (late_end - g0)
                for k, v in a_.items():
                    gap_app[CALLS.get(k, str(k))] += v * frac
                gap_app['application code (outside these calls)'] += ((late_end - g0) - in_calls) * frac
        if g1 > max(g0, n['kb']):
            gap_cause['kernel call to GPU start'] += g1 - max(g0, n['kb'])
    print(f'GPU idle between submissions: {per_frame(gaps_total):.2f} ms/frame; by gap length: ' +
          ', '.join(f'{k} {per_frame(v):.2f}' for k, v in sorted(gap_hist.items())))
    for k, v in gap_cause.most_common():
        print(f'  {per_frame(v):6.2f} ms/frame  {k}')
    if gap_app:
        print('  while the queue thread had nothing to submit, the application thread was in:')
        for k, v in gap_app.most_common():
            print(f'  {per_frame(v):6.2f} ms/frame    {k}')

    # Application thread.
    print('\napplication thread:')
    if len(frame_cycles) >= 2:
        cpu = (frame_cycles[-1][1] - frame_cycles[0][1]) / tsc_hz * 1e9
        print(f'  CPU time {per_frame(cpu):.2f} ms/frame ({cpu / (frame_cycles[-1][0] - frame_cycles[0][0]) * 100:.1f} % of wall time)')
    in_win = [c for c in app_calls if lo <= c[0] < hi]
    by = collections.defaultdict(list)
    for s, e, k in in_win:
        by[k].append(e - s)
    for k, d in sorted(by.items(), key=lambda kv: -sum(kv[1])):
        print(f'  {per_frame(sum(d)):6.3f} ms/frame in {CALLS.get(k, k)}: {len(d) / nframes:.1f} calls/frame, '
              f'median {statistics.median(d) / 1e3:.0f} us, max {max(d) / 1e6:.2f} ms')

    # Queue thread and completion thread.
    qb = [(s, e) for s, e, _ in q_busy if lo <= s < hi]
    sw = [(s, e) for s, e, _ in sem_waits if lo <= s < hi]
    print('\nqueue thread:')
    print(f'  kernel submissions: {per_frame(sum(e - s for s, e in qb)):.2f} ms/frame '
          f'(median {statistics.median([e - s for s, e in qb]) / 1e3:.0f} us)')
    print(f'  CPU semaphore waits: {per_frame(sum(e - s for s, e in sw)):.2f} ms/frame')
    presents_k = [sub[s]['ke'] - sub[s]['kb'] for s in win if sub[s]['present']]
    others_k = [sub[s]['ke'] - sub[s]['kb'] for s in win if not sub[s]['present']]
    if presents_k:
        print(f'  kernel call: present median {statistics.median(presents_k) / 1e3:.0f} us, others median '
              f'{statistics.median(others_k) / 1e3 if others_k else 0:.0f} us')
    sig = [sub[s]['sig'] - sub[s]['done'] for s in win if 'sig' in sub[s]]
    ahead = []
    for f in frames:
        ahead.append(sum(1 for s in win if sub[s]['kb'] <= f < sub[s]['ge_cpu']))
    print('\ncompletion (times relative to the clock fit):')
    for name, want in (('presents', 1), ('other submissions', 0)):
        group = [s for s in win if bool(sub[s]['present']) == bool(want)]
        if not group:
            continue
        lat = sorted(sub[s]['done'] - sub[s]['ge_cpu'] for s in group)
        start = sorted(sub[s]['gs_cpu'] - sub[s]['kb'] for s in group)
        run = sorted(sub[s]['ge_cpu'] - sub[s]['gs_cpu'] for s in group)
        print(f'  {name}: kernel call begin -> GPU start median {statistics.median(start) / 1e3:.0f} us; '
              f'GPU run median {statistics.median(run) / 1e3:.0f} us, {per_frame(sum(run)):.2f} ms/frame; '
              f'GPU end -> completion thread awake median {statistics.median(lat) / 1e3:.0f} us, p90 {lat[int(0.9 * len(lat))] / 1e3:.0f} us')
    if sig:
        print(f'  awake -> syncs signalled: median {statistics.median(sig) / 1e3:.0f} us, p90 {sorted(sig)[int(0.9 * len(sig))] / 1e3:.0f} us')
    print(f'  submissions in flight at each present: ' +
          ', '.join(f'{k}: {v}' for k, v in sorted(collections.Counter(ahead).items())))

    # Slow frames: present to present over twice the median. Per frame: the
    # application thread's CPU time (thread cycles at each present), the rest
    # of the frame off the CPU, its long Vulkan calls, the GPU's busy time and
    # the longest kernel call of the queue thread.
    median = statistics.median(ft)
    cycles = dict(frame_cycles)
    slow = [(a, b) for a, b in zip(frames, frames[1:]) if b - a > 2 * median * 1e6]
    print(f'\nslow frames (over {2 * median:.1f} ms): {len(slow)} of {nframes}')
    sum_extra = sum_cpu_extra = sum_calls = 0.0
    call_kinds = collections.Counter()
    median_cpu = statistics.median((cycles[b] - cycles[a]) / tsc_hz * 1e3 for a, b in zip(frames, frames[1:]) if a in cycles and b in cycles)
    for a, b in slow:
        dt = (b - a) / 1e6
        cpu = (cycles[b] - cycles[a]) / tsc_hz * 1e3 if a in cycles and b in cycles else float('nan')
        calls = overlap(app_calls, app_starts, a, b)
        long_calls = {CALLS.get(k, k): v / 1e6 for k, v in calls.items() if v > 2e5}
        gpu = sum(max(0, min(sub[s]['ge_cpu'], b) - max(sub[s]['gs_cpu'], a)) for s in win) / 1e6
        kernel = max([(sub[s]['ke'] - sub[s]['kb']) / 1e6 for s in win if a <= sub[s]['kb'] < b] or [0])
        sum_extra += dt - median; sum_cpu_extra += cpu - median_cpu
        for k, v in calls.items():
            call_kinds[CALLS.get(k, k)] += v / 1e6
        if len(slow) <= 40 or dt > 4 * median:
            print(f'  {dt:6.1f} ms: app CPU {cpu:5.1f} ms, off CPU {dt - cpu:5.1f} ms, GPU busy {gpu:5.1f} ms, '
                  f'longest kernel call {kernel:4.1f} ms' + (f'; calls ' + ', '.join(f'{k} {v:.1f} ms' for k, v in long_calls.items()) if long_calls else ''))
    if slow:
        print(f'  over the median frame ({median:.1f} ms, app CPU {median_cpu:.1f} ms): {sum_extra / len(slow):.1f} ms per slow frame, '
              f'of it {sum_cpu_extra / len(slow):.1f} ms more application CPU; time in calls per slow frame: ' +
              ', '.join(f'{k} {v / len(slow):.2f} ms' for k, v in call_kinds.most_common(5)))


if __name__ == '__main__':
    main()
