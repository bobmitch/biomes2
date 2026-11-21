#include "engine/character/character_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {

namespace {

const float kPi = 3.14159265f;

// Quaternions are (x, y, z, w), Hamilton products, rotating column vectors.
void qmul(const float a[4], const float b[4], float o[4]) {
    const float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    std::memcpy(o, r, sizeof r);
}

void qconj(const float a[4], float o[4]) { o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }

void qnorm(float q[4]) {
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l > 0.0f) for (int k = 0; k < 4; ++k) q[k] /= l;
}

void qaxis(const float axis[3], float angle, float o[4]) {
    const float s = std::sin(0.5f * angle);
    o[0] = axis[0] * s; o[1] = axis[1] * s; o[2] = axis[2] * s; o[3] = std::cos(0.5f * angle);
}

void qrot(const float q[4], const float v[3], float o[3]) {
    // v + 2w (q x v) + 2 q x (q x v)
    const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                        2.0f * (q[0] * v[1] - q[1] * v[0])};
    const float r[3] = {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]),
                        v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
                        v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
    o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
}

// Normalised lerp, the short way round.
void qnlerp(const float a[4], const float b[4], float t, float o[4]) {
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sb = d < 0.0f ? -1.0f : 1.0f;
    for (int k = 0; k < 4; ++k) o[k] = a[k] * (1.0f - t) + b[k] * t * sb;
    qnorm(o);
}

// The shortest rotation taking unit a to unit b.
void qarc(const float a[3], const float b[3], float o[4]) {
    const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 1.0f + d;
    if (o[3] < 1e-6f) { o[0] = 0.0f; o[1] = 0.0f; o[2] = 1.0f; o[3] = 0.0f; }
    qnorm(o);
}

// Rotation part of a 3 x 4 row matrix to a quaternion (orthonormal assumed).
void mat_to_q(const float m[12], float q[4]) {
    const float m00 = m[0], m01 = m[1], m02 = m[2], m10 = m[4], m11 = m[5], m12 = m[6], m20 = m[8], m21 = m[9],
                m22 = m[10];
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
    qnorm(q);
}

