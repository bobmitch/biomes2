// ---------------------------------------------------------------------------
// The player's survival state: health, stamina, what has been eaten, the
// inventory, what is in hand and the game's clock. The rules are Valheim's:
//
// - Health and stamina have a small base (25 and 50). Food raises the MAXIMUM
//   of each for as long as it lasts; up to three foods at once, each a
//   different one. A food's value is (time left / duration)^0.3 of its full
//   amount: near full for most of its time, then falling away at the end.
// - The same food cannot be eaten again until it is below half its time; then
//   eating it refreshes it. With every slot full, a new food replaces one
//   that is below half; otherwise "too full".
// - Health regenerates in ticks (every 10 s) by the foods' regen. Stamina
//   regenerates continuously, after a short delay since it was last used, and
//   not while swimming.
// - Sprinting and swimming drain stamina; a jump, a swing of the axe and a
//   dig cost it. With none left: no sprint, no jump, no swing; swimming with
//   none left drowns (health lost a second).
// - A fall further than 4 m hurts, rising to 100 at 20 m; into water it does
//   not. At zero health the player dies: the window loop respawns them at the
//   spawn with their foods gone (the inventory is kept, for now).
//
// No GPU, no window, no physics: the window loop passes in what the body did
// (Motion), and asks before it spends anything. So a headless test drives it
// directly (--survival-test).
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_SURVIVAL_H
#define ENGINE_PLAYER_SURVIVAL_H

#include "engine/player/inventory.h"
#include "engine/player/items.h"

#include <string>
#include <vector>

namespace engine {
namespace player {

struct ActiveFood {
    int item = -1;
    float time_left = 0.0f;   // seconds of play
};

// What the body did this frame.
struct Motion {
    bool  body = false;        // a character exists (walking, not flying)
    bool  moving = false;      // asked to move
    bool  sprinting = false;   // asked to sprint, and allowed to
    bool  swimming = false;
    bool  in_water = false;    // wading or swimming
    bool  grounded = false;
    float feet_y = 0.0f;
};

enum class EatResult { Eaten, Refreshed, Replaced, SameFood, TooFull, NotFood };

class Survival {
public:
    void init(const ItemTable* table);
    // Base stats, the start inventory, day 1 at `hour`.
    void new_game(float hour);

    // A frame of play: `dt` seconds (0 while paused). Advances the clock,
    // the foods, regeneration, the drains, drowning and falls.
    void update(float dt, const Motion& m);

    // Stamina for an action: spends it and says true, or spends nothing.
    bool spend(float cost);
    // Stamina taken by something that does not ask (a blow): what is there,
    // down to zero.
    void drain(float cost);
    bool can_sprint() const { return stamina_ > 0.0f; }

    // Eats one of `item` (does not touch the inventory).
    EatResult eat(int item);
    // What 1-8, or a right click in the inventory, does with a slot: eats a
    // food (taking one) or equips / unequips a tool. A message, or "".
    std::string use_slot(int slot);
    // Inventory moves that keep the equipped slot with its item.
    bool move_slot(int from, int to);
    bool split_slot(int from, int to);
    // Into the inventory: what did not fit.
    int give(int item, int count);

    int equipped() const { return equipped_; }   // a slot, -1 for empty hands
    ToolUse hand() const;

    void damage(float hp);
    bool dead() const { return dead_; }
    void respawn();
    // Forget a fall in progress (the body was moved by hand).
    void reset_fall() { airborne_ = false; }

    float health() const { return health_; }
    float stamina() const { return stamina_; }
    float max_health() const;
    float max_stamina() const;
    const std::vector<ActiveFood>& foods() const { return foods_; }
    float food_value(const ActiveFood& f) const;      // 0..1 of its full amounts
    float food_share(const ActiveFood& f) const;      // 0..1 of its time left
    bool  can_eat_again(const ActiveFood& f) const;
    float since_stamina_use() const { return since_use_; }
    float last_fall() const { return last_fall_; }    // metres, the last landing's
    float last_fall_damage() const { return last_fall_damage_; }

    // The clock: game seconds since day 1 at 00:00.
    double game_seconds() const { return clock_; }
    void   set_game_seconds(double s) { clock_ = s < 0.0 ? 0.0 : s; }
    void   advance_clock(double game_s) { set_game_seconds(clock_ + game_s); }
    int    day() const;      // 1, 2, ...
    float  hour() const;     // 0..24
    float  clock_rate() const;   // game seconds a second of play

    Inventory& inventory() { return inv_; }
    const Inventory& inventory() const { return inv_; }
    const ItemTable& table() const { return *table_; }
    const PlayerTuning& tuning() const { return table_->tuning; }

    // For the HUD's message line; the caller drains them.
    std::vector<std::string> take_messages();
    void say(const std::string& m) { msgs_.push_back(m); }

    // Text, items by id, so the item table can change under a save.
    bool save(const char* path, std::string& error) const;
    bool load(const char* path, std::string& error);

private:
    void clamp_to_max();

    const ItemTable* table_ = nullptr;
    Inventory inv_;
    std::vector<ActiveFood> foods_;
    float health_ = 25.0f, stamina_ = 50.0f;
    float regen_timer_ = 0.0f;
    float since_use_ = 100.0f;
    int   equipped_ = -1;
    bool  dead_ = false;
    bool  airborne_ = false;
    float peak_y_ = 0.0f;
    float last_fall_ = 0.0f, last_fall_damage_ = 0.0f;
    double clock_ = 14.0 * 3600.0;
    std::vector<std::string> msgs_;
};

}  // namespace player
}  // namespace engine

#endif
