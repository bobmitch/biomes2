"""How strongly a scatter (grass, rocks, anything placed per cell) sits on a lattice.

    python -I tools/profile/scatter.py <still.bmp> <metres above the ground> [cell_m 1.0] [vfov_deg 60]

tiling.py answers a different question: it reads the autocorrelation at a
texture's tile lags, which asks whether the same CONTENT repeats. A scatter
placed one-or-more-per-cell repeats no content at all -- every cell holds
different blades -- so that correlation is ~0 whether or not the spacing is a
grid, and it cannot see the thing the eye sees.

What the eye sees is the SPACING. A scatter with a fixed count per cell is a
jittered grid: its power spectrum has peaks on the axes at the cell frequency
(and its harmonics), which is what reads as rows. A natural (Poisson-ish)
scatter has no peak there.

For a still looking STRAIGHT DOWN (--cam x y z 0 -1.5533), where screen pixels
are world metres, the central square is high-passed, windowed and Fourier
transformed. Printed, as a ratio of the mean power in a ring of the same
radius:

    axis peak   -- the mean of the four (+-f, 0), (0, +-f) peaks: rows and
                   columns on the world axes, the lattice the cells are on.
    ring        -- the same at the diagonals, for scale.
    1 means the lattice frequency carries no more energy than its neighbours
    at the same scale: nothing on a grid. Above ~1.5 it shows.

Run from the repository root. Needs numpy and Pillow.
"""
import math
import sys

import numpy as np
from PIL import Image


def box(a, r):
    """Mean over a (2r+1)^2 window, edges clamped, by cumulative sums."""
    p = np.pad(a, r + 1, mode='edge')
    c = p.cumsum(0).cumsum(1)
    n = 2 * r + 1
    s = c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]
    return s[:a.shape[0], :a.shape[1]] / (n * n)


def peak(power, cy, cx, dy, dx, rad=2):
    """The largest power in a small disc about a lag, so a fractional period
    and the window's own spread do not miss it."""
    y, x = int(round(cy + dy)), int(round(cx + dx))
    return power[y - rad:y + rad + 1, x - rad:x + rad + 1].max()


def main(path, height_m, cell_m=1.0, vfov_deg=60.0):
    im = np.asarray(Image.open(path).convert('L'), dtype=np.float64) / 255.0
    h, w = im.shape
    mpp = 2.0 * height_m * math.tan(math.radians(vfov_deg) * 0.5) / h
    period = cell_m / mpp                      # the cell, in pixels

    n = int(min(h, w) * 0.7) // 2 * 2          # a centred square, even sided
    y0, x0 = (h - n) // 2, (w - n) // 2
    a = im[y0:y0 + n, x0:x0 + n]
    # High-pass at two cells, so lighting, the ground's own colour and the
    # slope do not enter, then a Hann window so the transform has no edge
    # cross through the middle of the spectrum.
    a = a - box(a, max(int(period), 2))
    win = np.hanning(n)
    a = a * win[:, None] * win[None, :]

    power = np.abs(np.fft.fftshift(np.fft.fft2(a))) ** 2
    cy = cx = n // 2
    f = n / period                             # the cell's frequency, in bins

    axis = 0.25 * (peak(power, cy, cx, f, 0) + peak(power, cy, cx, -f, 0) +
                   peak(power, cy, cx, 0, f) + peak(power, cy, cx, 0, -f))
    d = f / math.sqrt(2.0)
    diag = 0.25 * (peak(power, cy, cx, d, d) + peak(power, cy, cx, d, -d) +
                   peak(power, cy, cx, -d, d) + peak(power, cy, cx, -d, -d))

    # The ring's own mean, excluding the axes themselves: what the same scale
    # carries when nothing is lined up.
    yy, xx = np.mgrid[:n, :n]
    r = np.hypot(yy - cy, xx - cx)
    ring = (np.abs(r - f) <= max(f * 0.12, 3.0))
    ring &= (np.abs(yy - cy) > 4) & (np.abs(xx - cx) > 4)
    base = power[ring].mean()

    print('%s: %.1f m up, cell %.2f m = %.1f px; axis peak %.2f, diagonal %.2f '
          '(1 = nothing on the lattice)' % (path, height_m, cell_m, period,
                                            axis / base, diag / base))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        raise SystemExit(2)
    main(sys.argv[1], float(sys.argv[2]),
         float(sys.argv[3]) if len(sys.argv) > 3 else 1.0,
         float(sys.argv[4]) if len(sys.argv) > 4 else 60.0)