void q_to_mat(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

float smooth(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// The head joint's height over the capsule's feet while swimming: the eye
// (1.6 m) sits about at the water, the head joint at the base of the skull.
const float kSwimHeadY = 1.30f;

// Gait curves, degrees at 0%, 10% ... 90% of the cycle from heel strike,
// periodic. Walking after Winter (1991), running after Novacheck (1998),
// rounded; hip and knee are flexion, the ankle plantarflexion (toes down).
const float kWalkHip[10]   = {25, 22, 12, 3, -5, -10, -5, 10, 22, 27};
const float kWalkKnee[10]  = {5, 15, 12, 6, 5, 15, 38, 58, 45, 15};
const float kWalkAnkle[10] = {0, 6, -3, -8, -10, 5, 18, 8, 0, -2};
const float kRunHip[10]    = {30, 22, 10, -2, -12, -8, 10, 30, 45, 40};
const float kRunKnee[10]   = {20, 38, 30, 18, 15, 45, 85, 105, 80, 35};
const float kRunAnkle[10]  = {-5, -15, -18, -5, 20, 15, 5, 0, -5, -8};

// Periodic Catmull-Rom through the ten knots.
float gait_curve(const float k[10], float g) {
    g -= std::floor(g);
    const float x = g * 10.0f;
    const int i = int(x);
    const float t = x - float(i);
    const float p0 = k[(i + 9) % 10], p1 = k[i % 10], p2 = k[(i + 1) % 10], p3 = k[(i + 2) % 10];
    return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
}

float approach(float cur, float target, float rate, float dt) {
    return cur + (target - cur) * (1.0f - std::exp(-rate * dt));
}

template <class T> bool rd(const uint8_t*& p, const uint8_t* end, T& out) {
    if (size_t(end - p) < sizeof(T)) return false;
    std::memcpy(&out, p, sizeof(T));
    p += sizeof(T);
    return true;
}

}  // namespace

bool CharacterModel::load(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "no character pack at " + path + " -- run python -I tools/models/pack_character.py"; return false; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size_t(size > 0 ? size : 0));
    const size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size() || data.size() < 36 || std::memcmp(data.data(), "CHR1", 4) != 0) {
        error = path + ": not a CHR1 character pack";
        return false;
    }
    const uint8_t* p = data.data() + 4;
    const uint8_t* end = data.data() + data.size();
    uint32_t hdr[7];
    for (int k = 0; k < 7; ++k) rd(p, end, hdr[k]);
    if (hdr[0] != 1u || hdr[1] == 0u || hdr[1] > 255u || hdr[5] == 0u || hdr[6] == 0u || hdr[6] > 16u) {
        error = path + ": unsupported character pack";
        return false;
    }
    const uint32_t nj = hdr[1], nv = hdr[2], ni = hdr[3], nm = hdr[4];
    tex_size = hdr[5];
    tex_mips = hdr[6];
    float rm[16];
    for (float& v : rm) if (!rd(p, end, v)) { error = path + ": truncated"; return false; }
    std::memcpy(root, rm, sizeof root);
    joints.resize(nj);
    for (uint32_t j = 0; j < nj; ++j) {
        CharacterJoint& J = joints[j];
        char name[33] = {};
        if (size_t(end - p) < 32) { error = path + ": truncated joints"; return false; }
        std::memcpy(name, p, 32);
        p += 32;
        J.name = name;
        int32_t par = -1;
        rd(p, end, par);
        J.parent = par;
        for (float& v : J.t) rd(p, end, v);
        for (float& v : J.q) rd(p, end, v);
        for (float& v : J.s) rd(p, end, v);
        float ib[16];
        for (float& v : ib) if (!rd(p, end, v)) { error = path + ": truncated joints"; return false; }
        std::memcpy(J.inv_bind, ib, sizeof J.inv_bind);
        if (J.parent >= int(j)) { error = path + ": a joint's parent comes after it"; return false; }
    }
    const size_t vbytes = size_t(nv) * sizeof(CharacterVertex), ibytes = size_t(ni) * 4;
    if (size_t(end - p) < vbytes + ibytes) { error = path + ": truncated mesh"; return false; }
    vertices.resize(nv);
    std::memcpy(vertices.data(), p, vbytes);
    p += vbytes;
    indices.resize(ni);
    std::memcpy(indices.data(), p, ibytes);
    p += ibytes;
    materials.resize(nm);
    for (uint32_t m = 0; m < nm; ++m) {
        char name[33] = {};
        if (size_t(end - p) < 40) { error = path + ": truncated materials"; return false; }
        std::memcpy(name, p, 32);
        p += 32;
        materials[m].name = name;
        uint32_t flags = 0;
        rd(p, end, materials[m].roughness);
        rd(p, end, flags);
        materials[m].double_sided = (flags & 1u) != 0;
    }
    size_t per = 0;
    for (uint32_t k = 0; k < tex_mips; ++k) {
        const size_t s = (std::max)(tex_size >> k, 1u);
        per += s * s * 4;
    }
    if (size_t(end - p) < per * nm * 2) { error = path + ": truncated textures"; return false; }
    tex_a.assign(nm, std::vector<uint8_t>());
    tex_b.assign(nm, std::vector<uint8_t>());
    for (uint32_t m = 0; m < nm; ++m) { tex_a[m].assign(p, p + per); p += per; }
    for (uint32_t m = 0; m < nm; ++m) { tex_b[m].assign(p, p + per); p += per; }
    for (const CharacterVertex& v : vertices)
        for (int k = 0; k < 4; ++k)
            if (v.joints[k] >= nj || v.material >= nm) { error = path + ": a vertex names a joint or material that is not there"; return false; }
    for (uint32_t i : indices)
        if (i >= nv) { error = path + ": an index past the vertices"; return false; }

    // The rest pose in model space, scale ignored (the kit's is 1).
    float root_q[4];
    mat_to_q(root, root_q);
    for (size_t j = 0; j < joints.size(); ++j) {
        CharacterJoint& J = joints[j];
        if (J.parent < 0) {
            qmul(root_q, J.q, J.rest_q);
            for (int k = 0; k < 3; ++k)
                J.rest_p[k] = root[k * 4 + 0] * J.t[0] + root[k * 4 + 1] * J.t[1] + root[k * 4 + 2] * J.t[2] + root[k * 4 + 3];
        } else {
            const CharacterJoint& P = joints[size_t(J.parent)];
            qmul(P.rest_q, J.q, J.rest_q);
            float t[3];
            qrot(P.rest_q, J.t, t);
            for (int k = 0; k < 3; ++k) J.rest_p[k] = P.rest_p[k] + t[k];
        }
        qnorm(J.rest_q);
    }
    height = 0.0f;
    for (const CharacterVertex& v : vertices) height = (std::max)(height, v.pos[1]);
    return true;
}

int CharacterModel::find(const char* name) const {
    for (size_t j = 0; j < joints.size(); ++j)
        if (joints[j].name == name) return int(j);
    return -1;
}

// --- the animation library ----------------------------------------------------

