"""Pack the rigged glTF characters (plus hair on the same skeleton) and the animation library.

    python -I tools/models/pack_character.py            # every character in CHARACTERS, and the clips
    python -I tools/models/pack_character.py --list     # print each source's joints in rest pose

Run from the repository root. Needs numpy and Pillow. Reads the Quaternius "Universal Base
Characters" kit (CC0) from assets/models/, which is not in git; writes
assets/models/packed/<name>.chr, read by engine/character/character_model.cpp:

    magic 'CHR1', u32 version (1), u32 joints, u32 vertices, u32 indices, u32 materials,
    u32 tex_size, u32 tex_mips
    f32[16] root: the skeleton's non-joint ancestors, row-major, p' = M p
    joints:    char name[32], i32 parent (-1 for the root), f32 t[3], f32 q[4] (x y z w), f32 s[3],
               f32 inverse_bind[16] (row-major, p' = M p)  -- parents always come first
    vertices:  f32 pos[3], f32 normal[3], f32 uv[2], u8 joints[4], u8 weights[4] (sum 255), u32 material
    indices:   u32, triangle list, counter-clockwise front faces as glTF has them
    materials: char name[32], f32 roughness (when there is no map), u32 flags (1 double-sided)
    then, per material, per mip from the largest: tex_size_m^2 RGBA8 of A (sRGB albedo, 255)
    then, per material, per mip: tex_size_m^2 RGBA8 of B (normal x, normal y DOWN the texture as
               0.5 + 0.5n, roughness, 255)

The animation library (Quaternius's Universal Animation Library, CC0, the in-place glb -- physics
moves the body) goes to assets/models/packed/ual.anim, for any character on the same joint names:

    magic 'ANM1', u32 version (1), u32 joints, u32 clips, f32 fps
    joints:    char name[32], i32 parent, f32 t[3], f32 q[4]   -- the library's REST pose, local
    clips:     char name[32], u32 frames, f32 duration s, f32 speed m/s (root motion, from the _RM
               glb), f32 phase0 (the fraction of the cycle where the left foot is furthest
               forward; 0 for clips that are not a gait), u32 flags (1 loops)
    then per clip, per frame, per joint: f32 q[4], f32 t[3]   (local, the library's frames)

A loop's frames span [0, duration) and wrap; a one-shot's span [0, duration] inclusive. The
engine retargets the rotations onto each character's own rest pose (character_model.cpp).

The normal is packed with +y pointing DOWN the texture (along +v, D3D's direction) so the shader
can build its frame from dP/du and dP/dv directly. The kit's glTF folder ships the OpenGL
convention, which glTF requires; its "Textures" folder ships DirectX, which we do not read.

The glTF files are untrusted data: only the accessors and images they name are read, and image
paths must stay inside the kit's directory.
"""
import json
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.getcwd()
KIT = os.path.join(ROOT, 'assets', 'models', 'Universal Base Characters[Standard]')
BODY = os.path.join(KIT, 'Base Characters', 'Godot - UE')
HAIR = os.path.join(KIT, 'Hairstyles', 'Rigged to Head Bone', 'glTF (Godot -Unreal)')
GL_NORMALS = os.path.join(KIT, 'Base Characters', 'Textures', 'Normals Unity - Godot')
OUT = os.path.join(ROOT, 'assets', 'models', 'packed')
MODELS = os.path.join(ROOT, 'assets', 'models')
UAL = os.path.join(MODELS, 'Universal Animation Library[Standard]', 'Unreal-Godot')
FPS = 30.0
# Gait clips whose left foot's furthest-forward frame becomes their phase 0,
# so the engine can blend walk, jog and sprint in step.
GAITS = ('Walk_Loop', 'Walk_Formal_Loop', 'Jog_Fwd_Loop', 'Sprint_Loop', 'Crouch_Fwd_Loop', 'Swim_Fwd_Loop')
TEX = 1024

