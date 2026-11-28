# ADR-008: Particles, ambient fields, and smoke as a medium

Status: **proposed**, 2026-11-28. Nothing is built; there is no particle,
decal or smoke code in the tree. Everything below is the plan. Tuning goes
in data (decision 3), never in the golden hash: nothing here is upstream
of it.

Amended twice the same day, before any code. First after *"is this open
ended enough to accommodate ideas we have not had yet?"* -- which it was
not: the architecture held, but the INTERFACE had been written around the
six effects that prompted it, so kinds became data (decision 3), ambient
fields became a category of their own (decision 5), and what the design
forecloses was written down (decision 13). Then after *"can particles act
as light sources?"* -- which decision 4 had forbidden outright and should
not have: decision 7 is the answer, and media gained an emission channel
(decision 6). The reasoning for each is kept in "Alternatives considered",
so the arguments are not had twice.

## Context

The world has fires, falling trees, an axe that bites wood and blows that
land on flesh, and nothing in the air to show for any of it. Wanted, in
rough order of how soon: sparks off a fire, leaves shaken loose when a tree
is struck, dust when one comes down, wood chips from the axe, blood from a
blow, and **smoke** -- which is not like the others.

**Those six are the occasion for this design, not its specification.**
Playtesting will invent effects nobody has thought of, and a system where a
new effect costs a rebuild stops accumulating ideas. Decision 3 exists
entirely because of that.

What already exists and shapes every decision here:

- **A shared gust field.** `engine/render/wind.h` compiles as C++ AND HLSL
  and is sampled at `p - drift`, so a gust is seen to cross a meadow and
  then a wood at the air's own speed. Grass, trees and the sea already read
  it. Anything blown must read the same one, or a gust crosses a wood
  without touching the smoke standing in it.
- **A camera-centred volume of the world as it is now.** The near field
  (`engine/render/near_field.h`) keeps 128 x 64 x 128 voxels of 1 m --
  the field plus the player's edits -- as occupancy with mips to 16 m
  (`g_nf_occ`, t27), toroidal, refilled a block at a time as the camera
  moves. One fetch says whether a point is solid.
- **A per-column map of the real top surface** (`g_nf_top`, t28), read by
  `nf_depth(p, out dh)`: how far a point is under the rock over it.
  Indoor/outdoor is already a texture fetch, per point.
- **A flood that knows the way out.** `engine/render/sky_access.h` gives
  every air cell under the top surface the length of the **shortest path
  through air to the open sky**, exact within the reach
  (`underground.sky_reach_m`, 96 m), `kNoSky` (255) past it or sealed. It
  runs on its own thread, in 8 m blocks of 512 cells, and an edit forgets
  only the cells it can have changed. The GPU holds it as a byte a voxel
  (`u_sky`).
- **A lit, temporally stable froxel volume.** `cs_fog_inject` (fog.hlsl,
  160 x 90 x 128 froxels, log-spaced to 6 km) already gives each froxel the
  sun through the cascades and the clouds' shadow, the sky and ground
  through the near field's own openness, **every local light in its cluster
  through that light's own shadow**, the lightning flash, and a reprojected
  1:12 history. `cs_fog_integrate` accumulates it front to back and the
  composite applies it at each pixel's view depth.
- **A precedent for each category.** Rain (`engine/render/rain.h`) is
  100,000 stateless drops, a pure hash of index and time wrapped into a box
  round the camera -- which is decision 5 already built, for one effect.
  The trample map (`engine/foliage/trample.{h,cpp}`) is world state in a
  toroidal texture with two decay rates, and its budget rule is the one
  decision 3 borrows. Foliage bakes tiles and emits from them on the GPU.
- **Data tables with behaviour in Lua are how this codebase extends.**
  Biomes, trees, rocks and forage in `archetypes.json`; items in
  `items.json`; creatures in `creatures.json` with their behaviour in
  `game/scripts/creatures/*.lua` against a documented `engine.*` surface
  (`engine/ui/script_api.h`). Particles are not special.