bool AnimLibrary::load(const std::string& path, const CharacterModel& target, std::string& error) {
    clips.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "no animation pack at " + path + " -- run python -I tools/models/pack_character.py"; return false; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size_t(size > 0 ? size : 0));
    const size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size() || data.size() < 20 || std::memcmp(data.data(), "ANM1", 4) != 0) {
        error = path + ": not an ANM1 animation pack";
        return false;
    }
    const uint8_t* p = data.data() + 4;
    const uint8_t* end = data.data() + data.size();
    uint32_t version = 0, nl = 0, nc = 0;
    float fps = 0.0f;
    rd(p, end, version); rd(p, end, nl); rd(p, end, nc); rd(p, end, fps);
    if (version != 1u || nl == 0u || nl > 255u || nc > 1024u) { error = path + ": unsupported animation pack"; return false; }
    std::vector<std::string> lname(nl);
    std::vector<int> lpar(nl);
    std::vector<float> lt(nl * 3), lq(nl * 4);
    for (uint32_t j = 0; j < nl; ++j) {
        if (size_t(end - p) < 32 + 4 + 28) { error = path + ": truncated joints"; return false; }
        char name[33] = {};
        std::memcpy(name, p, 32);
        p += 32;
        lname[j] = name;
        int32_t par = -1;
        rd(p, end, par);
        if (par >= int(j)) { error = path + ": a joint's parent comes after it"; return false; }
        lpar[j] = par;
        for (int k = 0; k < 3; ++k) rd(p, end, lt[j * 3 + k]);
        for (int k = 0; k < 4; ++k) rd(p, end, lq[j * 4 + k]);
    }
    struct Head { std::string name; uint32_t frames; float dur, speed, phase0; uint32_t flags; };
    std::vector<Head> heads(nc);
    for (uint32_t c = 0; c < nc; ++c) {
        if (size_t(end - p) < 32 + 20) { error = path + ": truncated clips"; return false; }
        char name[33] = {};
        std::memcpy(name, p, 32);
        p += 32;
        heads[c].name = name;
        rd(p, end, heads[c].frames); rd(p, end, heads[c].dur); rd(p, end, heads[c].speed);
        rd(p, end, heads[c].phase0); rd(p, end, heads[c].flags);
        if (heads[c].frames == 0u || heads[c].frames > 100000u || !(heads[c].dur > 0.0f)) {
            error = path + ": a clip with no frames";
            return false;
        }
    }

    // The library's rest pose in model space (its skeleton's parent is the
    // identity; the packer checks), and the character's, joint by joint.
    std::vector<float> lg_q(nl * 4), lg_p(nl * 3);
    for (uint32_t j = 0; j < nl; ++j) {
        if (lpar[j] < 0) {
            std::memcpy(&lg_q[j * 4], &lq[j * 4], 16);
            std::memcpy(&lg_p[j * 3], &lt[j * 3], 12);
        } else {
            const size_t pj = size_t(lpar[j]);
            qmul(&lg_q[pj * 4], &lq[j * 4], &lg_q[j * 4]);
            float t[3];
            qrot(&lg_q[pj * 4], &lt[j * 3], t);
            for (int k = 0; k < 3; ++k) lg_p[j * 3 + k] = lg_p[pj * 3 + k] + t[k];
        }
        qnorm(&lg_q[j * 4]);
    }
    const size_t nj = target.joints.size();
    std::vector<int> map(nj, -1);
    int mapped = 0;
    for (size_t c = 0; c < nj; ++c)
        for (uint32_t l = 0; l < nl; ++l)
            if (lname[l] == target.joints[c].name) { map[c] = int(l); ++mapped; break; }
    // Global retarget: C = G * Lrest^-1 * Crest, so D = Lrest^-1 * Crest per joint.
    std::vector<float> D(nj * 4);
    for (size_t c = 0; c < nj; ++c) {
        if (map[c] < 0) continue;
        float inv[4];
        qconj(&lg_q[size_t(map[c]) * 4], inv);
        qmul(inv, target.joints[c].rest_q, &D[c * 4]);
    }
    float root_q[4], root_inv[4];
    mat_to_q(target.root, root_q);
    qconj(root_q, root_inv);
    const int cp = target.find("pelvis");
    int lp = -1;
    for (uint32_t l = 0; l < nl; ++l) if (lname[l] == "pelvis") lp = int(l);
    const float ratio = (cp >= 0 && lp >= 0 && lg_p[size_t(lp) * 3 + 1] > 0.1f)
                            ? target.joints[size_t(cp)].rest_p[1] / lg_p[size_t(lp) * 3 + 1] : 1.0f;

    clips.resize(nc);
    std::vector<float> G(nl * 4), C(nj * 4), rec(nl * 7);
    const size_t frame_bytes = size_t(nl) * 7 * 4;
    for (uint32_t c = 0; c < nc; ++c) {
        const Head& h = heads[c];
        AnimClip& A = clips[c];
        A.name = h.name;
        A.frames = h.frames;
        A.duration = h.dur;
        A.speed = h.speed;
        A.phase0 = h.phase0;
        A.loop = (h.flags & 1u) != 0;
        A.q.assign(size_t(h.frames) * nj * 4, 0.0f);
        A.pelvis.assign(size_t(h.frames) * 3, 0.0f);
        if (size_t(end - p) < frame_bytes * h.frames) { error = path + ": truncated clip data"; clips.clear(); return false; }
        for (uint32_t fr = 0; fr < h.frames; ++fr) {
            std::memcpy(rec.data(), p, frame_bytes);   // q[4] t[3] per library joint
            p += frame_bytes;
            for (uint32_t l = 0; l < nl; ++l) {
                if (lpar[l] < 0) std::memcpy(&G[l * 4], &rec[l * 7], 16);
                else qmul(&G[size_t(lpar[l]) * 4], &rec[l * 7], &G[l * 4]);
                qnorm(&G[l * 4]);
            }
            float* out = &A.q[size_t(fr) * nj * 4];
            for (size_t j = 0; j < nj; ++j) {
                const CharacterJoint& J = target.joints[j];
                if (map[j] >= 0) qmul(&G[size_t(map[j]) * 4], &D[j * 4], &C[j * 4]);
                else if (J.parent >= 0) qmul(&C[size_t(J.parent) * 4], J.q, &C[j * 4]);
                else qmul(root_q, J.q, &C[j * 4]);
                qnorm(&C[j * 4]);
                float pinv[4];
                if (J.parent >= 0) qconj(&C[size_t(J.parent) * 4], pinv);
                else std::memcpy(pinv, root_inv, 16);
                qmul(pinv, &C[j * 4], &out[j * 4]);
                qnorm(&out[j * 4]);
            }
            if (cp >= 0) {
                const float* rt = lp >= 0 ? &rec[size_t(lp) * 7 + 4] : nullptr;
                for (int k = 0; k < 3; ++k)
                    A.pelvis[size_t(fr) * 3 + k] = target.joints[size_t(cp)].t[k] +
                        (rt ? (rt[k] - lt[size_t(lp) * 3 + k]) * ratio : 0.0f);
            }
        }
    }
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s: %u clips, %d of %zu joints mapped, hip height ratio %.3f", path.c_str(), nc,
                  mapped, nj, ratio);
    note = buf;
    return true;
}

