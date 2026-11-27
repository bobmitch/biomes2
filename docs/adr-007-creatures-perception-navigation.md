# ADR-007: Creatures -- zones, spawning, perception and navigation

Status: **accepted**, 2026-11-27. Phases 1 (the player's sound and sight)
and 2 (zones, spawning, freezing, saving, drawing) are built -- CLAUDE.md,
"Creatures and stealth", has what was measured; the rest is the plan. Gameplay follows Valheim by default,
as the survival rules do; every number below is data, in
`game/data/creatures.json` or `game/data/items.json`, never in the golden
hash.

## Context

The world has a player, things to pick and trees to fell, and nothing
that moves on its own. Creatures are next, and the systems under them --
where they spawn, how they find the player, how they get from A to B --
should be solid before the creatures multiply. Kinds to come: walkers,
swimmers, walkers that also swim, water-only creatures, fliers; blind
creatures, keen-eared ones, ones that see at night.

Facts that shape the design:

- **Zones exist already.** Forage and rocks stream 64 m tiles round the
  player on workers (`engine/props/forage.h`, `engine/rocks/`), each with
  its own map. A plant or rock is a pure function of its cell; only what
  the player changed is stored.
- **Physics has ONE character.** Terrain collision covers +-56 m of the
  player at 0.5 m voxels, +-112 m at 1 m and +-224 m at 2 m
  (`kCollisionMaxLod` 2); trunk and rock colliders exist within 40 m of the
  player only (`TreeFelling::kColliderRadius`). Fallen logs are Jolt rigid
  bodies.
- **The animator's whole input is what physics did** (`LocomotionInput`:
  speed, grounded, swimming), so it drives a creature body as it is. The
  clip library has crouch (`Crouch_Idle_Loop`, `Crouch_Fwd_Loop`, 0.75
  m/s), attacks, hits and a death.
- **Water is a half-space at sea level** (ADR-006, superseded): every
  column whose floor is under sea level holds water at sea level.
- `docs/worldgen-design.md` keeps spawn tables out of the generator's
  contract on purpose: they are gameplay, keyed by biome NAME in data.
- A game hour is 75 real seconds (30-minute day).

## Decisions

### 1. Zones are the unit of spawning, simulation and saving

One shared grid of 64 m zones (the forage and rock tiles' grid; they move
onto it in phase 2).

- **Active zones**: centre within 128 m of the player (about a dozen; 11
  at the spawn), released past 160 m so a walk along the edge does not
  thrash. Valheim's active area is about 5 x 5. Only active zones spawn and simulate.
- **A zone that leaves the active set FREEZES** its creatures exactly as
  they are -- position, facing, health, AI state, target, home -- and
  keeps them in its record. Coming back resumes them. The records are
  saved with the world (F6 / F9, `build/mobs.bin`, under the field id).
  **This is deliberate: a player cannot shake a chase by leaving and
  coming back later (or, once it exists, by teleporting); they come back
  to the creature as they left it.** A creature belongs to the zone it is
  IN when its zone freezes, not the one it spawned in.
- Frozen zones do not accumulate spawn time: nothing is caught up on
  return (Valheim's behaviour, and it keeps a long absence from returning
  to a crowd).

### 2. Spawning: per zone, per rule, with an area cap

A rule in `creatures.json`:

| field | meaning | mimic |
| --- | --- | --- |
| `biome` | where (by name; blended weight >= 0.6 at the point) | meadows |
| `max_per_zone` | cap counted in the zone | 2 |
| `max_active` | cap over every active zone together | 4 |
| `interval_h` | game hours between attempts, per active zone | 1 |
| `chance` | an attempt's chance to spawn | 0.35 |
| `hours` | [from, to) of day, wrapping; [0, 24) any | [0, 24) |
| `group` | [min, max] spawned together | [1, 1] |
| `min_player_m` | never nearer the player | 20 |
| `min_same_m` | never nearer one of the same kind | 24 |

Starting values for the mimic, replacing "up to 3 a zone, 1 an hour": with
a dozen active meadow zones, 1 an hour per zone at a cap of 3 would put a
mimic into the active area about every 6 s of play until ~36 stood round
the player. With the values above a given zone sees its first mimic after
~3 game hours on average, but a dozen zones together reach the area cap of
4 in about 1.5 game hours (~2 real minutes; measured), and it is the cap
that holds the number. Both caps and the rate are data; a first guess.

**Where**: a random point in the zone, on dry or wadeable ground the
creature's movement can use (its navmesh, phase 3; until then the
generated surface, slope under the trees' limit, clear of trunks, rocks
and channels), with the rule's distances, and **not visible to the
player**: outside the camera's frustum widened by 10 degrees, OR with the
line from the eye to both the creature's head and feet blocked (physics
ray: terrain, trunks, rocks). An attempt that finds no such point in a few
tries spawns nothing. Rolls are seeded by zone and attempt (a hash, as the
lightning schedule is), so tests and stills reproduce.