- **Three scene targets.** Since the wet world every scene PSO declares
  direct, ambient and the puddle mask (`scene_out.hlsli`); the targets'
  ALPHA carries how much contact shadow and AO a surface takes.

## Decisions

### 1. Three categories, not one system -- split by physics, not by name

A spark, a chip, a leaf, a dust mote and a blood droplet are one thing: an
independent point, spawned at a moment, with a velocity and a lifetime,
aware of nothing but the ground. Snow, pollen, ash fall and fireflies are
NOT that -- they have no spawn moment and no lifetime; they are a field
that simply exists wherever the player is. Smoke is neither: it occupies
space, occludes what is behind it, and has to know where the walls and the
exits are.

Three different answers to "what is stored", so three implementations:

| | layer | state | drawn as |
| --- | --- | --- | --- |
| A | **the substrate** | none; it is four queries | -- |
| B | **ballistic** | a persistent buffer, spawned and freed | unsorted cards |
| C | **ambient fields** | **none** -- a hash of index and time | unsorted cards |
| D | **media** | a sparse world grid | the mist's froxels |

The split is physical, not a taxonomy of today's effects, which is why it
should survive effects nobody has named yet. **A shape that fits none of
them is a fourth renderer over the same substrate, not a tweak** -- see
decision 13 for the two already known.

### 2. Layer A, the substrate: `engine/particles/environment.h`

Ordinary C++ that also compiles as HLSL, in the style of `water_wave.h`,
`wind.h` and `tree_sway.h`. Four questions, one answer each, the same
answer on the CPU and the GPU, read by all three layers above it:

| question | what it reads |
| --- | --- |
| the wind here | `wind_gust_at(p - drift, ...)`, the field everything else bends in |
| indoors or out | `nf_depth(p, dh)`: how far under the real top surface |
| is this solid | `g_nf_occ`, 1 m occupancy with mips |
| the way out | `u_sky`: metres to the sky by path |

These are facts about the WORLD, not about any effect, which is why this
layer is expected to outlast everything built on it.

**Indoor/outdoor is NOT a raycast from the emitter.** `nf_depth` answers it
per PARTICLE for one fetch, and per particle is the answer that is wanted:
smoke leaving a cave mouth should take the wind up gradually across the
threshold, not all at once when the emitter's ray clears the rock. Past the
near field's 128 m volume, fall back to the heightfield GI's top map and
the canopy map's vertical transmittance -- which is also the "under crowns,
so less wind" answer, and is what the rain already uses to keep drops out
from under a canopy.

Sharing the header is the point. A second copy of a formula drifting from
the first is this codebase's recurring bug: it is why `wind.h` and
`tree_sway.h` were lifted out of their shaders in the first place.

### 3. A KIND IS A ROW OF DATA, not a C++ type

**This is the decision that makes the rest open-ended, and the one the
first draft left implicit.** `game/data/particles.json`, beside
`items.json` and `creatures.json`, one row per kind:

```json
"wood_chip": {
  "layer": "ballistic",
  "draw":  "cutout",
  "mesh":  "chip",
  "life_s": [2.0, 4.0],
  "emit":  { "shape": "cone", "half_angle_deg": 35,
             "speed": [2.0, 5.0], "count": [6, 12] },
  "forces": ["gravity", "drag", "wind"],
  "collide": "bounce",
  "restitution": 0.25,
  "on_hit": { "kind": "dust_puff", "chance": 0.3 },
  "budget": 2000
}
```

The C++ knows a **closed vocabulary of modules** -- emit shapes, forces,
collision responses, draw modes -- and the row composes them. **A new
effect is a row and zero C++.** A genuinely new MODULE is C++, and that is
meant to be the rare case; if it stops being rare the vocabulary is wrong.

Three things follow, all of which are cheap now and awkward later:

- **Anyone can emit, from either language.** One public
  `ParticleSystem::emit(id, where, ...)`, plus `engine.emit(id, x, y, z)`
  on the Lua surface next to `engine.player()` and `engine.messages()`, so
  a creature behaviour or a gameplay script invents effects with no
  rebuild. `land_swing`, `impacts_frame`, the crown shed, `bumps()`,
  `Piece::struck` and `on_hurt` are then simply **the first six callers**,
  not a fixed list.