int AnimLibrary::find(const char* name) const {
    for (size_t c = 0; c < clips.size(); ++c)
        if (clips[c].name == name) return int(c);
    return -1;
}

// --- the animator -------------------------------------------------------------

void CharacterAnimator::bind(const CharacterModel* m, const AnimLibrary* lib) {
    model_ = m;
    lib_ = (m && lib && !lib->clips.empty()) ? lib : nullptr;
    if (!m) return;
    const size_t n = m->joints.size();
    local_q_.assign(n * 4, 0.0f);
    global_m_.assign(n * 12, 0.0f);
    pelvis_ = m->find("pelvis");
    spine_[0] = m->find("spine_01"); spine_[1] = m->find("spine_02"); spine_[2] = m->find("spine_03");
    neck_ = m->find("neck_01");
    head_ = m->find("Head");
    const char* side[2] = {"_l", "_r"};
    for (int s = 0; s < 2; ++s) {
        auto j = [&](const char* base) { return m->find((std::string(base) + side[s]).c_str()); };
        thigh_[s] = j("thigh"); calf_[s] = j("calf"); foot_[s] = j("foot"); ball_[s] = j("ball");
        clav_[s] = j("clavicle"); upper_[s] = j("upperarm"); lower_[s] = j("lowerarm"); hand_[s] = j("hand");
        fingers_[s].clear();
        thumbs_[s].clear();
        for (const char* f : {"index", "middle", "ring", "pinky"})
            for (const char* k : {"_01", "_02", "_03"}) {
                const int i = j((std::string(f) + k).c_str());
                if (i >= 0) fingers_[s].push_back(i);
            }
        for (const char* k : {"_02", "_03"}) {
            const int i = j((std::string("thumb") + k).c_str());
            if (i >= 0) thumbs_[s].push_back(i);
        }
        // From the T-pose's arm direction to hanging 10 degrees out from the
        // side, and to 40 degrees below level for the air.
        if (upper_[s] >= 0 && lower_[s] >= 0) {
            const float* a = m->joints[size_t(upper_[s])].rest_p;
            const float* b = m->joints[size_t(lower_[s])].rest_p;
            float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (float& v : d) v /= (l > 0.0f ? l : 1.0f);
            const float out = d[0] >= 0.0f ? 1.0f : -1.0f;
            const float hang[3] = {out * std::sin(0.17f), -std::cos(0.17f), 0.0f};
            const float half[3] = {out * std::cos(0.70f), -std::sin(0.70f), 0.0f};
            qarc(d, hang, arm_down_[s]);
            qarc(d, half, arm_half_[s]);
        }
    }
    phase_ = 0.0f;
    if (lib_) {
        c_idle_ = lib_->find("Idle_Loop");
        c_walk_ = lib_->find("Walk_Loop");
        c_jog_ = lib_->find("Jog_Fwd_Loop");
        c_sprint_ = lib_->find("Sprint_Loop");
        c_air_ = lib_->find("Jump_Loop");
        c_land_ = lib_->find("Jump_Land");
        c_swim_idle_ = lib_->find("Swim_Idle_Loop");
        c_swim_fwd_ = lib_->find("Swim_Fwd_Loop");
        c_torch_ = lib_->find("Idle_Torch_Loop");
        c_action_ = lib_->find("Sword_Attack");
        // Masks: the upper body (spine_01 and everything it carries) and the
        // right arm (clavicle_r down), by walking each joint's parents.
        const int spine = m->find("spine_01"), clav_r = m->find("clavicle_r");
        upper_mask_.assign(n, 0);
        right_arm_mask_.assign(n, 0);
        for (size_t j = 0; j < n; ++j)
            for (int a = int(j); a >= 0; a = m->joints[size_t(a)].parent) {
                if (a == spine) upper_mask_[j] = 1;
                if (a == clav_r) right_arm_mask_[j] = 1;
            }
        pose_a_.resize(n);
        pose_b_.resize(n);
        pose_c_.resize(n);
    }
}

// Fold a MODEL-SPACE rotation at the rest pose into joint j's local rotation:
// local' = local * (rest^-1 * q * rest). With the parents at rest, joint j's
// global becomes q * rest -- the rotation about the model-space axis at the
// joint -- and posed parents carry it along.
void CharacterAnimator::apply(int j, const float q_model[4]) {
    if (j < 0) return;
    const float* rq = model_->joints[size_t(j)].rest_q;
    float inv[4], t[4], d[4];
    qconj(rq, inv);
    qmul(inv, q_model, t);
    qmul(t, rq, d);
    float* l = &local_q_[size_t(j) * 4];
    float o[4];
    qmul(l, d, o);
    qnorm(o);
    std::memcpy(l, o, sizeof o);
}