### 3. Perception: the player is a source of sound and of sight

The player side is `engine/player/stealth.{h,cpp}` (phase 1): no GPU, the
window passes in what the body did, a headless test drives it.

**Sound, in decibels.** The player has a SOURCE LEVEL L (dB at 1 m) every
frame: a continuous level by gait, and the max of decaying SPIKES for
events (`items.json` `stealth`):

| gait / event | L dB | | event | spike dB |
| --- | --- | --- | --- | --- |
| still | none | | jump | 62 |
| crouch walk | 35 | | landing | 64, +2 a metre fallen |
| walk | 50 | | swing of the axe | 82 |
| jog | 60 | | a dig | 78 |
| sprint | 70 | | | |
| swimming | 55 | | | |
| wading | +4 on the gait | | | |

Spikes decay at 30 dB a second. The level at a listener d metres away is
inverse-square: **L - 20 log10(max(d, 1))**. A creature has a
`hear_range_m` (a hard limit) and a `hear_threshold_db` (its acuity, lower
is keener). It hears the player when d <= hear_range and the level there
>= its threshold.

This is the "scaling threshold within the range": what a creature needs
from the source rises with distance -- at its range's edge only a loud
source carries, right beside it almost anything does. The mimic (range 20
m, threshold 34 dB) hears a jog (60) out to its 20 m, a walk (50) to 6 m,
a crouch walk (35) to 1.1 m, and anything louder than a jog only to its
20 m. A deaf creature has range 0. Later: an `ambient_db` from wind and
rain raises every threshold (a storm masks footsteps); the call takes it
already, at 0.

