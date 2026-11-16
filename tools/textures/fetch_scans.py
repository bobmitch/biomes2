"""Download the CC0 scan sets in assets/textures/scans.json and pack them for the engine.

    python -I tools/textures/fetch_scans.py            # fetch what is missing, pack what changed
    python -I tools/textures/fetch_scans.py --repack   # re-pack every set from the downloads
    python -I tools/textures/fetch_scans.py --record   # write each download's sha256 into scans.json

Run from the repository root. Needs numpy and Pillow.

Each set becomes assets/textures/packed/<id>.bin, read by engine/render/terrain_textures.cpp:

    magic 'SCN1', u32 size, u32 mips, f32 mean linear albedo r g b, f32 tile_m
    then, for each mip from the largest: size_m^2 RGBA8 of A (sRGB albedo, linear height in alpha)
    then, for each mip: size_m^2 RGBA8 of B (normal x, normal y as 0.5 + 0.5n, roughness, AO)

The normal is OpenGL-convention (+y up the texture), tangent space. Albedo mips average in
linear light; normal mips renormalise. Height is stretched to the set's own 0..1 range.

The zip is untrusted data: only members whose names end in a known map suffix are read, by
name, into memory, and nothing from the archive is written to disk under its own path.
"""
import hashlib
import io
import json
import os
import struct
import sys
import urllib.request
import zipfile

import numpy as np
from PIL import Image

ROOT = os.getcwd()
MANIFEST = os.path.join(ROOT, 'assets', 'textures', 'scans.json')
DOWNLOAD = os.path.join(ROOT, 'assets', 'textures', 'download')
PACKED = os.path.join(ROOT, 'assets', 'textures', 'packed')
MAPS = {'color': '_Color.jpg', 'normal': '_NormalGL.jpg', 'rough': '_Roughness.jpg',
        'ao': '_AmbientOcclusion.jpg', 'height': '_Displacement.jpg'}


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def down2(a):
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def load(z, suffix, size, mode):
    names = [n for n in z.namelist() if n.endswith(suffix) and '/' not in n.strip('/')]
    if not names:
        return None
    with z.open(names[0]) as f:
        data = f.read()
    im = Image.open(io.BytesIO(data))
    im.load()
    im = im.convert(mode)
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / (65535.0 if mode == 'I;16' else 255.0)


def pack(set_id, zip_path, tile_m, out_path):
    with zipfile.ZipFile(zip_path) as z:
        color = load(z, MAPS['color'], 1024, 'RGB')
        if color is None:
            raise RuntimeError(f'{set_id}: no colour map in the archive')
        size = color.shape[0]
        normal = load(z, MAPS['normal'], size, 'RGB')
        rough = load(z, MAPS['rough'], size, 'L')
        ao = load(z, MAPS['ao'], size, 'L')
        height = load(z, MAPS['height'], size, 'L')
    lin = srgb_to_linear(color)
    n = normal * 2.0 - 1.0 if normal is not None else np.dstack([np.zeros((size, size)), np.zeros((size, size)), np.ones((size, size))])
    rough = rough if rough is not None else np.full((size, size), 0.8)
    ao = ao if ao is not None else np.ones((size, size))
    if height is None:
        height = np.full((size, size), 0.5)
    else:
        lo, hi = np.percentile(height, 0.5), np.percentile(height, 99.5)
        height = np.clip((height - lo) / max(hi - lo, 1e-6), 0.0, 1.0)
    mean = lin.reshape(-1, 3).mean(axis=0)
    mips = int(np.log2(size)) + 1
    a_chain, b_chain = [], []
    for m in range(mips):
        if m > 0:
            lin, n, rough, ao, height = down2(lin), down2(n), down2(rough), down2(ao), down2(height)
            n = n / np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
        a = np.dstack([linear_to_srgb(lin), height])
        b = np.dstack([n[..., 0] * 0.5 + 0.5, n[..., 1] * 0.5 + 0.5, rough, ao])
        a_chain.append(np.round(np.clip(a, 0, 1) * 255.0).astype(np.uint8).tobytes())
        b_chain.append(np.round(np.clip(b, 0, 1) * 255.0).astype(np.uint8).tobytes())
    tmp = out_path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'SCN1')
        f.write(struct.pack('<II4f', size, mips, float(mean[0]), float(mean[1]), float(mean[2]), float(tile_m)))
        for c in a_chain:
            f.write(c)
        for c in b_chain:
            f.write(c)
    os.replace(tmp, out_path)
    return mean


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    repack = '--repack' in sys.argv
    record = '--record' in sys.argv
    with open(MANIFEST, encoding='utf-8') as f:
        manifest = json.load(f)
    res = manifest['resolution']
    os.makedirs(DOWNLOAD, exist_ok=True)
    os.makedirs(PACKED, exist_ok=True)
    total = 0
    for set_id, info in manifest['sets'].items():
        if not set_id.replace('_', '').isalnum():
            raise RuntimeError(f'bad set id {set_id!r}')
        zip_path = os.path.join(DOWNLOAD, f'{set_id}_{res}-JPG.zip')
        if not os.path.exists(zip_path):
            url = f'https://ambientcg.com/get?file={set_id}_{res}-JPG.zip'
            req = urllib.request.Request(url, headers={'User-Agent': 'texture-fetch/1.0'})
            with urllib.request.urlopen(req, timeout=120) as r, open(zip_path + '.part', 'wb') as f:
                while True:
                    block = r.read(1 << 20)
                    if not block:
                        break
                    f.write(block)
            os.replace(zip_path + '.part', zip_path)
            print(f'downloaded {set_id} ({os.path.getsize(zip_path) / 1e6:.1f} MB)')
        total += os.path.getsize(zip_path)
        digest = sha256(zip_path)
        if record:
            info['sha256'] = digest
        elif info.get('sha256') and info['sha256'] != digest:
            raise RuntimeError(f'{set_id}: sha256 {digest} does not match the manifest')
        out = os.path.join(PACKED, f'{set_id}.bin')
        if repack or not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(zip_path):
            mean = pack(set_id, zip_path, info['tile_m'], out)
            print(f'packed {set_id}: mean linear albedo {mean[0]:.3f} {mean[1]:.3f} {mean[2]:.3f}')
    if record:
        with open(MANIFEST, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')
    print(f'{len(manifest["sets"])} sets, {total / 1e6:.0f} MB downloaded, packs in {PACKED}')


if __name__ == '__main__':
    main()