void CharacterAnimator::update(const LocomotionInput& in) {
    if (!model_) return;
    const CharacterModel& M = *model_;
    const float dt = in.dt;
    const bool ground = in.grounded && !in.swimming;
    move_ = approach(move_, ground ? smooth(0.15f, 1.2f, in.speed) : 0.0f, 8.0f, dt);
    run_ = approach(run_, smooth(2.2f, 5.0f, in.speed), 5.0f, dt);
    air_ = approach(air_, (!in.grounded && !in.swimming) ? 1.0f : 0.0f, in.grounded ? 12.0f : 6.0f, dt);
    swim_ = approach(swim_, in.swimming ? 1.0f : 0.0f, 4.0f, dt);
    for (size_t j = 0; j < M.joints.size(); ++j) std::memcpy(&local_q_[j * 4], M.joints[j].q, 16);
    pelvis_offset_[0] = pelvis_offset_[1] = pelvis_offset_[2] = 0.0f;
    pelvis_t_on_ = false;
    if (lib_) pose_clips(in);
    else pose_procedural(in);
    finish_globals();

    // The lowest of the four contact points -- each heel and ball -- goes back
    // to the ground, so the stance foot is planted at any stride without IK.
    // On the ground only; in the air and the water the pose hangs from the
    // capsule as it is.
    float drop = 1e9f;
    for (int s = 0; s < 2; ++s) {
        if (foot_[s] < 0 || ball_[s] < 0) continue;
        const CharacterJoint& F = M.joints[size_t(foot_[s])];
        const float* g = &global_m_[size_t(foot_[s]) * 12];
        // The heel: under and behind the ankle at rest.
        const float heel_rest[3] = {F.rest_p[0], 0.0f, F.rest_p[2] - 0.05f};
        const float rel[3] = {heel_rest[0] - F.rest_p[0], heel_rest[1] - F.rest_p[1], heel_rest[2] - F.rest_p[2]};
        // Posed global rotation times rest^-1 carries a model-space offset.
        float gq[4], inv[4], d[4], v[3];
        mat_to_q(g, gq);
        qconj(F.rest_q, inv);
        qmul(gq, inv, d);
        qrot(d, rel, v);
        const float heel_y = g[7] + v[1];
        drop = (std::min)(drop, heel_y - heel_rest[1]);
        const float* gb = &global_m_[size_t(ball_[s]) * 12];
        drop = (std::min)(drop, gb[7] - M.joints[size_t(ball_[s])].rest_p[1]);
    }
    if (drop > 1e8f) drop = 0.0f;
    // The clips are authored with their feet on the ground; the drop only
    // mends retargeting's few centimetres. Uncapped it pulled a run's flight
    // phase down 18 cm and glued a foot to the ground.
    if (lib_ && drop > 0.02f) drop = 0.02f;
    drop_ = drop * (1.0f - air_) * (1.0f - swim_);
    // In the water the head rides at the surface, where buoyancy holds the
    // eye: the swim clips lay the body level at the pelvis's height, which
    // stood on the capsule's feet would be a metre and more under water.
    if (swim_ > 0.0f && head_ >= 0) drop_ -= swim_ * (kSwimHeadY - global_m_[size_t(head_) * 12 + 7]);
}

// --- clips --------------------------------------------------------------------

void CharacterAnimator::sample(int clip, float time, AnimPose& out) const {
    const AnimClip& c = lib_->clips[size_t(clip)];
    const size_t nj = model_->joints.size();
    int i0, i1;
    float f;
    if (c.loop) {
        float u = time / c.duration;
        u -= std::floor(u);
        const float x = u * float(c.frames);
        i0 = int(x) % int(c.frames);
        i1 = (i0 + 1) % int(c.frames);
        f = x - std::floor(x);
    } else {
        float u = time / c.duration;
        u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
        const float x = u * float(c.frames - 1);
        i0 = (std::min)(int(x), int(c.frames) - 2);
        if (i0 < 0) i0 = 0;
        i1 = (std::min)(i0 + 1, int(c.frames) - 1);
        f = x - float(i0);
    }
    const float* a = &c.q[size_t(i0) * nj * 4];
    const float* b = &c.q[size_t(i1) * nj * 4];
    for (size_t j = 0; j < nj; ++j) qnlerp(a + j * 4, b + j * 4, f, &out.q[j * 4]);
    for (int k = 0; k < 3; ++k)
        out.pelvis[k] = lerpf(c.pelvis[size_t(i0) * 3 + k], c.pelvis[size_t(i1) * 3 + k], f);
}

void CharacterAnimator::sample_phase(int clip, float phase, AnimPose& out) const {
    const AnimClip& c = lib_->clips[size_t(clip)];
    float u = phase + c.phase0;
    u -= std::floor(u);
    sample(clip, u * c.duration, out);
}

