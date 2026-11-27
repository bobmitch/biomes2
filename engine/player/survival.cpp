#include "engine/player/survival.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {
namespace player {

namespace {
const double kDay = 86400.0;

std::string lower(std::string s) {
    for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}
}  // namespace

void Survival::init(const ItemTable* table) {
    table_ = table;
    inv_.set_table(table);
}

void Survival::new_game(float hour) {
    inv_.clear();
    foods_.clear();
    equipped_ = -1;
    dead_ = false;
    airborne_ = false;
    regen_timer_ = 0.0f;
    since_use_ = 100.0f;
    health_ = tuning().base_health;
    stamina_ = tuning().base_stamina;
    clock_ = double(hour) * 3600.0;
    for (const StartItem& s : table_->start) {
        const int it = table_->find(s.id);
        if (it < 0) continue;
        int left = s.count;
        if (Inventory::valid(s.slot)) left = inv_.add_at(s.slot, it, left);
        if (left > 0) inv_.add(it, left);
    }
}

float Survival::food_share(const ActiveFood& f) const {
    if (!table_->valid(f.item)) return 0.0f;
    const float d = (*table_)[f.item].food.duration_s;
    return d > 0.0f ? std::max(0.0f, std::min(1.0f, f.time_left / d)) : 0.0f;
}

float Survival::food_value(const ActiveFood& f) const {
    const float s = food_share(f);
    return s <= 0.0f ? 0.0f : std::pow(s, tuning().food_decay_power);
}

bool Survival::can_eat_again(const ActiveFood& f) const {
    return food_share(f) < tuning().food_refresh_at;
}

float Survival::max_health() const {
    float h = tuning().base_health;
    for (const ActiveFood& f : foods_) h += (*table_)[f.item].food.health * food_value(f);
    return h;
}

float Survival::max_stamina() const {
    float s = tuning().base_stamina;
    for (const ActiveFood& f : foods_) s += (*table_)[f.item].food.stamina * food_value(f);
    return s;
}

void Survival::clamp_to_max() {
    health_ = std::min(health_, max_health());
    stamina_ = std::min(stamina_, max_stamina());
}

ToolUse Survival::hand() const {
    if (!Inventory::valid(equipped_)) return ToolUse::None;
    const Slot& s = inv_.at(equipped_);
    return s.empty() ? ToolUse::None : (*table_)[s.item].use;
}

int Survival::day() const { return int(clock_ / kDay) + 1; }
float Survival::hour() const { return float(std::fmod(clock_, kDay) / 3600.0); }
float Survival::clock_rate() const { return float(kDay / double(table_->day_length_s)); }

bool Survival::spend(float cost) {
    if (cost <= 0.0f) return true;
    // Valheim's leniency: an action needs SOME stamina, and may take it to 0.
    if (stamina_ <= 0.0f || stamina_ < 0.5f * cost) return false;
    stamina_ = std::max(0.0f, stamina_ - cost);
    since_use_ = 0.0f;
    return true;
}

void Survival::drain(float cost) {
    // Taken, not spent: a blow takes what is there, and never refuses.
    if (cost <= 0.0f) return;
    stamina_ = std::max(0.0f, stamina_ - cost);
    since_use_ = 0.0f;
}

void Survival::damage(float hp) {
    if (dead_ || hp <= 0.0f) return;
    health_ -= hp;
    if (health_ <= 0.0f) {
        health_ = 0.0f;
        dead_ = true;
        say("You died");
    }
}

void Survival::respawn() {
    foods_.clear();
    dead_ = false;
    airborne_ = false;
    health_ = tuning().base_health;
    stamina_ = tuning().base_stamina;
    regen_timer_ = 0.0f;
}

void Survival::update(float dt, const Motion& m) {
    if (dt <= 0.0f) return;
    const PlayerTuning& t = tuning();
    clock_ += double(dt) * double(clock_rate());

    // Foods run down; one whose time is out is gone.
    for (ActiveFood& f : foods_) f.time_left -= dt;
    for (size_t i = 0; i < foods_.size();) {
        if (foods_[i].time_left <= 0.0f) {
            say((*table_)[foods_[i].item].name + " wore off");
            foods_.erase(foods_.begin() + std::ptrdiff_t(i));
        } else {
            ++i;
        }
    }
    if (dead_) return;

    // Regeneration in ticks.
    regen_timer_ += dt;
    while (regen_timer_ >= t.regen_interval_s) {
        regen_timer_ -= t.regen_interval_s;
        float r = t.regen_base;
        for (const ActiveFood& f : foods_) r += (*table_)[f.item].food.regen;
        health_ += r;
    }

    // The body: drains, drowning and falls. Flying spends nothing.
    since_use_ += dt;
    if (m.body) {
        float drain = 0.0f;
        if (m.sprinting && m.moving && !m.swimming) drain += t.sprint_drain;
        if (m.swimming && m.moving) drain += t.swim_drain;
        if (drain > 0.0f) {
            stamina_ = std::max(0.0f, stamina_ - drain * dt);
            since_use_ = 0.0f;
        }
        if (m.swimming && stamina_ <= 0.0f) {
            if (health_ > 0.0f && health_ - t.drown_damage * dt <= 0.0f) say("You drowned");
            damage(t.drown_damage * dt);
        }
        if (m.grounded || m.in_water) {
            if (airborne_ && !m.in_water) {
                last_fall_ = peak_y_ - m.feet_y;
                last_fall_damage_ = 0.0f;
                if (last_fall_ > t.fall_safe_m) {
                    const float k = std::min(1.0f, (last_fall_ - t.fall_safe_m) / (t.fall_lethal_m - t.fall_safe_m));
                    last_fall_damage_ = k * t.fall_max_damage;
                    damage(last_fall_damage_);
                }
            }
            airborne_ = false;
        } else if (!airborne_) {
            airborne_ = true;
            peak_y_ = m.feet_y;
        } else {
            peak_y_ = std::max(peak_y_, m.feet_y);
        }
    } else {
        airborne_ = false;
    }
    if (dead_) return;
    if (since_use_ >= t.stamina_regen_delay_s && !m.swimming)
        stamina_ += t.stamina_regen * dt;
    clamp_to_max();
}

EatResult Survival::eat(int item) {
    if (!table_->valid(item) || (*table_)[item].kind != ItemKind::Food) return EatResult::NotFood;
    const FoodStats& fs = (*table_)[item].food;
    const std::string name = lower((*table_)[item].name);
    for (ActiveFood& f : foods_) {
        if (f.item != item) continue;
        if (!can_eat_again(f)) {
            say("You can't eat any more " + name + " yet");
            return EatResult::SameFood;
        }
        const float before = food_value(f);
        f.time_left = fs.duration_s;
        health_ += fs.health * (1.0f - before);
        stamina_ += fs.stamina * (1.0f - before);
        clamp_to_max();
        say("Ate " + name);
        return EatResult::Refreshed;
    }
    EatResult r = EatResult::Eaten;
    if (int(foods_.size()) >= tuning().food_slots) {
        // Replace the one with least time left among those below half.
        int best = -1;
        for (int i = 0; i < int(foods_.size()); ++i) {
            if (!can_eat_again(foods_[size_t(i)])) continue;
            if (best < 0 || food_share(foods_[size_t(i)]) < food_share(foods_[size_t(best)])) best = i;
        }
        if (best < 0) {
            say("You are too full to eat");
            return EatResult::TooFull;
        }
        foods_.erase(foods_.begin() + best);
        r = EatResult::Replaced;
    }
    ActiveFood f;
    f.item = item;
    f.time_left = fs.duration_s;
    foods_.push_back(f);
    health_ += fs.health;
    stamina_ += fs.stamina;
    clamp_to_max();
    say("Ate " + name);
    return r;
}

std::string Survival::use_slot(int slot) {
    if (!Inventory::valid(slot)) return "";
    const Slot s = inv_.at(slot);
    if (s.empty()) {
        if (equipped_ >= 0) { equipped_ = -1; return "Hands empty"; }
        return "";
    }
    const ItemDef& d = (*table_)[s.item];
    if (d.kind == ItemKind::Food) {
        if (dead_) return "";
        const EatResult r = eat(s.item);
        if (r == EatResult::Eaten || r == EatResult::Refreshed || r == EatResult::Replaced) inv_.take(slot, 1);
        return "";
    }
    if (d.kind == ItemKind::Tool) {
        if (equipped_ == slot) {
            equipped_ = -1;
            return "Put away the " + lower(d.name);
        }
        equipped_ = slot;
        return "Equipped the " + lower(d.name);
    }
    return d.name + " can't be used";
}

bool Survival::move_slot(int from, int to) {
    if (!inv_.move(from, to)) return false;
    // The equipped tool follows its slot through a swap (tools never merge).
    if (equipped_ == from) equipped_ = to;
    else if (equipped_ == to) equipped_ = inv_.at(from).empty() ? -1 : from;
    if (Inventory::valid(equipped_) && inv_.at(equipped_).empty()) equipped_ = -1;
    return true;
}

bool Survival::split_slot(int from, int to) {
    return inv_.split(from, to);
}

int Survival::give(int item, int count) {
    return inv_.add(item, count);
}

std::vector<std::string> Survival::take_messages() {
    std::vector<std::string> m;
    m.swap(msgs_);
    return m;
}

bool Survival::save(const char* path, std::string& error) const {
    FILE* f = std::fopen(path, "w");
    if (!f) { error = std::string("cannot write ") + path; return false; }
    std::fprintf(f, "player 1\n");
    std::fprintf(f, "clock %.3f\n", clock_);
    std::fprintf(f, "health %.4f\nstamina %.4f\nequipped %d\n", health_, stamina_, equipped_);
    for (const ActiveFood& a : foods_) std::fprintf(f, "food %s %.3f\n", (*table_)[a.item].id.c_str(), a.time_left);
    for (int i = 0; i < Inventory::kSlots; ++i) {
        const Slot& s = inv_.at(i);
        if (!s.empty()) std::fprintf(f, "slot %d %s %d\n", i, (*table_)[s.item].id.c_str(), s.count);
    }
    std::fclose(f);
    return true;
}

bool Survival::load(const char* path, std::string& error) {
    FILE* f = std::fopen(path, "r");
    if (!f) { error = std::string("no ") + path; return false; }
    char line[256];
    if (!std::fgets(line, sizeof line, f) || std::strncmp(line, "player 1", 8) != 0) {
        std::fclose(f);
        error = std::string(path) + " is not a player save";
        return false;
    }
    inv_.clear();
    foods_.clear();
    equipped_ = -1;
    dead_ = false;
    airborne_ = false;
    while (std::fgets(line, sizeof line, f)) {
        char key[32] = "", id[96] = "";
        double v = 0.0;
        int a = 0, b = 0;
        if (std::sscanf(line, "%31s", key) != 1) continue;
        const std::string k = key;
        if (k == "clock" && std::sscanf(line, "%*s %lf", &v) == 1) clock_ = v;
        else if (k == "health" && std::sscanf(line, "%*s %lf", &v) == 1) health_ = float(v);
        else if (k == "stamina" && std::sscanf(line, "%*s %lf", &v) == 1) stamina_ = float(v);
        else if (k == "equipped" && std::sscanf(line, "%*s %d", &a) == 1) equipped_ = a;
        else if (k == "food" && std::sscanf(line, "%*s %95s %lf", id, &v) == 2) {
            const int it = table_->find(id);
            if (it >= 0 && (*table_)[it].kind == ItemKind::Food) {
                ActiveFood af;
                af.item = it;
                af.time_left = float(v);
                foods_.push_back(af);
            }
        } else if (k == "slot" && std::sscanf(line, "%*s %d %95s %d", &a, id, &b) == 3) {
            const int it = table_->find(id);
            if (it >= 0) inv_.add_at(a, it, b);
        }
    }
    std::fclose(f);
    if (Inventory::valid(equipped_) && (inv_.at(equipped_).empty() ||
                                        (*table_)[inv_.at(equipped_).item].kind != ItemKind::Tool))
        equipped_ = -1;
    if (health_ <= 0.0f) respawn();
    clamp_to_max();
    return true;
}

}  // namespace player
}  // namespace engine
