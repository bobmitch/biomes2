#include "engine/player/items.h"
#include "worldgen/assign/json_lite.h"

#include <cstdlib>
#include <stdexcept>

namespace engine {
namespace player {

namespace {

// "#rrggbb" or "#rrggbbaa", sRGB as typed.
void parse_colour(const std::string& s, float out[4]) {
    if (s.size() != 7 && s.size() != 9) throw std::runtime_error("colour " + s + " is not #rrggbb[aa]");
    if (s[0] != '#') throw std::runtime_error("colour " + s + " is not #rrggbb[aa]");
    for (int k = 0; k < 4; ++k) {
        if (k == 3 && s.size() == 7) { out[3] = 1.0f; break; }
        const std::string h = s.substr(size_t(1 + 2 * k), 2);
        out[k] = float(std::strtol(h.c_str(), nullptr, 16)) / 255.0f;
    }
}

IconShape parse_shape(const jsonlite::Value& v) {
    IconShape s;
    parse_colour(v.string("c"), s.col);
    if (v.has("e")) {
        const jsonlite::Array& a = v.array("e");
        if (a.size() != 4) throw std::runtime_error("icon ellipse wants [cx, cy, rx, ry]");
        s.type = IconShape::Ellipse;
        for (int k = 0; k < 4; ++k) s.a[k] = float(a[size_t(k)].num);
    } else if (v.has("r")) {
        const jsonlite::Array& a = v.array("r");
        if (a.size() != 4) throw std::runtime_error("icon rect wants [x0, y0, x1, y1]");
        s.type = IconShape::Rect;
        for (int k = 0; k < 4; ++k) s.a[k] = float(a[size_t(k)].num);
    } else if (v.has("p")) {
        const jsonlite::Array& a = v.array("p");
        if (a.size() < 6 || a.size() % 2) throw std::runtime_error("icon poly wants x, y pairs, three or more");
        s.type = IconShape::Poly;
        for (size_t k = 0; k < a.size(); ++k) s.pts.push_back(float(a[k].num));
    } else {
        throw std::runtime_error("icon shape needs e, r or p");
    }
    if (v.has("rot")) s.rot = float(v.number("rot"));
    return s;
}

}  // namespace

bool ItemTable::load(const char* path, std::string& error) {
    items_.clear();
    start.clear();
    try {
        const jsonlite::Value root = jsonlite::parse_file(path);
        if (root.has("player")) {
            const jsonlite::Value& p = root.at("player");
            auto num = [&](const char* k, float& v) { if (p.has(k)) v = float(p.number(k)); };
            PlayerTuning& t = tuning;
            num("base_health", t.base_health);
            num("base_stamina", t.base_stamina);
            num("regen_interval_s", t.regen_interval_s);
            num("regen_base", t.regen_base);
            num("stamina_regen", t.stamina_regen);
            num("stamina_regen_delay_s", t.stamina_regen_delay_s);
            num("sprint_drain", t.sprint_drain);
            num("swim_drain", t.swim_drain);
            num("jump_cost", t.jump_cost);
            num("axe_cost", t.axe_cost);
            num("swing_buffer_s", t.swing_buffer_s);
            num("dig_cost", t.dig_cost);
            num("push_newtons", t.push_newtons);
            num("heave_newtons", t.heave_newtons);
            num("heave_ramp_s", t.heave_ramp_s);
            num("heave_cost", t.heave_cost);
            num("bump_transfer", t.bump_transfer);
            num("drown_damage", t.drown_damage);
            num("fall_safe_m", t.fall_safe_m);
            num("fall_lethal_m", t.fall_lethal_m);
            num("fall_max_damage", t.fall_max_damage);
            float slots = float(t.food_slots);
            num("food_slots", slots);
            t.food_slots = int(slots);
            num("food_decay_power", t.food_decay_power);
            num("food_refresh_at", t.food_refresh_at);
            if (t.base_health <= 0.0f || t.base_stamina <= 0.0f || t.food_slots < 1 || t.food_slots > 6 ||
                t.regen_interval_s <= 0.0f || t.fall_lethal_m <= t.fall_safe_m)
                throw std::runtime_error("player: base_health and base_stamina > 0, 1..6 food_slots, "
                                         "regen_interval_s > 0, fall_lethal_m > fall_safe_m");
        }
        if (root.has("stealth")) {
            const jsonlite::Value& p = root.at("stealth");
            auto num = [&](const char* k, float& v) { if (p.has(k)) v = float(p.number(k)); };
            StealthTuning& s = stealth;
            num("crouch_speed", s.crouch_speed);
            num("crouch_db", s.crouch_db);
            num("walk_db", s.walk_db);
            num("jog_db", s.jog_db);
            num("sprint_db", s.sprint_db);
            num("swim_db", s.swim_db);
            num("swim_idle_db", s.swim_idle_db);
            num("wade_add_db", s.wade_add_db);
            num("jump_db", s.jump_db);
            num("land_db", s.land_db);
            num("land_db_per_m", s.land_db_per_m);
            num("axe_db", s.axe_db);
            num("dig_db", s.dig_db);
            num("spike_decay_db_s", s.spike_decay_db_s);
            num("vis_still", s.vis_still);
            num("vis_walk", s.vis_walk);
            num("vis_jog", s.vis_jog);
            num("vis_sprint", s.vis_sprint);
            num("vis_crouch_still", s.vis_crouch_still);
            num("vis_crouch_move", s.vis_crouch_move);
            num("vis_swim", s.vis_swim);
            num("dark_lux", s.light.dark_lux);
            num("full_lux", s.light.full_lux);
            num("light_floor", s.light.floor);
            if (s.crouch_speed <= 0.0f || s.spike_decay_db_s <= 0.0f || s.light.dark_lux <= 0.0f ||
                s.light.full_lux <= s.light.dark_lux)
                throw std::runtime_error("stealth: crouch_speed and spike_decay_db_s > 0, 0 < dark_lux < full_lux");
        }
        if (root.has("logs")) {
            const jsonlite::Value& p = root.at("logs");
            auto num = [&](const char* k, float& v) { if (p.has(k)) v = float(p.number(k)); };
            LogTuning& g = logs;
            num("wood_density", g.wood_density);
            num("chop_damage", g.chop_damage);
            num("health_per_m3", g.health_per_m3);
            num("min_health", g.min_health);
            num("split_at", g.split_at);
            float billets = float(g.billets);
            num("billets", billets);
            g.billets = int(billets + 0.5f);
            if (p.has("wood_item")) g.wood_item = p.string("wood_item");
            num("wood_per_m3", g.wood_per_m3);
            num("billet_max_m", g.billet_max_m);
            num("shed_after_s", g.shed_after_s);
            num("topple_spin_rad_s", g.topple_spin_rad_s);
            num("knock_spin_rad_s", g.knock_spin_rad_s);
            num("hinge_release_deg", g.hinge_release_deg);
            num("debris_toss", g.debris_toss);
            num("impact_per_chop", g.impact_per_chop);
            num("impact_min_speed", g.impact_min_speed);
            num("impact_keep", g.impact_keep);
            float debris = float(g.debris_pieces);
            num("debris_pieces", debris);
            g.debris_pieces = int(debris + 0.5f);
            num("debris_rest_s", g.debris_rest_s);
            num("debris_fade_s", g.debris_fade_s);
            if (g.chop_damage <= 0.0f || g.health_per_m3 <= 0.0f || g.min_health <= 0.0f ||
                g.split_at <= 0.05f || g.split_at >= 0.95f || g.billets < 1 || g.billets > 8 ||
                g.impact_per_chop <= 0.0f || g.debris_pieces < 0 || g.debris_pieces > 8 ||
                g.billet_max_m <= 0.1f || g.debris_fade_s <= 0.0f)
                throw std::runtime_error("logs: chop_damage, health_per_m3, min_health, "
                                         "impact_per_chop and debris_fade_s > 0, 0.05 < split_at < 0.95, "
                                         "1..8 billets, 0..8 debris_pieces");
        }
        if (root.has("clock")) {
            const jsonlite::Value& c = root.at("clock");
            if (c.has("day_length_min")) day_length_s = float(c.number("day_length_min")) * 60.0f;
            if (day_length_s < 10.0f) throw std::runtime_error("clock.day_length_min is under 10 s");
        }
        const jsonlite::Array& list = root.array("items");
        for (size_t i = 0; i < list.size(); ++i) {
            const jsonlite::Value& e = list[i];
            ItemDef d;
            d.id = e.string("id");
            d.name = e.string("name");
            if (e.has("description")) d.description = e.string("description");
            if (e.has("stack")) d.stack = int(e.number("stack"));
            if (d.stack < 1) throw std::runtime_error(d.id + ": stack must be at least 1");
            const std::string kind = e.has("kind") ? e.string("kind") : "resource";
            if (kind == "food") d.kind = ItemKind::Food;
            else if (kind == "tool") d.kind = ItemKind::Tool;
            else if (kind == "resource") d.kind = ItemKind::Resource;
            else throw std::runtime_error(d.id + ": kind is food, tool or resource");
            if (d.kind == ItemKind::Food) {
                const jsonlite::Value& f = e.at("food");
                d.food.health = float(f.number("health"));
                d.food.stamina = float(f.number("stamina"));
                d.food.regen = f.has("regen") ? float(f.number("regen")) : 0.0f;
                d.food.duration_s = float(f.number("duration_s"));
                if (d.food.duration_s <= 0.0f) throw std::runtime_error(d.id + ": food.duration_s must be positive");
            }
            if (d.kind == ItemKind::Tool) {
                const std::string use = e.string("use");
                if (use == "axe") d.use = ToolUse::Axe;
                else if (use == "torch") d.use = ToolUse::Torch;
                else throw std::runtime_error(d.id + ": a tool's use is axe or torch");
                d.stack = 1;
                if (e.has("tool")) {
                    const jsonlite::Value& t = e.at("tool");
                    if (t.has("damage")) d.tool.damage = float(t.number("damage"));
                    if (t.has("reach_m")) d.tool.reach_m = float(t.number("reach_m"));
                    if (t.has("impact_s")) d.tool.impact_s = float(t.number("impact_s"));
                    if (t.has("recover_s")) d.tool.recover_s = float(t.number("recover_s"));
                    if (t.has("anim")) d.tool.anim = t.string("anim");
                    if (t.has("anim_s")) d.tool.anim_s = float(t.number("anim_s"));
                    if (d.tool.damage < 0.0f || d.tool.reach_m <= 0.0f)
                        throw std::runtime_error(d.id + ": tool damage >= 0, reach_m > 0");
                    if (d.tool.impact_s < 0.0f || d.tool.recover_s < 0.0f)
                        throw std::runtime_error(d.id + ": tool impact_s and recover_s >= 0");
                }
            }
            if (e.has("icon")) {
                const jsonlite::Array& ic = e.array("icon");
                for (size_t k = 0; k < ic.size(); ++k) d.icon.push_back(parse_shape(ic[k]));
            }
            if (find(d.id) >= 0) throw std::runtime_error(d.id + " twice");
            items_.push_back(d);
        }
        if (root.has("start")) {
            const jsonlite::Array& st = root.array("start");
            for (size_t i = 0; i < st.size(); ++i) {
                StartItem s;
                s.id = st[i].string("item");
                if (st[i].has("count")) s.count = int(st[i].number("count"));
                if (st[i].has("slot")) s.slot = int(st[i].number("slot"));
                if (find(s.id) < 0) throw std::runtime_error("start names unknown item " + s.id);
                start.push_back(s);
            }
        }
    } catch (const std::exception& e) {
        error = std::string(path) + ": " + e.what();
        return false;
    }
    return true;
}

int ItemTable::find(const std::string& id) const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].id == id) return int(i);
    return -1;
}

}  // namespace player
}  // namespace engine