namespace {

// a = a blended towards b by w, over the joints the mask allows (all if
// none); the pelvis's translation only when it is asked for.
void pose_blend(AnimPose& a, const AnimPose& b, float w, const std::vector<uint8_t>* mask, bool pelvis) {
    if (w <= 0.0f) return;
    if (w > 1.0f) w = 1.0f;
    const size_t nj = a.q.size() / 4;
    for (size_t j = 0; j < nj; ++j) {
        if (mask && !(*mask)[j]) continue;
        qnlerp(&a.q[j * 4], &b.q[j * 4], w, &a.q[j * 4]);
    }
    if (pelvis) for (int k = 0; k < 3; ++k) a.pelvis[k] = lerpf(a.pelvis[k], b.pelvis[k], w);
}

}  // namespace

// Locomotion: idle, walk, jog and sprint on one phase, the pair either side
// of the speed blended, advanced by distance over the blended stride; the air
// and the landing; swimming; then the masked layers -- the torch arm, the
// swing of the axe.
void CharacterAnimator::pose_clips(const LocomotionInput& in) {
    const AnimLibrary& L = *lib_;
    const float dt = in.dt, v = in.speed;
    const bool airborne = !in.grounded && !in.swimming;
    idle_t_ += dt;
    auto spd = [&](int c, float d) { return (c >= 0 && L.clips[size_t(c)].speed > 0.05f) ? L.clips[size_t(c)].speed : d; };
    auto stride = [&](int c, float d) { return c >= 0 ? spd(c, d) * L.clips[size_t(c)].duration : d; };
    const float sw = spd(c_walk_, 1.0f), sj = spd(c_jog_, 5.0f), ss = spd(c_sprint_, 8.0f);
    int ca, cb;
    float t;
    if (v <= sj) { ca = c_walk_; cb = c_jog_; t = (v - sw) / (sj - sw); }
    else { ca = c_jog_; cb = c_sprint_; t = (v - sj) / (ss - sj); }
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    if (ca < 0) { ca = cb; t = 1.0f; }
    if (cb < 0) { cb = ca; t = 0.0f; }
    run_ = v <= sj ? t * 0.5f : 0.5f + 0.5f * t;   // for the report: 0 walk, 0.5 jog, 1 sprint
    AnimPose& A = pose_a_;
    AnimPose& B = pose_b_;
    AnimPose& P = pose_c_;
    if (c_idle_ >= 0) {
        sample(c_idle_, idle_t_, P);
    } else {
        for (size_t j = 0; j < model_->joints.size(); ++j) std::memcpy(&P.q[j * 4], model_->joints[j].q, 16);
        if (pelvis_ >= 0) std::memcpy(P.pelvis, model_->joints[size_t(pelvis_)].t, 12);
    }
    if (ca >= 0) {
        const float st = lerpf(stride(ca, 1.3f), stride(cb, 1.3f), t);
        if (in.grounded && !in.swimming) phase_ += v * dt / (st > 0.3f ? st : 0.3f);
        phase_ -= std::floor(phase_);
        sample_phase(ca, phase_, A);
        if (cb != ca && t > 0.0f) { sample_phase(cb, phase_, B); pose_blend(A, B, t, nullptr, true); }
        pose_blend(P, A, move_, nullptr, true);
    }
    // The air: the jump's loop from the moment the feet leave.
    if (airborne && !was_air_) air_t_ = 0.0f;
    if (airborne) air_t_ += dt;
    if (c_air_ >= 0 && air_ > 0.001f) { sample(c_air_, air_t_, A); pose_blend(P, A, air_, nullptr, true); }
    // The landing, after a real fall: played out, faded by speed (a run
    // lands into its stride).
    if (was_air_ && !airborne && in.grounded && air_t_ > 0.35f) land_t_ = 0.0f;
    was_air_ = airborne;
    if (land_t_ >= 0.0f && c_land_ >= 0) {
        land_t_ += dt;
        const float d = L.clips[size_t(c_land_)].duration;
        if (land_t_ >= d || airborne) {
            land_t_ = -1.0f;
        } else {
            sample(c_land_, land_t_, A);
            pose_blend(P, A, (1.0f - smooth(d - 0.35f, d, land_t_)) * (1.0f - smooth(0.4f, 2.0f, v)), nullptr, true);
        }
    }
    // Swimming: treading water to the stroke by speed.
    if (swim_ > 0.001f && (c_swim_idle_ >= 0 || c_swim_fwd_ >= 0)) {
        swim_t_ += dt;
        const int a = c_swim_idle_ >= 0 ? c_swim_idle_ : c_swim_fwd_;
        sample(a, swim_t_, A);
        if (c_swim_fwd_ >= 0) {
            swim_phase_ += (v > 0.5f ? v : 0.5f) * dt / stride(c_swim_fwd_, 2.9f);
            swim_phase_ -= std::floor(swim_phase_);
            sample_phase(c_swim_fwd_, swim_phase_, B);
            pose_blend(A, B, smooth(0.3f, 1.5f, v), nullptr, true);
        }
        pose_blend(P, A, swim_, nullptr, true);
    }
    // The torch in the right hand, over whatever the rest is doing.
    torch_ = approach(torch_, in.torch ? 1.0f : 0.0f, 6.0f, dt);
    if (c_torch_ >= 0 && torch_ > 0.001f) {
        sample(c_torch_, idle_t_, A);
        pose_blend(P, A, torch_, &right_arm_mask_, false);
    }
    // The axe: the upper body swings, the legs keep going.
    if (action_t_ >= 0.0f && c_action_ >= 0) {
        action_t_ += dt;
        const float d = L.clips[size_t(c_action_)].duration;
        if (action_t_ >= d) {
            action_t_ = -1.0f;
        } else {
            sample(c_action_, action_t_, A);
            pose_blend(P, A, smooth(0.0f, 0.1f, action_t_) * (1.0f - smooth(d - 0.3f, d, action_t_)), &upper_mask_,
                       false);
        }
    }
    std::memcpy(local_q_.data(), P.q.data(), P.q.size() * sizeof(float));
    std::memcpy(pelvis_t_, P.pelvis, sizeof pelvis_t_);
    pelvis_t_on_ = true;
}

