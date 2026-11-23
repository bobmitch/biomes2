"""How strongly a terrain still repeats at the ground scan's tile.

    python -I tools/profile/tiling.py <still.bmp> <metres above the ground> [tile_m 2.5] [vfov_deg 60]

For a still looking STRAIGHT DOWN (--cam x y z 0 -1.5533), where screen pixels
are world metres: the tile's period in pixels is tile_m / (2 h tan(vfov/2) /
height_px). The central square is high-passed (a box blur two periods wide
taken off, so lighting, the macro variation and slopes do not count), then its
normalised autocorrelation is read at the tile's lattice lags -- (P, 0), (0, P),
(P, P), (P, -P), (2P, 0), (0, 2P) -- and, for a baseline, at half-period lags,
where a repeating texture has no reason to correlate.

    tile correlation ~0: nothing repeats at the tile; towards 1: the grid shows.

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


def at(ac, dy, dx):
    """Bilinear read of a centred autocorrelation at a fractional lag."""
    cy, cx = ac.shape[0] // 2, ac.shape[1] // 2
    y, x = cy + dy, cx + dx
    y0, x0 = int(math.floor(y)), int(math.floor(x))
    fy, fx = y - y0, x - x0
    return ((1 - fy) * (1 - fx) * ac[y0, x0] + (1 - fy) * fx * ac[y0, x0 + 1] +
            fy * (1 - fx) * ac[y0 + 1, x0] + fy * fx * ac[y0 + 1, x0 + 1])


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path, h = sys.argv[1], float(sys.argv[2])
    tile = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
    vfov = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0
    im = np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255.0
    H, W = im.shape[:2]
    lum = im @ np.array([0.2126, 0.7152, 0.0722])
    P = tile / (2.0 * h * math.tan(math.radians(vfov) / 2.0) / H)
    n = min(H, W, 512)
    crop = lum[(H - n) // 2:(H + n) // 2, (W - n) // 2:(W + n) // 2]
    hp = crop - box(crop, max(1, int(round(P))))
    hp -= hp.mean()
    f = np.fft.fft2(hp, s=(2 * n, 2 * n))
    ac = np.fft.fftshift(np.real(np.fft.ifft2(f * np.conj(f))))
    ac /= ac[n, n]
    lattice = [(0, P), (P, 0), (P, P), (P, -P), (0, 2 * P), (2 * P, 0)]
    half = [(0, 0.5 * P), (0.5 * P, 0), (0.5 * P, 0.5 * P), (0.5 * P, -0.5 * P), (0, 1.5 * P), (1.5 * P, 0)]
    c = np.mean([at(ac, dy, dx) for dy, dx in lattice])
    b = np.mean([at(ac, dy, dx) for dy, dx in half])
    print('%s: %.1f m up, tile %.2f m = %.1f px; tile correlation %.3f (half-period baseline %.3f)' %
          (path, h, tile, P, c, b))


if __name__ == '__main__':
    main()
