#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Reads a WAYLAND_DEBUG=client log of an interactive resize of Kinoklip's main window (stderr of
# `WAYLAND_DEBUG=client kinoklip`, or scripts/resize_sweep.sh with KK_SWEEP_LOG) and prints:
#   - configures received / applied (acked) and how many sizes the compositor's configures
#     skipped, the time per applied size;
#   - EGL frames (buffer swaps) per applied size;
#   - how many applied sizes first showed a buffer at the previous size (content cut, or a
#     black band next to the window frame) and how long until the buffer at the right size.
# Usage: scripts/wayland_resize_trace.py trace.log [minimum width of the gesture, default 0]
import re
import statistics
import sys


def ms(stamp):
    """[hh:mm:ss.micro] -> milliseconds."""
    return sum(float(x) * m for x, m in zip(stamp.split(':'), (3600000, 60000, 1000)))


def main():
    path = sys.argv[1]
    min_width = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    sizes = {}  # wl_buffer id -> (w, h), EGL buffers
    pending = {}  # configure serial -> size
    last_conf = None
    attached = None
    configures = 0
    rows = []  # [size, ack ms, first commit ms, first stale, right-size commit ms, swaps]
    for line in open(path, errors='replace'):
        m = re.match(r'\[(\d+:\d+:[\d.]+)\]', line)
        if not m:
            continue
        ts = ms(m.group(1))
        c = re.search(r'xdg_toplevel#\d+\.configure\((\d+), (\d+)', line)
        if c:
            last_conf = (int(c.group(1)), int(c.group(2)))
            if last_conf[0] > min_width:
                configures += 1
            continue
        s = re.search(r'xdg_surface#\d+\.configure\((\d+)\)', line)
        if s and last_conf:
            pending[s.group(1)] = last_conf
            continue
        a = re.search(r'ack_configure\((\d+)\)', line)
        if a and a.group(1) in pending and pending[a.group(1)][0] > min_width:
            rows.append([pending[a.group(1)], ts, None, None, None, 0])
            continue
        # wl_shm (software EGL) or linux-dmabuf (GPU) buffers and their sizes
        b = re.search(r'create_buffer\(new id wl_buffer#(\d+), 0, (\d+), (\d+)', line) or re.search(
            r'create_immed\(new id wl_buffer#(\d+), (\d+), (\d+)', line)
        if b and 'mesa' in line:
            sizes[b.group(1)] = (int(b.group(2)), int(b.group(3)))
            continue
        at = re.search(r'wl_surface#\d+\.attach\(wl_buffer#(\d+)', line)
        if at and 'mesa' in line:
            attached = sizes.get(at.group(1))
            continue
        if 'mesa' in line and '.commit()' in line and rows:
            row = rows[-1]
            row[5] += 1
            ok = attached is None or attached == row[0]  # unknown size (dmabuf): assume right
            if row[2] is None:
                row[2] = ts
                row[3] = not ok
            if ok and row[4] is None:
                row[4] = ts
    if not rows:
        print('no applied configures found')
        return
    steps = sorted(b[1] - a[1] for a, b in zip(rows, rows[1:]))
    stale = [r for r in rows if r[3]]
    fixed = [r[4] - r[2] for r in stale if r[4] is not None]
    print('configures %d, applied %d (%.0f%% skipped), time per applied size median %.1f ms, '
          'max %.1f ms' % (configures, len(rows), 100.0 * (configures - len(rows)) / max(configures, 1),
                           steps[len(steps) // 2] if steps else 0, steps[-1] if steps else 0))
    print('EGL frames per applied size: %.1f' % (sum(r[5] for r in rows) / len(rows)))
    print('first frame at the previous size: %d of %d; until the right size: median %.1f ms, '
          'max %.1f ms; not corrected before the next size: %d'
          % (len(stale), len(rows), statistics.median(fixed) if fixed else 0,
             max(fixed) if fixed else 0, sum(1 for r in stale if r[4] is None)))


if __name__ == '__main__':
    main()