**Sight.** A creature has `view_range_m`, `view_cone_deg` (the TOTAL
width; the mimic's 45 is 22.5 each side) and `dark_gain` (night vision).
It sees the player when, all of:

- the angle from its facing is within half the cone (from its head joint,
  horizontal and vertical together);
- d <= view_range x **visibility** x **light**;
- a physics ray from its eyes to the player's chest and head is clear
  (terrain, trunks, rocks; foliage and crowns do not block yet).

**Visibility** is the player's posture and speed (`items.json` `stealth`):
standing still 0.7, walk 0.85, jog 0.95, sprint 1.0; crouched still 0.35,
crouched moving 0.5; swimming 0.5.

**Light**: full light, full acuity; less light, less. The illuminance on
the player E (lux: the key light's beam if a ray to it is clear, x the
cloud cover; the sky's x the eye's open sky; every local light in clear
view, the carried torch always) times the creature's `dark_gain`, on a log
scale between `dark_lux` (0.05) and `full_lux` (400), floored at 0.1:

| E | light |
| --- | --- |
| day, open (10-100 klx) | 1.0 |
| overcast or deep shade (~400 lx) | 1.0 |
| dusk (~10 lx) | 0.59 |
| a carried torch at night (~60 lx on the body) | 0.79 |
| full moon (0.19 lx) | 0.15 |
| moonless, cave | 0.10 (the floor) |

So a torch at night gives the player away, crouching in the dark is
nearly invisible, and a creature with `dark_gain` 100 sees by moonlight as
others do at dusk. **Night vision** is that one number; creatures that see
ONLY in the dark (light hurts) are a later variant (a gain curve instead
of a gain).

The mimic: view 50 m, cone 45, gain 1. In daylight it sees a sprinter at
50 m, a walker at 42, a crouched walker at 25.

### 4. Navigation: Recast/Detour, built from the field

Recast/Detour (zlib licence) as a submodule in `third_party/recastnavigation`,
built in the engine block like Jolt; worldgen and validate never see it.

- **Input is the terrain's solid spans, not triangles.** Recast rasterises
  triangles into a heightfield of solid SPANS per column; the field gives
  those spans directly. Per 0.5 m column (the edit lattice, so digs land
  exactly), the edited density sampled down a band round the 2.5D height
  (padded where layer 6's overhang or a cave reaches) gives solid
  intervals, handed to Recast's `rcAddSpan`. Then its usual pipeline
  (filter ledges and low headroom, compact, regions, contours, polygons,
  detail) makes a Detour tile. Overhangs, caves and dug ground come for
  free; no GPU readback, so the navmesh is not bound to the physics rings.
- **One tile per zone**, built on a worker when the zone becomes active,
  cached while it stays near, rebuilt when an edit's generation reaches
  it. Its cost is the open question of phase 3 (the field evaluations);
  the fallback is to build from the LOD 0-2 collision meshes where they
  are resident.
- **Static obstacles are stamped into the spans**: every trunk (layer 8,
  a cylinder of the trunk's radius + the root flare) and every rock
  (layer 9, its footprint) in the tile, straight from their pure
  functions -- not only those within physics' 40 m.
- **Fallen logs** are Detour TileCache box obstacles (centre, half
  extents, yaw: a lying log is a yawed box) once their Jolt body sleeps,
  removed if it wakes; while one rolls, creatures steer round it locally.
  Creatures do not push or clear logs, and do not dig (for now).
- **Water**: a column whose walkable floor is more than `wade_depth`
  (1.0 m) under sea level gets a synthetic walkable span AT sea level,
  area SWIM; shallower, the floor is area WADE. A query filter per
  creature: walkers exclude SWIM, amphibious ones include it at a cost,
  water-only ones include only SWIM.
- **Movement domains** (data, `move`): `ground`, `amphibious` (the
  mimic), `water_surface`, and later `water_deep` and `air`, which need 3D
  navigation (a sparse occupancy grid and 3D A*, like the near field's
  volume), not a navmesh.
- **Agent sizes**: a navmesh is built for one radius, height and climb.
  Three classes: small (r 0.3, h 0.8), human (r 0.4, h 1.8, the mimic),
  large (r 1.0, h 3.5); all step 0.5 m, slope 45 degrees. No jumps or
  climbs between polygons yet (Detour off-mesh links, later).

### 5. Simulation by distance

- **Near** (within 36 m of the player, inside the trunk and rock
  colliders' 40 m): a Jolt `CharacterVirtual` each, as the player's; the
  path's next corner is the wanted velocity; full animation.
- **Mid** (the rest of the active area): no physics; the creature moves
  along its corridor with Detour's crowd (local avoidance included), feet
  on the polygon's detail height; AI at a lower rate; animated, skinned
  less often far off.
- **Frozen**: outside the active area (decision 1).

### 6. Behaviour is Lua; the engine routes

As the UI: C++ routes, Lua decides. A creature kind names a script
(`game/scripts/creatures/mimic.lua`) with hooks -- `on_spawn`,
`on_think(dt)` at a rate by distance (5 Hz near, 1 Hz mid), `on_hear(ev)`,
`on_see(ev)`, `on_lose(ev)` -- and calls on the engine: `move_to(p,
gait)`, `stop()`, `look_at(p)`, `face(p)`, `play(clip)`, `home()`,
`player()`, `distance_to_player()`, `random(...)`. Perception, pathing and
movement are C++; per-creature numbers in `creatures.json`. At the
numbers planned (tens, not hundreds) Lua at these rates costs little.

**The mimic** (the player's own model, a tint to tell it apart):

- idle / wander near home;
- HEARS the player -> stops and looks (turns its head, then body, to the
  sound's position) for a few seconds; nothing heard again: back to idle;
- SEES the player within 50 m (its range x visibility x light) -> walks
  towards them; within 40 m -> runs;
- reaches them -> stops at ~2 m and STARES (no combat yet);
- loses sight -> goes to the last place it saw them, looks round, then
  goes home.

### 7. Drawing

The character renderer draws one body. Phase 2 makes it instanced (bone
matrices per instance in a structured buffer) in the main pass, every
cascade and the shadowed local lights' faces, with the body's light-shadow
bookkeeping per instance. **Creatures are the first things that move past
28 m**, where cascades redraw at 15-30 Hz: their shadows lag there until
each cascade keeps its static map as a cached copy and draws the moving
casters over it every frame (CLAUDE.md, Shadows).

### 8. Sound files

There is no audio engine. Sound is a gameplay quantity for now; footstep
and creature sounds will play from the same events (the user is sourcing
open-licensed files).

### 9. Relation to the NPC note in gaps.md

`docs/gaps.md` ("Future design: NPC simulation by distance") sketched
simulation tiers before this ADR, with ambient wildlife dropped out of
range and never saved, and zones CATCHING UP on waking. For creatures this
ADR decides otherwise: they freeze and are saved, with no catch-up, so a
player returns to a chase as they left it. The note's catch-up stays the
plan for persistent NPCs with schedules.

## Phases, each with a headless test

1. **Crouch, and the player's sound and sight.** Left Ctrl toggles crouch
   on land (in water Ctrl still dives); crouch speed 1.4 m/s, no sprint;
   the crouch clips. `Stealth` gives L, visibility and light every frame;
   the HUD shows them; `--survival-test` covers the numbers and the
   perception functions (hearing and sight against the mimic's numbers).
2. **Zones, spawning, freezing, saving, drawing many bodies.** Creatures
   stand still. `--mob-test` spawns, freezes, saves and reloads.
3. **The navmesh**: tiles from the field, obstacles, water areas;
   `--nav-report` (build times, coverage, a path across the spawn's
   meadow, a swim) and a debug overlay.
4. **Movement**: near and mid simulation, the crowd, animation.
5. **Perception and the mimic's script**: `--mob-test` walks the player
   round a mimic at fixed exposures and asserts each transition (heard at
   20 m jogging and not walking, seen in the cone and not behind, walks at
   45 m, runs at 35, stares at 2, loses and returns).

## Later

Night-vision creatures and light-averse ones (decision 3); ambient
masking from weather; foliage and crowns blocking sight; jumping and
climbing links; fliers and deep-water swimmers (3D navigation); combat;
creatures pushing logs or digging; footstep and creature audio.