# Each character: its body, then hair on the same skeleton, and a colour for
# any material whose texture is a grey mask meant to be tinted (the kit's
# hair, eyebrows included): the albedo is scaled in linear light so its mean
# becomes this sRGB colour.
CHARACTERS = {
    'male': {'sources': [os.path.join(BODY, 'Superhero_Male_FullBody.gltf'),
                         os.path.join(HAIR, 'Hair_SimpleParted.gltf')],
             'tint': {'MI_Hair_1': (62, 44, 30)}},
    'female': {'sources': [os.path.join(BODY, 'Superhero_Female_FullBody.gltf'),
                           os.path.join(HAIR, 'Hair_Buns.gltf')],
               'tint': {'MI_Hair_2': (96, 52, 30)}},
}

COMP = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def inside(base, path):
    full = os.path.realpath(os.path.join(base, path))
    if not full.startswith(os.path.realpath(MODELS) + os.sep):
        raise SystemExit('refusing a path outside assets/models: %r' % path)
    return full


class Gltf:
    def __init__(self, path):
        self.dir = os.path.dirname(path)
        self.bufs = []
        if path.endswith('.glb'):
            with open(path, 'rb') as f:
                d = f.read()
            if d[:4] != b'glTF':
                raise SystemExit('%s: not a glb' % path)
            off, binary = 12, None
            while off + 8 <= len(d):
                n, kind = struct.unpack('<II', d[off:off + 8])
                chunk = d[off + 8:off + 8 + n]
                if kind == 0x4E4F534A:
                    self.g = json.loads(chunk.decode('utf-8'))
                elif kind == 0x004E4942:
                    binary = chunk
                off += 8 + n
            for b in self.g['buffers']:
                if 'uri' in b:
                    with open(inside(self.dir, b['uri']), 'rb') as f:
                        self.bufs.append(f.read())
                else:
                    self.bufs.append(binary)
            return
        with open(path, 'r', encoding='utf-8') as f:
            self.g = json.load(f)
        for b in self.g['buffers']:
            with open(inside(self.dir, b['uri']), 'rb') as f:
                self.bufs.append(f.read())

    def acc(self, i):
        a = self.g['accessors'][i]
        bv = self.g['bufferViews'][a['bufferView']]
        fmt, size = COMP[a['componentType']]
        n = NCOMP[a['type']]
        stride = bv.get('byteStride', size * n)
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        data = self.bufs[bv['buffer']]
        dt = np.dtype('<' + fmt)
        out = np.empty((a['count'], n), dtype=dt)
        if stride == size * n:
            out[:] = np.frombuffer(data, dtype=dt, count=a['count'] * n, offset=off).reshape(-1, n)
        else:
            for k in range(a['count']):
                out[k] = np.frombuffer(data, dtype=dt, count=n, offset=off + k * stride)
        if a.get('normalized'):
            out = out.astype(np.float64) / float((1 << (8 * size)) - 1)
        return out

    def parents(self):
        p = [-1] * len(self.g['nodes'])
        for i, n in enumerate(self.g['nodes']):
            for c in n.get('children', []):
                p[c] = i
        return p


def quat_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def node_local(n):
    if 'matrix' in n:
        return np.array(n['matrix'], dtype=np.float64).reshape(4, 4).T
    m = np.eye(4)
    s = np.array(n.get('scale', [1, 1, 1]), dtype=np.float64)
    m[:3, :3] = quat_mat(n.get('rotation', [0, 0, 0, 1])) * s[None, :]
    m[:3, 3] = n.get('translation', [0, 0, 0])
    return m


def node_global(g, parents, i):
    m = node_local(g['nodes'][i])
    p = parents[i]
    while p >= 0:
        m = node_local(g['nodes'][p]) @ m
        p = parents[p]
    return m


def load_rgb(path, size):
    im = Image.open(path)
    im.load()
    im = im.convert('RGB')
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1.0 / 2.4) - 0.055)


def down2(a):
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def mips_a(rgb_srgb):
    lin = srgb_to_linear(rgb_srgb)
    out = []
    while True:
        a = np.concatenate([linear_to_srgb(lin), np.ones(lin.shape[:2] + (1,))], axis=2)
        out.append(np.round(a * 255).astype(np.uint8))
        if lin.shape[0] == 1:
            return out
        lin = down2(lin)