- **`where` is a TRANSFORM, optionally ATTACHED.** A fixed world point
  cannot express fire on a burning creature, blood dripping from a wounded
  mob, or smoke off the torch in a moving hand. An emitter may name a
  body, or a creature and a bone, and the system moves it each frame --
  `set_torch_anchor` already does exactly this for the carried light.
- **`on_hit` names another kind, so particles emit particles.** A chip
  striking stone throws a puff; a droplet hitting ground leaves a decal
  (when decals exist); a spark landing in dry grass starts a fire (when
  fire spreads). **Combinations nobody anticipated become data rather than
  code**, which is the whole point of the exercise.

**With that comes a budget, or a playtester will tank the frame.** Every
row carries one, there is a global cap, and the rule is the trample map's,
which already solved this: *"The order is the budget: 64 capsules a frame,
player first, because `add` stops when it is full."* Effects the player
caused outrank ambience. A kind over its cap drops the oldest, never the
newest -- a burst must always show something.

### 4. Layer B, ballistic particles

GPU-simulated, persistent buffer, CPU-fed spawn queue.

- **Spawn on the CPU, simulate on the GPU.** Unlike tree placement, nothing
  here is gameplay-authoritative -- no particle is ever chopped, picked or
  saved -- so there is no reason to keep the simulation on the CPU. The
  game pushes `{kind, transform, count, seed}` into a ring buffer; a
  compute pass appends to a free list and a second integrates every live
  particle.
- **Lit through `lighting.hlsli` like everything else** -- `gi_diffuse`,
  the local lights' cluster walk, the near field's probes -- so a chip in a
  cave is dark and a chip by a campfire is lit. Emissive kinds (sparks)
  skip it: additive, ambient alpha 0 so GTAO does not darken them.
  **Emissive and illuminating are different things**: by default such a
  kind is bright on screen and lights nothing around it. What it takes to
  actually cast light, and what that costs, is decision 7 -- which the
  first draft got wrong by forbidding it outright.
- **Three scene targets, like every other scene PSO**, with the puddle mask
  written zero: a chip falling over a puddle should clear it, as grass and
  rock do.
- **Collision against the occupancy volume**: one fetch, bounce, friction,
  settle, then `on_hit`. Past the volume, the heightfield GI's top map, as
  the rain's splashes already do.
- **Budget: ~0.2 ms at 1440p** for tens of thousands, measured with
  `--shot ... --gpu-profile` under its own mark.

### 5. Layer C, ambient fields: stateless, and already prototyped

Snow, pollen, ash fall, drifting spores, fireflies, insects over a bog.
These have no spawn event and no lifetime: they are everywhere the player
is, always. Running them through a spawn queue and a free list would be
strictly worse than what the rain already does.

**So they are the rain's pattern, generalised**: a pure hash of index and
time, wrapped into a box round the camera, nothing stored, 100,000 of them
for a fraction of a millisecond. A row in `particles.json` with
`"layer": "ambient"` gives the box, the count, the fall and drift, and the
draw -- and a **where**: a biome weight, an altitude band, a weather
condition, or a distance from something (ash near a volcano, snow in
glacier, pollen in meadows in spring). Density is a field evaluated at the
camera, not a spawn decision.

This category was missing from the first draft entirely, which is how a
design gets quietly tied to its examples: all six founding effects happen
to be Layer B or D.

### 6. Layer D, media: a sparse world grid

**Smoke is the first instance of this layer, not the layer.** Density,
temperature and albedo advected against walls describes steam, a dust
cloud in a collapsing cave, poison gas, a fog bank off the sea and cave
damp equally well -- different numbers, same code. The code should be
named for the medium, not for smoke.

State is density and temperature, 1 m cells, in **sparse 8 m blocks -- the
sky access flood's own block size** (`SkyAccess::kBrick`, 512 cells), so a
block's escape data is the result that already exists for it. Blocks are
allocated only where there is medium, only within the near field's reach,
and freed when they empty.

