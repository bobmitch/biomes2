#include "engine/player/inventory.h"

#include <algorithm>

namespace engine {
namespace player {

void Inventory::clear() {
    for (Slot& s : s_) s = Slot();
}

int Inventory::stack_of(int item) const {
    return table_ && table_->valid(item) ? (*table_)[item].stack : 1;
}

int Inventory::add_at(int slot, int item, int count) {
    if (!valid(slot) || count <= 0) return count;
    Slot& s = s_[slot];
    if (!s.empty() && s.item != item) return count;
    const int room = stack_of(item) - (s.empty() ? 0 : s.count);
    const int put = std::min(room, count);
    if (put <= 0) return count;
    if (s.empty()) { s.item = item; s.count = 0; }
    s.count += put;
    return count - put;
}

int Inventory::add(int item, int count) {
    for (int i = 0; i < kSlots && count > 0; ++i)
        if (!s_[i].empty() && s_[i].item == item) count = add_at(i, item, count);
    for (int i = 0; i < kSlots && count > 0; ++i)
        if (s_[i].empty()) count = add_at(i, item, count);
    return count;
}

int Inventory::take(int slot, int count) {
    if (!valid(slot) || s_[slot].empty() || count <= 0) return 0;
    const int got = std::min(count, s_[slot].count);
    s_[slot].count -= got;
    if (s_[slot].count <= 0) s_[slot] = Slot();
    return got;
}

int Inventory::count_of(int item) const {
    int n = 0;
    for (const Slot& s : s_)
        if (!s.empty() && s.item == item) n += s.count;
    return n;
}

int Inventory::room_for(int item) const {
    int n = 0;
    for (const Slot& s : s_) {
        if (s.empty()) n += stack_of(item);
        else if (s.item == item) n += std::max(0, stack_of(item) - s.count);
    }
    return n;
}

bool Inventory::move(int from, int to) {
    if (!valid(from) || !valid(to) || from == to || s_[from].empty()) return false;
    Slot& a = s_[from];
    Slot& b = s_[to];
    if (!b.empty() && b.item == a.item) {
        const int left = add_at(to, a.item, a.count);
        if (left == a.count) {
            // The target stack is full: swap instead, so the move does something.
            std::swap(a, b);
            return true;
        }
        a.count = left;
        if (a.count <= 0) a = Slot();
        return true;
    }
    std::swap(a, b);
    return true;
}

bool Inventory::split(int from, int to) {
    if (!valid(from) || !valid(to) || from == to || s_[from].empty() || !s_[to].empty()) return false;
    if (s_[from].count < 2) return move(from, to);
    const int half = (s_[from].count + 1) / 2;
    s_[to].item = s_[from].item;
    s_[to].count = half;
    s_[from].count -= half;
    return true;
}

}  // namespace player
}  // namespace engine