def mips_b(nrm, rough):
    # nrm: (x, y-down) in -1..1 with z implied; mips average the vector and renormalise.
    v = np.concatenate([nrm, np.sqrt(np.clip(1 - (nrm ** 2).sum(2, keepdims=True), 0, 1))], axis=2)
    out = []
    while True:
        u = v / np.maximum(np.linalg.norm(v, axis=2, keepdims=True), 1e-8)
        b = np.concatenate([0.5 + 0.5 * u[..., :2], rough[..., None], np.ones(rough.shape + (1,))], axis=2)
        out.append(np.round(np.clip(b, 0, 1) * 255).astype(np.uint8))
        if v.shape[0] == 1:
            return out
        v = down2(v)
        rough = down2(rough)


def pack(name, sources, tint, list_only=False):
    body = Gltf(sources[0])
    g = body.g
    bparents = body.parents()
    skin = g['skins'][0]
    jnodes = skin['joints']
    jname = [g['nodes'][j]['name'] for j in jnodes]
    jindex = {n: k for k, n in enumerate(jname)}
    if len(jname) > 255:
        raise SystemExit('more than 255 joints')
    # Parents among joints, and the transform of whatever is above the top joint.
    jparent = []
    for j in jnodes:
        p = bparents[j]
        jparent.append(jnodes.index(p) if p in jnodes else -1)
    roots = [k for k, p in enumerate(jparent) if p < 0]
    if len(roots) != 1:
        raise SystemExit('expected one root joint, found %d' % len(roots))
    # Order joints so parents come first.
    order, seen = [], set()
    def visit(k):
        if k in seen:
            return
        if jparent[k] >= 0:
            visit(jparent[k])
        seen.add(k)
        order.append(k)
    for k in range(len(jnodes)):
        visit(k)
    remap = {old: new for new, old in enumerate(order)}
    top = bparents[jnodes[roots[0]]]
    root_m = node_global(g, bparents, top) if top >= 0 else np.eye(4)
    ibm = body.acc(skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)   # column-major in glTF

    if list_only:
        print(name, 'root matrix', np.round(root_m, 4).tolist())
        for k in order:
            gm = node_global(g, bparents, jnodes[k])
            print('  %-20s parent %-20s at %s' % (jname[k], jname[jparent[k]] if jparent[k] >= 0 else '-',
                                                  np.round(gm[:3, 3], 3).tolist()))
        return

    verts, idx, mats, mat_index = [], [], [], {}
    for src in sources:
        gl = body if src == sources[0] else Gltf(src)
        gg = gl.g
        rebind = None
        if gl is not body:
            # The hair's skin must name this skeleton's joints. Its inverse
            # binds need not match: the kit's hairstyles were bound on the
            # female skeleton, 35 cm out at the male's head. Per joint,
            # body_bind * hair_inverse_bind takes a hair vertex to where it
            # sits with the hair skeleton posed as the body's rest pose.
            hs = gg['skins'][0]
            hib = gl.acc(hs['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
            worst = 0.0
            rebind = []
            for k, j in enumerate(hs['joints']):
                nm = gg['nodes'][j]['name']
                if nm not in jindex:
                    raise SystemExit('%s: joint %s is not in the body' % (src, nm))
                worst = max(worst, float(np.abs(hib[k] - ibm[jindex[nm]]).max()))
                rebind.append(np.linalg.inv(ibm[jindex[nm]]) @ hib[k])
            rebind = np.array(rebind)
            print('  %s: inverse binds differ from the body by at most %.2e; rebound' %
                  (os.path.basename(src), worst))
        for n in gg['nodes']:
            if 'mesh' not in n or 'skin' not in n:
                continue
            sk = gg['skins'][n['skin']]
            jmap = np.array([remap[jindex[gg['nodes'][j]['name']]] for j in sk['joints']], dtype=np.int64)
            for prim in gg['meshes'][n['mesh']]['primitives']:
                at = prim['attributes']
                pos = gl.acc(at['POSITION']).astype(np.float64)
                nrm = gl.acc(at['NORMAL']).astype(np.float64)
                uv = gl.acc(at['TEXCOORD_0']).astype(np.float64)
                jraw = gl.acc(at['JOINTS_0']).astype(np.int64)
                jj = jmap[jraw]
                ww = gl.acc(at['WEIGHTS_0']).astype(np.float64)
                if ww.max() > 1.5:
                    raise SystemExit('unnormalised integer weights')
                ww /= np.maximum(ww.sum(1, keepdims=True), 1e-9)
                if rebind is not None:
                    m = (rebind[jraw] * ww[..., None, None]).sum(1)          # (n, 4, 4)
                    pos = np.einsum('nij,nj->ni', m[:, :3, :3], pos) + m[:, :3, 3]
                    nrm = np.einsum('nij,nj->ni', m[:, :3, :3], nrm)
                    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
                wq = np.floor(ww * 255 + 0.5).astype(np.int64)
                # Make each row sum to exactly 255 by fixing the largest.
                big = wq.argmax(1)
                wq[np.arange(len(wq)), big] += 255 - wq.sum(1)
                mat = gg['materials'][prim['material']]
                key = mat['name']
                if key not in mat_index:
                    mat_index[key] = len(mats)
                    mats.append((gl, mat))
                mid = mat_index[key]
                base = sum(len(v[0]) for v in verts)
                verts.append((pos, nrm, uv, jj, wq, np.full(len(pos), mid)))
                ii = gl.acc(prim['indices']).astype(np.int64).reshape(-1)
                idx.append(ii + base)
    pos = np.concatenate([v[0] for v in verts]); nrm = np.concatenate([v[1] for v in verts])
    uv = np.concatenate([v[2] for v in verts]); jj = np.concatenate([v[3] for v in verts])
    wq = np.concatenate([v[4] for v in verts]); mid = np.concatenate([v[5] for v in verts])
    ind = np.concatenate(idx)
    print('  %d vertices, %d triangles, %d materials: %s' % (len(pos), len(ind) // 3, len(mats),
                                                            ', '.join(m['name'] for _, m in mats)))

    mips = int(np.log2(TEX)) + 1
    tex_a, tex_b, mat_rec = [], [], []
    for gl, mat in mats:
        pbr = mat.get('pbrMetallicRoughness', {})
        def image_path(tex_index):
            src = gl.g['textures'][tex_index]['source']
            return inside(gl.dir, gl.g['images'][src]['uri'])
        if 'baseColorTexture' in pbr:
            albedo = load_rgb(image_path(pbr['baseColorTexture']['index']), TEX)
        else:
            f = pbr.get('baseColorFactor', [0.8, 0.8, 0.8, 1])[:3]
            albedo = np.ones((TEX, TEX, 3)) * np.array(f)[None, None, :]
        rough_c = float(pbr.get('roughnessFactor', 1.0))
        if 'metallicRoughnessTexture' in pbr:
            rough = load_rgb(image_path(pbr['metallicRoughnessTexture']['index']), TEX)[..., 1] * rough_c
        else:
            rough = np.full((TEX, TEX), rough_c)
        nrm_xy = np.zeros((TEX, TEX, 2))
        if 'normalTexture' in mat:
            p = image_path(mat['normalTexture']['index'])
            # Prefer the kit's OpenGL copy of the same name when there is one.
            gl_copy = os.path.join(GL_NORMALS, os.path.basename(p).replace('_png.png', '.png'))
            if os.path.exists(gl_copy):
                p = gl_copy
            n = load_rgb(p, TEX)
            nrm_xy[..., 0] = n[..., 0] * 2 - 1
            nrm_xy[..., 1] = -(n[..., 1] * 2 - 1)    # OpenGL +y up the texture -> +y down it
        if mat['name'] in tint:
            lin = srgb_to_linear(albedo)
            want = srgb_to_linear(np.array(tint[mat['name']], dtype=np.float64) / 255.0)
            lin = lin * (want / np.maximum(lin.reshape(-1, 3).mean(0), 1e-4))[None, None, :]
            albedo = linear_to_srgb(lin)
        tex_a.append(mips_a(albedo))
        tex_b.append(mips_b(nrm_xy, rough))
        mat_rec.append((mat['name'], rough_c, 1 if mat.get('doubleSided') else 0))

    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, name + '.chr')
    with open(out + '.tmp', 'wb') as f:
        f.write(b'CHR1')
        f.write(struct.pack('<7I', 1, len(order), len(pos), len(ind), len(mats), TEX, mips))
        f.write(struct.pack('<16f', *root_m.reshape(-1)))
        for new, old in enumerate(order):
            n = g['nodes'][jnodes[old]]
            p = jparent[old]
            f.write(jname[old].encode('ascii')[:31].ljust(32, b'\0'))
            f.write(struct.pack('<i', remap[p] if p >= 0 else -1))
            f.write(struct.pack('<3f', *n.get('translation', [0, 0, 0])))
            f.write(struct.pack('<4f', *n.get('rotation', [0, 0, 0, 1])))
            f.write(struct.pack('<3f', *n.get('scale', [1, 1, 1])))
            f.write(struct.pack('<16f', *ibm[old].reshape(-1)))
        vrec = np.zeros(len(pos), dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2),
                                         ('j', 'u1', 4), ('w', 'u1', 4), ('m', '<u4')])
        vrec['p'] = pos; vrec['n'] = nrm; vrec['uv'] = uv
        vrec['j'] = jj; vrec['w'] = wq; vrec['m'] = mid
        assert vrec.dtype.itemsize == 44
        f.write(vrec.tobytes())
        f.write(ind.astype('<u4').tobytes())
        for nm, r, fl in mat_rec:
            f.write(nm.encode('ascii')[:31].ljust(32, b'\0'))
            f.write(struct.pack('<fI', r, fl))
        for t in tex_a:
            for m in t:
                f.write(m.tobytes())
        for t in tex_b:
            for m in t:
                f.write(m.tobytes())
    os.replace(out + '.tmp', out)
    print('  wrote %s (%.1f MB)' % (os.path.relpath(out, ROOT), os.path.getsize(out) / 1e6))


