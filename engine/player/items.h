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

#include "engine/player/stealth.h"

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

// What a tool does to a creature (engine/mobs/combat.h) and how its swing
// is timed (engine/player/swing.h). damage 0: it hurts nothing, whatever it
// is for. A weapon's blow lands impact_s after the press -- where the head
// meets the wood in its clip -- and nothing else may be swung until
// recover_s after that.
struct ToolStats {
    float damage = 0.0f;
    float reach_m = 2.6f;      // from the eye along the swing
    float impact_s = 0.28f;    // press -> the blow lands, the clip's impact
    float recover_s = 0.32f;   // ... and after that, before the next swing
    std::string anim = "Sword_Attack";   // the clip the upper body plays
    float anim_s = 0.0f;       // how much of that clip to play, 0 for all
};

struct ItemDef {
    std::string id;            // stable: saves and the forage table name items by it
    std::string name;
    std::string description;
    int stack = 1;
    ItemKind kind = ItemKind::Resource;
    ToolUse use = ToolUse::None;
    FoodStats food;
    ToolStats tool;
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
    float swing_buffer_s = 0.3f;        // a click this near the end of a swing is kept for the next
    float dig_cost = 5.0f;              // a dig
    // Shoving a felled log out of the way. Walking into one pushes with
    // push_newtons, a nudge; keep at it and it builds to heave_newtons over
    // heave_ramp_s -- setting your shoulder to it -- which costs heave_cost
    // stamina a second, by the weight being strained against. Jolt's own
    // default is 100 N, which will not move anything made of wood.
    float push_newtons = 400.0f;
    float heave_newtons = 1300.0f;
    float heave_ramp_s = 0.9f;
    float heave_cost = 7.0f;
    // ... and what walking INTO one hands over: the share of the
    // character's own momentum. 1, all of it, which is also what a
    // controller that stops dead actually loses. A real body is not a
    // billiard ball -- the legs buckle, the feet slip, the chest gives,
    // none of it modelled -- so 0.6 was tried as the honest discount and
    // is why it is a knob; it made WALKING into a log worth a centimetre a
    // knock, which is the one thing this was built for.
    float bump_transfer = 1.0f;
    float drown_damage = 5.0f;          // a second, swimming with no stamina left
    float fall_safe_m = 4.0f;           // a fall up to this does nothing ...
    float fall_lethal_m = 20.0f;        // ... rising linearly to fall_max_damage here
    float fall_max_damage = 100.0f;
    int   food_slots = 3;
    float food_decay_power = 0.3f;      // a food's value is (time left / duration)^power
    float food_refresh_at = 0.5f;       // the same food can be eaten again below this share of its time
};

// What a felled tree leaves and what it takes to break up (items.json
// "logs", engine/trees/felling.h). Gameplay, never world data.
struct LogTuning {
    float wood_density = 650.0f;     // kg/m^3, for a falling trunk's momentum
    float chop_damage = 15.0f;       // health one swing of the reference axe takes
    float health_per_m3 = 70.0f;     // a trunk piece's health, by its own volume
    float min_health = 30.0f;
    float split_at = 0.5f;           // where a whole log breaks, along its length
    int   billets = 2;               // rounds of firewood a broken half leaves
    std::string wood_item = "wood";
    float wood_per_m3 = 9.0f;        // ... and what one is worth, by its volume
    float billet_max_m = 1.2f;       // a round is no longer than this
    // The canopy comes off at the first thing the crown touches -- a tree
    // lands on its branches and they break -- and shed_after_s only catches
    // one that touched nothing and is still leaning somewhere.
    float shed_after_s = 8.0f;
    // ... and how much of a standing tree's mass is in that canopy. The
    // branches, twigs and leaves above the bole are mostly air: a tenth is
    // generous. Modelled as solid trunk the whole way up they weighed as
    // much as the bole, which put the centre of mass at half the height
    // instead of a third -- a slow topple, and far too much spin to shed
    // once it was down.
    float crown_mass_share = 0.1f;
    // How a tree goes over. It pivots on its stump (a hinge) until it has
    // tipped hinge_release_deg, then slips off and falls free; the spin is
    // only the nudge that breaks an upright tree's equilibrium, since a
    // pendulum balanced on its end never falls on its own.
    float topple_spin_rad_s = 0.05f;   // a tree the player cut, on its hinge
    float knock_spin_rad_s = 0.35f;    // ... and one shoved over by another, free
    float hinge_release_deg = 45.0f;
    float debris_toss = 0.6f;        // ... with this share of the speed it had there
    float impact_per_chop = 8000.0f; // joules of impact worth one chop of damage
    float impact_min_speed = 2.5f;   // slower than this and nothing is hurt
    // A tree that has tipped less than this is LEANING, not falling, and
    // anything it touches it was already touching: crowns interlock in a
    // wood, and a trunk whose top is six metres of branches is pressed
    // against its neighbours long before anyone cuts it. Without the gate
    // a faller spent its one blow per neighbour at 2-4 m/s in the first
    // quarter-second, and dropped its crown there too.
    float impact_lean_deg = 15.0f;
    float impact_keep = 0.6f;        // the speed a faller keeps after felling another
    int   debris_pieces = 3;         // clumps the crown comes off as
    float debris_rest_s = 2.5f;      // ... how long they lie there once settled ...
    float debris_fade_s = 2.5f;      // ... and how long they take to go
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
    StealthTuning stealth;          // the `stealth` block: the player as heard and seen
    LogTuning logs;                 // the `logs` block: felled trees, and the wood in them
    float day_length_s = 1800.0f;   // real seconds of play for 24 game hours
    std::vector<StartItem> start;

private:
    std::vector<ItemDef> items_;
};

}  // namespace player
}  // namespace engine

#endif