// The fallback when there is no animation library: a gait from tables.
void CharacterAnimator::pose_procedural(const LocomotionInput& in) {
    const float dt = in.dt;
    const bool ground = in.grounded && !in.swimming;
    // The gait advances by distance: one cycle (two steps) is 1.4 m walking,
    // 4.2 m running, so a foot on the ground moves with the ground.
    const float cycle = lerpf(1.4f, 4.2f, run_);
    if (ground) phase_ += in.speed * dt / cycle;
    else if (in.swimming) phase_ += dt * 0.45f;
    phase_ -= std::floor(phase_);

    const float X[3] = {1, 0, 0}, Y[3] = {0, 1, 0}, Z[3] = {0, 0, 1};
    const float mv = move_, rn = run_, ai = air_, sw = swim_;
    const float still = (1.0f - mv) * (1.0f - ai) * (1.0f - sw);
    const float deg = kPi / 180.0f;
    float q[4], r[4], o[4];

    // The legs by percent of the cycle from that foot's heel strike, as gait
    // labs tabulate them (Winter's walking; Novacheck's running): hip
    // flexion, knee flexion and ankle plantarflexion, in degrees. The left
    // heel strikes at phase 0, the right at 0.5.
    float hip[2], knee[2], ankle[2];
    for (int s = 0; s < 2; ++s) {
        const float g = phase_ + 0.5f * float(s);
        hip[s] = lerpf(gait_curve(kWalkHip, g), gait_curve(kRunHip, g), rn) * deg;
        knee[s] = lerpf(gait_curve(kWalkKnee, g), gait_curve(kRunKnee, g), rn) * deg;
        ankle[s] = lerpf(gait_curve(kWalkAnkle, g), gait_curve(kRunAnkle, g), rn) * deg;
    }
    const float hip_mid = lerpf(8.0f, 15.0f, rn) * deg, hip_amp = lerpf(18.0f, 28.0f, rn) * deg;
    const float twist = (hip[0] - hip[1]) / (2.0f * hip_amp);   // +1: left leg forward

    // Hips and spine: twist against each other, lean into the speed, breathe.
    const float lean = mv * lerpf(0.04f, 0.22f, rn) + ai * 0.06f;
    const float breathe = 0.018f * std::sin(in.time * 1.7f) * still;
    if (pelvis_ >= 0) { qaxis(Y, -0.10f * mv * twist, q); apply(pelvis_, q); }
    for (int k = 0; k < 3; ++k) {
        if (spine_[k] < 0) continue;
        qaxis(X, lean / 3.0f + (k == 1 ? breathe : 0.0f), q);
        qaxis(Y, (k == 2 ? 0.16f : 0.03f) * mv * twist, r);
        qmul(r, q, o);
        apply(spine_[k], o);
    }
    // Keep the gaze level over the lean.
    if (neck_ >= 0) { qaxis(X, -0.6f * lean, q); apply(neck_, q); }

    for (int s = 0; s < 2; ++s) {
        const float sgn = s == 0 ? 1.0f : -1.0f;  // left is +X
        const float ts = 2.0f * kPi * phase_ + float(s) * kPi;
        // About +X a positive angle swings a downward limb BACK, bends a knee
        // and tips a forward-pointing foot's toes DOWN: hip flexion is the
        // negative of it.
        const float hip_swim = -0.25f - 0.35f * std::sin(ts);
        const float knee_swim = 0.6f + 0.35f * std::cos(ts);
        const float th_rot = -mv * hip[s] + ai * (s == 0 ? -0.8f : -0.45f) + sw * hip_swim;
        const float kn_rot = mv * knee[s] + ai * (s == 0 ? 1.1f : 0.7f) + sw * knee_swim + still * 0.03f;
        const float an_rot = mv * ankle[s] + ai * 0.25f + sw * 0.4f;
        qaxis(X, th_rot, q); apply(thigh_[s], q);
        qaxis(X, kn_rot, q); apply(calf_[s], q);
        qaxis(X, an_rot, q); apply(foot_[s], q);

        // The shoulders settle a little out of the T-pose's square.
        if (clav_[s] >= 0) { qaxis(Z, -sgn * 0.12f, q); apply(clav_[s], q); }
        // Arms: down from the T-pose (or half up, in the air), then swung
        // about the lateral axis with the OTHER leg's hip.
        if (upper_[s] >= 0) {
            float base[4];
            qnlerp(arm_down_[s], arm_half_[s], ai, base);
            const float fwd = (hip[1 - s] - hip_mid) * lerpf(0.8f, 1.1f, rn);
            const float arm_swing = -mv * fwd + ai * -0.35f;
            qaxis(X, arm_swing, q);
            qmul(q, base, o);
            // Treading water: the arms out and sweeping fore and aft.
            if (sw > 0.0f) {
                float tread[4], sweep[4], t2[4];
                qnlerp(arm_down_[s], arm_half_[s], 0.75f, tread);
                qaxis(Y, sgn * (0.2f + 0.55f * std::sin(in.time * 2.4f)), sweep);
                qmul(sweep, tread, t2);
                qnlerp(o, t2, sw, o);
            }
            apply(upper_[s], o);
        }
        // The elbow flexes about +Y in the T-pose (palms down): forward is
        // -Y for the left forearm (+X) and +Y for the right. Walking, a
        // little more on the forward swing.
        const float fwd_share = 0.5f + 0.5f * (hip[1 - s] - hip_mid) / hip_amp;
        const float elbow = 0.18f * still + mv * lerpf(0.30f + 0.25f * fwd_share, 1.45f, rn) + ai * 0.7f + sw * 0.5f;
        qaxis(Y, -sgn * elbow, q); apply(lower_[s], q);
        // A relaxed hand: fingers curled towards the palm (-Y), more running.
        const float curl = lerpf(0.28f, 0.55f, rn * mv);
        qaxis(Z, -sgn * curl, q);
        for (int f : fingers_[s]) apply(f, q);
        qaxis(Z, -sgn * 0.15f, q);
        for (int f : thumbs_[s]) apply(f, q);
    }
}

