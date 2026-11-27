// ---------------------------------------------------------------------------
// What the player can carry: the item table, and the player's tuning, read
// from game/data/items.json. Gameplay data, not world data: nothing here
// reaches the golden hash, and worldgen never reads it.
//
// An item is a resource, a food or a tool. A food gives health, stamina and
// health regeneration for a while (survival.h has the rules); a tool is
// equipped from the hotbar and changes what the hands do (the axe chops, the
// torch lights). An item's icon is a list of flat shapes in a unit square,
// painted at startup (icons.h) -- no image files.
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_ITEMS_H
#define ENGINE_PLAYER_ITEMS_H

#include <string>
#include <vector>

namespace engine {
namespace player {

enum class ItemKind { Resource, Food, Tool };
enum class ToolUse { None, Axe, Torch };

struct FoodStats {
    float health = 0.0f;       // added to max health while it lasts (at full value)
    float stamina = 0.0f;      // added to max stamina
    float regen = 0.0f;        // health a regeneration tick
    float duration_s = 600.0f; // seconds of play it lasts
};

// One shape of an icon, in a unit square with y DOWN. Ellipse: centre and
// radii; rect: corners, turned by rot degrees about its middle; poly: points.
struct IconShape {
    enum Type { Ellipse, Rect, Poly } type = Ellipse;
    float a[4] = {0, 0, 0, 0};
    float rot = 0.0f;
    std::vector<float> pts;    // x, y pairs
    float col[4] = {1, 1, 1, 1};   // sRGB, alpha
};

struct ItemDef {
    std::string id;            // stable: saves and the forage table name items by it
    std::string name;
    std::string description;
    int stack = 1;
    ItemKind kind = ItemKind::Resource;
    ToolUse use = ToolUse::None;
    FoodStats food;
    std::vector<IconShape> icon;
};

// The player's numbers (items.json "player"). Valheim's where it has one.
struct PlayerTuning {
    float base_health = 25.0f;
    float base_stamina = 50.0f;
    float regen_interval_s = 10.0f;     // health regenerates in ticks this far apart
    float regen_base = 0.0f;            // health a tick with no food
    float stamina_regen = 6.0f;         // a second, once the delay has passed
    float stamina_regen_delay_s = 1.0f; // after any use
    float sprint_drain = 10.0f;         // a second, moving at a sprint on land
    float swim_drain = 6.0f;            // a second, swimming under way
    float jump_cost = 10.0f;
    float axe_cost = 8.0f;              // a swing, hit or miss
    float dig_cost = 5.0f;              // a dig
    float drown_damage = 5.0f;          // a second, swimming with no stamina left
    float fall_safe_m = 4.0f;           // a fall up to this does nothing ...
    float fall_lethal_m = 20.0f;        // ... rising linearly to fall_max_damage here
    float fall_max_damage = 100.0f;
    int   food_slots = 3;
    float food_decay_power = 0.3f;      // a food's value is (time left / duration)^power
    float food_refresh_at = 0.5f;       // the same food can be eaten again below this share of its time
};

struct StartItem {
    std::string id;
    int count = 1;
    int slot = -1;     // -1: wherever it lands
};

class ItemTable {
public:
    bool load(const char* path, std::string& error);
    int find(const std::string& id) const;   // -1 if none
    const ItemDef& operator[](int i) const { return items_[size_t(i)]; }
    int size() const { return int(items_.size()); }
    bool valid(int i) const { return i >= 0 && i < size(); }

    PlayerTuning tuning;
    float day_length_s = 1800.0f;   // real seconds of play for 24 game hours
    std::vector<StartItem> start;

private:
    std::vector<ItemDef> items_;
};

}  // namespace player
}  // namespace engine

#endif
