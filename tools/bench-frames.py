"""Slow frames in The Talos Principle's own frame-time tables.

run-talos.ps1 passes +bmk_bSaveFrameTimes 1, and the game writes every frame
time of the measured 30 s to Temp\\BenchFrames_<n>.csv in its folder. This
lists, per table, the frames over twice the median, when they happen, and how
many of the gaps between them are ~5 s (the game's own periodic hitch, present
under DX11 and Vulkan alike).

  python tools/bench-frames.py [glob] [count]
  default glob: the game's Temp\\BenchFrames_*.csv; count: the newest 10
"""
import datetime
import glob
import os
import statistics
import sys

pattern = sys.argv[1] if len(sys.argv) > 1 else \
    r'C:\Program Files (x86)\Steam\steamapps\common\The Talos Principle\Temp\BenchFrames_*.csv'
count = int(sys.argv[2]) if len(sys.argv) > 2 else 10
for path in sorted(glob.glob(pattern), key=os.path.getmtime)[-count:]:
    ms = [float(line.split(',')[1]) for line in open(path) if ',' in line]
    if not ms:
        continue
    median = statistics.median(ms)
    t, slow = 0.0, []
    for value in ms:
        if value > 2 * median:
            slow.append((t, value))
        t += value
    gaps = [b[0] - a[0] for a, b in zip(slow, slow[1:])]
    periodic = sum(1 for g in gaps if 4700 <= g <= 5300)
    when = datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime('%Y-%m-%d %H:%M:%S')
    print(f'{os.path.basename(path)} ({when}): {len(ms)} frames, median {median:.1f} ms, {len(slow)} over {2 * median:.1f} ms, '
          f'{periodic} of {len(gaps)} gaps 4.7-5.3 s')
    print('   ' + ' '.join(f'{s / 1000:.2f}s:{v:.0f}ms' for s, v in slow[:16]))