void CharacterAnimator::finish_globals() {
    const CharacterModel& M = *model_;
    float root_q[4];
    mat_to_q(M.root, root_q);
    std::vector<float> gq(M.joints.size() * 4), gp(M.joints.size() * 3);
    for (size_t j = 0; j < M.joints.size(); ++j) {
        const CharacterJoint& J = M.joints[j];
        float* q = &gq[j * 4];
        float* p = &gp[j * 3];
        const float* jt = (int(j) == pelvis_ && pelvis_t_on_) ? pelvis_t_ : J.t;
        if (J.parent < 0) {
            qmul(root_q, &local_q_[j * 4], q);
            for (int k = 0; k < 3; ++k)
                p[k] = M.root[k * 4 + 0] * jt[0] + M.root[k * 4 + 1] * jt[1] + M.root[k * 4 + 2] * jt[2] + M.root[k * 4 + 3];
        } else {
            const size_t pj = size_t(J.parent);
            qmul(&gq[pj * 4], &local_q_[j * 4], q);
            float t[3];
            qrot(&gq[pj * 4], jt, t);
            for (int k = 0; k < 3; ++k) p[k] = gp[pj * 3 + k] + t[k];
        }
        qnorm(q);
        if (int(j) == pelvis_) for (int k = 0; k < 3; ++k) p[k] += pelvis_offset_[k];
        float r[9];
        q_to_mat(q, r);
        float* g = &global_m_[j * 12];
        for (int row = 0; row < 3; ++row) {
            g[row * 4 + 0] = r[row * 3 + 0];
            g[row * 4 + 1] = r[row * 3 + 1];
            g[row * 4 + 2] = r[row * 3 + 2];
            g[row * 4 + 3] = p[row];
        }
    }
}

bool CharacterAnimator::joint_world(int j, const float feet[3], float yaw, float out[3]) const {
    if (!model_ || j < 0 || size_t(j) >= model_->joints.size()) return false;
    const float* g = &global_m_[size_t(j) * 12];
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = feet[0] + c * g[3] + s * g[11];
    out[1] = feet[1] - drop_ + g[7];
    out[2] = feet[2] - s * g[3] + c * g[11];
    return true;
}

void CharacterAnimator::skin(const float feet[3], float yaw, std::vector<float>& out) const {
    if (!model_) { out.clear(); return; }
    const CharacterModel& M = *model_;
    out.resize(M.joints.size() * 12);
    // World = T(feet, less the planted foot's drop) * Ry(yaw); +Z goes to
    // (sin yaw, 0, cos yaw).
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float W[12] = {c, 0, s, feet[0], 0, 1, 0, feet[1] - drop_, -s, 0, c, feet[2]};
    for (size_t j = 0; j < M.joints.size(); ++j) {
        const float* G = &global_m_[j * 12];
        const float* B = M.joints[j].inv_bind;
        float GB[12];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 4; ++k)
                GB[r * 4 + k] = G[r * 4 + 0] * B[0 * 4 + k] + G[r * 4 + 1] * B[1 * 4 + k] +
                                G[r * 4 + 2] * B[2 * 4 + k] + (k == 3 ? G[r * 4 + 3] : 0.0f);
        float* o = &out[j * 12];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 4; ++k)
                o[r * 4 + k] = W[r * 4 + 0] * GB[0 * 4 + k] + W[r * 4 + 1] * GB[1 * 4 + k] +
                               W[r * 4 + 2] * GB[2 * 4 + k] + (k == 3 ? W[r * 4 + 3] : 0.0f);
    }
}

}  // namespace engine
