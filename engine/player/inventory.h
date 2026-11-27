// ---------------------------------------------------------------------------
// The player's inventory: 8 x 4 slots, Valheim's. The top row (slots 0-7) is
// also the hotbar, used with 1-8. A slot holds one item's stack, up to the
// item's own stack size. What lands where follows Valheim: onto stacks of the
// same item first, then the first empty slot from the top left -- so a new
// kind of thing goes into the hotbar while it has room.
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_INVENTORY_H
#define ENGINE_PLAYER_INVENTORY_H

#include "engine/player/items.h"

namespace engine {
namespace player {

struct Slot {
    int item = -1;    // index into the ItemTable, -1 empty
    int count = 0;
    bool empty() const { return item < 0 || count <= 0; }
};

class Inventory {
public:
    static const int kCols = 8, kRows = 4, kSlots = kCols * kRows;

    void set_table(const ItemTable* t) { table_ = t; }
    void clear();

    // Adds what fits and returns what did not.
    int add(int item, int count);
    // Into one slot (empty, or a stack of the same item): what did not fit.
    int add_at(int slot, int item, int count);
    // Takes up to `count` from a slot; returns how many it took.
    int take(int slot, int count);
    int count_of(int item) const;
    // How many more of `item` would fit.
    int room_for(int item) const;

    // The whole stack at `from` to `to`: onto a stack of the same item as far
    // as it fits (the rest stays), otherwise the two swap. False if nothing
    // moved.
    bool move(int from, int to);
    // Half of `from` (rounded up) to an EMPTY `to`.
    bool split(int from, int to);

    const Slot& at(int i) const { return s_[i]; }
    Slot& at(int i) { return s_[i]; }
    static bool valid(int i) { return i >= 0 && i < kSlots; }

private:
    int stack_of(int item) const;
    const ItemTable* table_ = nullptr;
    Slot s_[kSlots];
};

}  // namespace player
}  // namespace engine

#endif
