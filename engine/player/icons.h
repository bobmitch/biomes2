// ---------------------------------------------------------------------------
// An item's icon, painted from its shapes (items.h, IconShape) into RGBA8 --
// sRGB, premultiplied alpha, which is what the UI's renderer blends. The UI
// asks for one by the image source "icon:<item id>" (ui_render_d3d12.cpp),
// so a document writes <img src="icon:strawberry"/>.
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_ICONS_H
#define ENGINE_PLAYER_ICONS_H

#include "engine/player/items.h"

#include <cstdint>
#include <vector>

namespace engine {
namespace player {

// `size` x `size` pixels, 4 x 4 samples a pixel, shapes painted in order.
void paint_icon(const ItemDef& item, int size, std::vector<uint8_t>& rgba);

}  // namespace player
}  // namespace engine

#endif