def qmul(a, b):
    ax, ay, az, aw = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    bx, by, bz, bw = b[..., 0], b[..., 1], b[..., 2], b[..., 3]
    return np.stack([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz], axis=-1)


def qrot(q, v):
    u = q[..., :3]
    t = 2.0 * np.cross(u, v)
    return v + q[..., 3:4] * t + np.cross(u, t)


def resample(times, values, at, rotation):
    """Linear (rotations: normalised, the short way) at the times `at`."""
    if len(times) == 1:
        return np.repeat(values[:1], len(at), axis=0)
    i = np.clip(np.searchsorted(times, at, side='right') - 1, 0, len(times) - 2)
    t0, t1 = times[i], times[i + 1]
    f = np.clip((at - t0) / np.maximum(t1 - t0, 1e-9), 0.0, 1.0)[:, None]
    a, b = values[i], values[i + 1]
    if rotation:
        b = np.where((a * b).sum(1, keepdims=True) < 0.0, -b, b)
        r = a * (1 - f) + b * f
        return r / np.linalg.norm(r, axis=1, keepdims=True)
    return a * (1 - f) + b * f


def pack_animations():
    src = os.path.join(UAL, 'UAL1_Standard.glb')
    src_rm = os.path.join(UAL, 'UAL1_Standard_RM.glb')
    if not os.path.exists(src):
        print('no animation library at %s -- the engine falls back to its procedural gait' %
              os.path.relpath(src, ROOT))
        return
    gl = Gltf(src)
    g = gl.g
    par = gl.parents()
    jnodes = g['skins'][0]['joints']
    jn = [g['nodes'][j]['name'] for j in jnodes]
    jpar = [jnodes.index(par[j]) if par[j] in jnodes else -1 for j in jnodes]
    for k, p in enumerate(jpar):
        if p >= k:
            raise SystemExit("the library's joints are not parents-first")
    top = par[jnodes[jpar.index(-1)]]
    if top >= 0 and not np.allclose(node_global(g, par, top), np.eye(4)):
        raise SystemExit("the library's skeleton sits under a transformed node")
    rest_q = np.array([g['nodes'][j].get('rotation', [0, 0, 0, 1]) for j in jnodes], dtype=np.float64)
    rest_t = np.array([g['nodes'][j].get('translation', [0, 0, 0]) for j in jnodes], dtype=np.float64)
    node_to_joint = {j: k for k, j in enumerate(jnodes)}

    # Root-motion speeds from the _RM glb: the root's horizontal travel over the clip.
    speeds = {}
    if os.path.exists(src_rm):
        rm = Gltf(src_rm)
        for a in rm.g['animations']:
            for c in a['channels']:
                if rm.g['nodes'][c['target']['node']].get('name') == 'root' and c['target']['path'] == 'translation':
                    smp = a['samplers'][c['sampler']]
                    t = rm.acc(smp['input'])[:, 0].astype(np.float64)
                    v = rm.acc(smp['output']).astype(np.float64)
                    if t[-1] > 0:
                        speeds[a['name']] = float(np.hypot(*(v[-1] - v[0])[[0, 2]])) / float(t[-1])

    clips = []
    for a in g['animations']:
        name = a['name']
        dur = 0.0
        for smp in a['samplers']:
            dur = max(dur, float(gl.acc(smp['input'])[-1, 0]))
        loop = name.endswith('_Loop')
        if loop:
            frames = max(1, int(round(dur * FPS)))
            at = np.arange(frames) * (dur / frames)
        else:
            frames = max(2, int(round(dur * FPS)) + 1)
            at = np.linspace(0.0, dur, frames)
        q = np.repeat(rest_q[None], frames, axis=0)
        t = np.repeat(rest_t[None], frames, axis=0)
        for c in a['channels']:
            j = node_to_joint.get(c['target']['node'])
            path = c['target']['path']
            if j is None or path not in ('rotation', 'translation'):
                continue
            smp = a['samplers'][c['sampler']]
            if smp.get('interpolation', 'LINEAR') == 'CUBICSPLINE':
                raise SystemExit('%s: cubic spline keys are not handled' % name)
            times = gl.acc(smp['input'])[:, 0].astype(np.float64)
            vals = gl.acc(smp['output']).astype(np.float64)
            if path == 'rotation':
                q[:, j] = resample(times, vals, at, True)
            else:
                t[:, j] = resample(times, vals, at, False)
        phase0 = 0.0
        if name in GAITS:
            # Forward kinematics for the feet: where the left one leads most.
            gq = np.zeros((frames, len(jn), 4))
            gp = np.zeros((frames, len(jn), 3))
            for j in range(len(jn)):
                if jpar[j] < 0:
                    gq[:, j] = q[:, j]
                    gp[:, j] = t[:, j]
                else:
                    p = jpar[j]
                    gq[:, j] = qmul(gq[:, p], q[:, j])
                    gp[:, j] = gp[:, p] + qrot(gq[:, p], t[:, j])
            lead = gp[:, jn.index('foot_l'), 2] - gp[:, jn.index('foot_r'), 2]
            phase0 = float(np.argmax(lead)) / frames
        clips.append((name, frames, dur, speeds.get(name, 0.0), phase0, 1 if loop else 0, q, t))

    os.makedirs(OUT, exist_ok=True)
    out = os.path.join(OUT, 'ual.anim')
    with open(out + '.tmp', 'wb') as f:
        f.write(b'ANM1')
        f.write(struct.pack('<3If', 1, len(jn), len(clips), FPS))
        for k in range(len(jn)):
            f.write(jn[k].encode('ascii')[:31].ljust(32, b'\0'))
            f.write(struct.pack('<i3f4f', jpar[k], *rest_t[k], *rest_q[k]))
        for name, frames, dur, speed, phase0, flags, q, t in clips:
            f.write(name.encode('ascii')[:31].ljust(32, b'\0'))
            f.write(struct.pack('<I3fI', frames, dur, speed, phase0, flags))
        for name, frames, dur, speed, phase0, flags, q, t in clips:
            f.write(np.concatenate([q, t], axis=2).astype('<f4').tobytes())
    os.replace(out + '.tmp', out)
    total = sum(c[2] for c in clips)
    print('animations: %d clips, %.0f s at %g fps' % (len(clips), total, FPS))
    for name, frames, dur, speed, phase0, flags, q, t in clips:
        if speed > 0.0 or phase0 > 0.0:
            print('  %-18s %.2f s, %.2f m/s, left foot leads at %.2f of the cycle' % (name, dur, speed, phase0))
    print('  wrote %s (%.1f MB)' % (os.path.relpath(out, ROOT), os.path.getsize(out) / 1e6))


def main():
    list_only = '--list' in sys.argv
    if not os.path.isdir(KIT):
        raise SystemExit('the character kit is not at %s' % os.path.relpath(KIT, ROOT))
    for name, c in CHARACTERS.items():
        print(name)
        pack(name, c['sources'], c.get('tint', {}), list_only)
    if not list_only:
        pack_animations()


if __name__ == '__main__':
    main()