Advection is semi-Lagrangian: one backtrace, one fetch, CFL-clamped.
Temperature decays and buoyancy follows it, so a plume rises fast at the
fire and lazily at its top. The velocity is **derived, not solved** --
there is no pressure projection and no fluid simulation. A medium with
negative buoyancy (gas heavier than air) pools in hollows by the same
arithmetic with the sign flipped, which is free and worth having.

**A medium carries EMISSION as well as density and albedo**, one more
channel, and it is what makes flame possible at all. Fire is a hot
emissive medium: without an emission term a flame can only be faked as a
grey cloud with a light hidden inside it. In `cs_fog_inject` an emissive
froxel simply adds its radiance before the phase-weighted sum, so it is
volumetrically self-illuminated, it glows through its own smoke, and it
needs no light slot to look right. **It still lights nothing around it**
-- for that it wants one local light beside it, which is decision 7, and
the division of labour there is the point: the emission is the LOOK, the
light is the ILLUMINATION, and trying to make either do both is what
makes fire look wrong.

### 7. Particles as light sources: three tiers, one budget

**Emissive and illuminating are different things**, and the first draft
collapsed them by ruling that "a spark must not become a real light". That
is right as a DEFAULT and wrong as a rule: it would have had someone
reinventing from scratch a pattern that is already in the tree, or
concluding the engine could not do this at all.

**The precedent is the glow mushrooms**, whose header describes almost
exactly what a particle group wants: *"each cluster is one local light at
its middle... its lumens the mushrooms LEFT times `lumens_each`, shadowed
like any other so it does not light through rock -- while it is among the
24 most important (lumens over distance squared)."* One light for nine
mushrooms, dimming as they are picked. A spark burst is the same shape: a
group of emitters inside a metre or two, dimming as it dies.

So a row in `particles.json` may carry a `light`, and there are three
tiers:

| `light` | what it does | cost |
| --- | --- | --- |
| *(absent)* | additive emissive: bright on screen, illuminates nothing | free |
| `"group"` | ONE light at the live particles' centroid, lumens summed | 1 slot of 256 |
| `"each"` | one light per particle, hard-capped at a few | 1 slot each |

- **`"group"` is the default for anything bursty**, and it is very nearly
  free visually: at any distance where a shower's illumination reads at
  all, the shower is smaller than its own falloff, so a point at the
  centroid and the true sum are indistinguishable.
- **`"each"` is real but small** -- a thrown torch, a dropped ember, a
  single magic bolt. Two to four, not two hundred.
- **Thousands of individually lighting particles are not a budget to tune
  but a structural impossibility.** The clustered forward path is O(lights
  in the cluster) per pixel; `--lights-report` already measures 255
  campfires at 46.5 lights per lit cluster costing 4.42 ms, and 256 is the
  whole budget, which fires and torches need. Anyone wanting "every ember
  lights the ground" is asking for a different technique -- light the
  ground from the MEDIUM, or splat into an irradiance volume -- not for
  more slots.

**What a group light gets for free**, because it is an ordinary
`LocalLightGpu` and nothing about it is special:

- **volumetric glow** -- `cs_fog_inject` walks every light in the froxel's
  cluster with its own shadow;
- **bounce on nearby walls** -- the near field's fine hits are lit by the
  frame's first 32 shadowed lights;
- **terrain, trees and grass**, which already walk their pixel's cluster.

