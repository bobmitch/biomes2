#include "engine/player/icons.h"

#include <algorithm>
#include <cmath>

namespace engine {
namespace player {

namespace {

bool inside(const IconShape& s, float x, float y) {
    switch (s.type) {
        case IconShape::Ellipse: {
            const float dx = (x - s.a[0]) / std::max(1e-4f, s.a[2]);
            const float dy = (y - s.a[1]) / std::max(1e-4f, s.a[3]);
            return dx * dx + dy * dy <= 1.0f;
        }
        case IconShape::Rect: {
            const float cx = 0.5f * (s.a[0] + s.a[2]), cy = 0.5f * (s.a[1] + s.a[3]);
            float px = x - cx, py = y - cy;
            if (s.rot != 0.0f) {
                const float r = -s.rot * 0.01745329f;
                const float c = std::cos(r), sn = std::sin(r);
                const float qx = c * px - sn * py, qy = sn * px + c * py;
                px = qx;
                py = qy;
            }
            return std::fabs(px) <= 0.5f * std::fabs(s.a[2] - s.a[0]) &&
                   std::fabs(py) <= 0.5f * std::fabs(s.a[3] - s.a[1]);
        }
        case IconShape::Poly: {
            // Even-odd crossing test.
            bool in = false;
            const size_t n = s.pts.size() / 2;
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                const float xi = s.pts[2 * i], yi = s.pts[2 * i + 1];
                const float xj = s.pts[2 * j], yj = s.pts[2 * j + 1];
                if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
            }
            return in;
        }
    }
    return false;
}

}  // namespace

void paint_icon(const ItemDef& item, int size, std::vector<uint8_t>& rgba) {
    const int n = size * size;
    std::vector<float> acc(size_t(n) * 4, 0.0f);   // premultiplied, sRGB
    const int S = 4;
    for (const IconShape& sh : item.icon) {
        for (int py = 0; py < size; ++py) {
            for (int px = 0; px < size; ++px) {
                int hit = 0;
                for (int sy = 0; sy < S; ++sy)
                    for (int sx = 0; sx < S; ++sx) {
                        const float x = (float(px) + (float(sx) + 0.5f) / S) / float(size);
                        const float y = (float(py) + (float(sy) + 0.5f) / S) / float(size);
                        if (inside(sh, x, y)) ++hit;
                    }
                if (!hit) continue;
                const float a = sh.col[3] * float(hit) / float(S * S);
                float* d = &acc[size_t(py * size + px) * 4];
                for (int k = 0; k < 3; ++k) d[k] = sh.col[k] * a + d[k] * (1.0f - a);
                d[3] = a + d[3] * (1.0f - a);
            }
        }
    }
    rgba.resize(size_t(n) * 4);
    for (size_t i = 0; i < acc.size(); ++i)
        rgba[i] = uint8_t(std::max(0.0f, std::min(1.0f, acc[i])) * 255.0f + 0.5f);
}

}  // namespace player
}  // namespace engine