Movement is handled too: `LocalLightGpu` carries `moving` ("moved within
60 frames... the near field's far hits skip it"), and the fast-light path
gives probes within 10 m a 0.1 s memory so the bounce tracks instead of
lagging. That machinery exists because of the carried torch, and a spark
shower is the same problem.

The rules, which are what keep this from eating the frame:

- **Particle lights enter the EXISTING importance sort** -- lumens over
  distance squared -- as ordinary lights, with no reserved budget of their
  own. A fire 5 m away then beats a spark shower 40 m away automatically,
  and there is no second budget to balance against the first.
- **Unshadowed by default**, so they light through walls. For a burst a
  metre from the fire that threw it, nobody will see that. Shadowed is
  opt-in and stays rare: 24 slots, and 64 campfires cost 2.98 ms
  unshadowed against 3.55 with eight shadowed.
- **Every row declares a maximum light count and a lumens ceiling**,
  enforced like the per-kind particle budget of decision 3.

**Fire wants three mechanisms, not one**, and this is the clearest case
for the whole split: the LIGHT is a single well-placed local light (which
campfires already have), the LOOK is an emissive medium (decision 6), and
the SPARKS are emissive-only particles lighting nothing. Each does the one
thing it is good at, and none pretends to be the others.

### 8. Fill and spill: the sky-access gradient IS the escape route

Per cell, in order:

1. **Buoyancy up**, proportional to temperature.
2. **Blocked above?** One occupancy fetch. If so, spread laterally along
   the ceiling instead -- which is what fills a chamber from the top down,
   as real smoke does.
3. **The lateral direction is down the sky-access gradient.** Medium that
   cannot rise follows decreasing distance-to-sky and arrives at the exit.
   This is the whole fill-and-spill mechanic and **the data is already
   built, already threaded, already edit-aware**.
4. **`kNoSky` means it simply fills.** Sealed, or no exit within 96 m: the
   smoke has nowhere to go and the chamber fills. That is correct, and it
   is a mechanic -- smoke yourself out of your own cave, then dig a vent.
5. **Outdoors** (`nf_depth` says nothing is over it) buoyancy hands over to
   the wind field and the plume drifts and shears.

The gradient is a good escape route and not a perfect one: it points at the
NEAREST opening, which may be below the smoke, where real smoke would hold
at the ceiling and only sink once the chamber was full. Buoyancy running
first is what keeps that honest -- the gradient is only consulted once a
cell cannot rise.

### 9. Media are drawn by the mist, not by cards

The medium raises `sigma_t` in `cs_fog_inject` and carries its own albedo
(soot is dark, steam is not). Free, by doing it there:

- the fire lighting its own column from inside, through the local lights'
  cluster walk that kernel already does, each with its own shadow;
- the sun, the cascades and the clouds' shadow, so a plume in the open is
  lit from the right side and shadows itself;
- correct depth compositing against the whole scene, and the temporal
  history, so it does not boil;
- **no sorting, and no overdraw** -- the cost is the froxel count, not the
  amount of smoke. This engine has fought translucent overdraw hard enough
  in the trees to be worth avoiding by construction;
- a seamless meeting with the mist that is already there, instead of a
  visible edge where one system's fog stops and another's starts;
- **genuine occlusion**, which cards fighting a depth prepass cannot give.

**Its one weakness, and the fix.** Froxels are view-aligned and log-spaced,
so the crisp curling first metre over a flame will be blobby. Put that bit
in Layer B as a handful of ordinary lit cards: a volumetric column with
particle wisps at its source. The hand-over is by distance from the
emitter, not by distance from the camera.

### 10. Distance LOD is not optional

Sky access is requested per 8 m block round the camera and the occupancy
volume is 128 m across. A fire 500 m away has neither. Three tiers:

| distance | the medium |
| --- | --- |
| inside the volume | the grid: buoyancy, walls, fill and spill |
| past it, in view | a cheap drifting plume injected straight into the froxels, no grid |
| far | nothing, or a billboard column under the haze |

The expensive treatment applies only where the player can walk into the
smoke, which is the only place the difference can be seen.

### 11. Buildings must be VOXELISED -- not necessarily stored as voxels

**Noted now, deferred by agreement; nothing is built for it.**

Fill and spill ride entirely on the occupancy volume and the sky-access
flood, and today **both read the field plus the EDIT STORE** -- the field
at a 1 m voxel, the player's edits at the 1 m lattice points. A building
invisible to both means smoke pours straight through its walls (and, worse
and sooner, the near field's probes light the inside of a hut with open
sky).

`docs/gaps.md` already leans the other way for good reasons: *"NPCs and
player constructions will NOT edit voxels; constructions are expected to be
their own layer."* **That is not in conflict with this**, and the
distinction matters: smoke needs buildings VOXELISED, not STORED as voxels.

**The precedent is the navmesh, which already does exactly this.**
`engine/nav/nav_tile.h` keeps no props in the field: the main thread
GATHERS the trunks, rocks, stumps and resting logs, hands the worker a flat
list, and the worker stamps them as solid spans inflated by a whole cell.
The worker never touches engine state. A building layer of its own can feed
the same kind of list to two more consumers:

- **the near field's fill**, where a block's cells are evaluated -- a cell
  inside a building's hull is written solid, as an edit would be;
- **the sky-access flood's batch**, which already reads the edit store
  under a shared lock and can take buildings as a third source.

What buildings must then offer is small and nothing they would not want
anyway: a cheap "is this 1 m cell inside something solid" query over a box,
and a **generation that bumps when one is placed, changed or destroyed** --
at which point the flood forgets the cells within reach of it, which is the
machinery a dig already uses. A building that MOVES would re-flood, so
moving structures are the case to think about before allowing them.

None of this is work the smoke imposes on its own: **the navmesh needs
buildings stamped regardless**, or creatures will path through walls. Smoke
is a second consumer of one stamp list.

It blocks nothing here. Smoke in caves and in dug tunnels works with what
is in the tree today.

### 12. Decals are a separate feature, named here and not decided

"Blood splats" is two features. The airborne droplets are Layer B. The
mark that stays on the ground is a decal, and there is no decal system.

The trample map is the precedent for the shape it should take: a toroidal
camera-centred texture of world state with a decay, read by whichever
shader is already drawing that surface -- which costs one fetch in shaders
that already run, needs no new draw and no sorting, and forgets anything
that scrolls past its edge. Blood, scorch marks and char all fit it.
`on_hit` (decision 3) is how a particle would ask for one. Not decided
here.

### 13. What this design does NOT support, said plainly

So that it is a known cost rather than a discovery mid-playtest:

- **Soft alpha.** Layers B and C sort nothing, which is only free because
  additive blending is order-independent and alpha-TESTED cutouts write
  depth. **What is excluded is soft, semi-transparent alpha** -- and it
  went unnoticed in the first draft precisely because all six founding
  effects are cutouts or additive. The first idea wanting a soft puff of
  dust, or a translucent wisp too small for froxels, hits this. Escapes,
  cheapest first: push it to Layer D; accept additive; weighted-blended
  OIT, which is a real fallback and a real cost.
- **Ribbons and trails** -- an arrow's wake, a sword arc, a whip, a comet.
  Connected geometry extruded along a path over time: a fourth renderer
  over the same substrate, not a particle kind.
- **Beams** -- magic, which `local_lights.h` already anticipates ("Torches,
  fires, later magic"). Same answer as ribbons.
- **Liquids** -- pouring, streams, splashing volumes. Water is a half-space
  at sea level (ADR-006 superseded) and nothing here changes that.

None of these is blocked by anything above; each is additive work, which is
the test that the split in decision 1 was drawn in the right place.

## Phases, each with a headless test

1. **The substrate.** `environment.h`, dual-compiled, with a CPU-side test
   that its four queries agree with the GPU's at sampled points -- the
   `--probe-field` pattern, a tolerance comparison, never a hash.
2. **The data table and one kind: wood chips.** `particles.json`, the
   module vocabulary, `emit` from C++ and Lua, the per-kind budget. Spawn
   from `land_swing`, collide, settle, fade. `--particle-test`: a chop,
   counts live and settled, a kind added by editing JSON only (the test
   writes its own table, as `--survival-test` does, so retuning the
   shipping data moves nothing), a still, and the pass's cost.
3. **The rest of the founding kinds**: sparks (additive, from a campfire),
   leaves (from a struck tree), dust (from `impacts_frame` when one lands,
   beside the trample stamp that already fires there), blood (from
   `on_hurt`), and one `on_hit` chain to prove kinds compose.
4. **A kind that casts light** (decision 7): the spark burst given
   `"group"`, in a cave at night so the walls show it. The test is
   `--lights-report`'s own: the burst takes ONE slot, the row's lumens
   ceiling holds, it is sorted out by a nearer fire rather than by a rule
   of its own, and the frame cost is within noise of the same burst
   emissive-only. A still each way (`build/particle_light.bmp`), since
   "the walls lit up" is not a number.
5. **One ambient field**: snow or pollen, by biome, no spawn queue. The
   test is that it costs what the rain costs and holds still in the world
   while the camera moves (two `--no-taa` stills a metre apart).
6. **Media in the open**: the grid, buoyancy, wind, injection into the
   froxels. A campfire plume. `--smoke-test` with a still and a cost.
7. **Media under cover**: the ceiling rule and the sky-access gradient.
   The test is the shipping world's forest cave and a dug tunnel -- a fire
   in the chamber fills it and the smoke leaves by the mouth, and a fire in
   a sealed chamber fills it and stays. Both are already-reachable places
   with `--cave-walk` and `--dig-tunnel`.

## Alternatives considered

**No particle lights at all.** The first draft's flat rule, and wrong:
"emissive, never illuminating" is a sensible default dressed up as a
constraint, and it hid the fact that the aggregate pattern was already
shipping in `glow_mushrooms.h`. Kept as the default (a row with no
`light`), dropped as a rule.

**A light per particle, with a bigger light budget.** Rejected: it is not
a tuning problem. The clustered forward path is O(lights in the cluster)
per pixel, and the measured cost at 46.5 lights per lit cluster is already
4.42 ms; raising `kMaxLights` makes that worse rather than better, and the
256 are spoken for by fires and torches. A hundred emitters inside one
metre want one light, which is also what they look like.

**A separate light budget reserved for particles.** Rejected as a thing to
balance forever: two budgets need a policy for how they trade against each
other, and the engine already has the right policy for free -- one
importance sort on lumens over distance squared, which a particle light
joins as an ordinary light and wins or loses on its merits.

**Kinds as a C++ enum, with each effect written as code.** The first
draft's unstated assumption. Rejected on the second reading: it makes every
idea from a playtest a rebuild, and a system that charges a rebuild per
idea stops collecting ideas. It also has no answer for a creature script
wanting an effect of its own. The whole codebase already decided this the
other way -- biomes, items and creatures are all tables.

**Two categories (ballistic and medium), with ambient effects forced into
the spawn queue.** Also the first draft. Snow and pollen have no spawn
moment and no lifetime; giving them one means a queue, a free list and a
lifetime per flake to express a field that is simply everywhere. `rain.h`
already demonstrates the better answer at 100,000 drops.

**Smoke as particles, with a fill rule.** One system, simpler to start.
Rejected: filling a chamber takes a great many large soft cards, which is
exactly the translucent overdraw the trees' depth prepass was built to kill;
it needs inter-particle pressure to fill anything convincingly; it needs
sorting against the scene; and it would be lit by a different path from the
mist it stands in, so the two would not match where they meet. The froxel
route is cheaper, better lit and bounded.

**A real fluid solver** (pressure projection on the medium grid). Correct,
and far more than is needed: the behaviour wanted is "rise, then fill, then
find the way out", and the flood already answers the third part exactly.
A solver would also have to be told about walls that the occupancy volume
already describes.

**Its own volumetric pass, separate from the mist.** Would duplicate the
sun, cloud-shadow, local-light, sky-share and temporal machinery of
`cs_fog_inject` -- several hundred lines that are already written and
already debugged against `--leak-test`.

## Later

Medium pooling against a built ceiling (decision 11); smoke that puts the
player off, stings, or is seen by creatures; wind shelter behind terrain
and buildings; decals (decision 12); ribbons, beams and soft alpha
(decision 13); particles writing motion vectors -- nothing in this engine
does, and fast particles will ghost under TAA, as the character already
does; smoke casting a shadow; particles saved across a session (none are,
by choice -- they are moments, as the felling debris is); audio, which
`on_hit` is the natural place to hang.
