# ADR-006: Water below sea level -- dug holes, tunnels and caves

Status: proposed, 2026-11-25. Stages 1 (no sea under a roof, for physics)
and 2 (underwater rendering, a first version) built; the rest not. This records the direction chosen in discussion and
the corrections found while choosing it, so the first implementation starts
from them.

## Context

The sea is 2.5D. Whether a column holds sea is decided from the GENERATED
terrain height and nothing else, in two places that agree with each other
and with nothing the player has done:

- drawing: `water.hlsl` reads `real_terrain_height` at the wave's rest point
  and clips the sheet where the column is above sea level;
- swimming: `PhysicsWorld` (`physics_world.cpp`, the `column_height_` test)
  calls the character in the sea wherever the generated column is below sea
  level, whatever lies between the feet and the surface.

So, today:

| what the player does | what happens | what they expect |
| --- | --- | --- |
| digs a pit inland, below sea level | dry | dry (fine) |
| digs a hole on the beach, below sea level | dry | it fills |
| digs a channel in from the coast | the water stops at the OLD shoreline, a wall of water | the sea runs in |
| tunnels under the seabed, sealed | they SWIM in dry air, rock overhead | dry -- an under-sea tunnel |
| breaks a tunnel through into the sea | nothing | it floods, fast |
| stands under water | the seabed lit like dry land | an underwater view |

Generated caves meet the same rule. Since v19 caves are placed features
(layer 7) that descend from their mouths, and CLAUDE.md records one running
out under the seabed: the cover check keeps its roof closed, so it is dry
rock overhead -- and the generated column there is below sea level, so the
player swims in it. The v10 ridged tunnels (still off) would meet it too.

## What players expect, strongest first

1. **Breaching into the sea floods the tunnel, fast and dangerously.** The
   first thing anyone will try, and the dramatic one.
2. **A sealed tunnel under the seabed stays dry.** Under-sea tunnels are a
   favourite build; today's behaviour is exactly backwards.
3. **Water in a pit, channel or tunnel is calm**, not the open sea's swell.
4. **A pocket of air trapped under a dome stays air** -- somewhere to
   breathe. Memorable, and cheap here (below).
5. **Water does not appear from nowhere and does not vanish into nothing.**

## Decision

**Water is air below sea level that is connected through air to the open
sea.** A connectivity labelling, not a fluid simulation: a sibling of sky
access (`engine/render/sky_access.{h,cpp}`) on the same worker thread, with
the same per-8 m-block requests, the same rule that an edit forgets only
the labels it can have changed, and the same generation stamps on results.

### Solid blocks water completely -- no seepage

A cell that holds ANY solid blocks water, 100%, whatever it is made of.
No permeability, no seepage through sand or soil, no material rules. The
player needs no mental model of materials: water goes where there is an
open path of air and nowhere else. A beach hole that does not break into
the sea stays dry. That is a choice for clarity, made knowingly.

What it means at the edges, which the implementation must make agree in
every reader:

- **The search's cells must be conservative.** Sky access floods 1 m cells
  sampled at a lattice point. For water that is too loose: a wall thinner
  than a cell can fall between two samples and the water would pass through
  rock the player can see. A water cell is passable only if it is air at
  every edit-lattice point it holds (the 0.5 m edit voxels, 27 points a
  1 m cell), so a wall of any thickness that is in the edit store or the
  field stops it.
- **So the smallest breach that floods is one whole open cell.** A
  pinhole the player pokes through (a 0.5 m dig that clears no full 1 m
  cell) shows no water coming through. Reads as surface tension and is
  consistent with the rule. If it looks wrong in play, the lever is a finer
  search where there are edits (0.5 m cells inside edited bricks), not
  seepage.
- **The water reaches the wall, not the last open cell.** The surface sheet
  and the swimmer's test take a point as water if it is in air by the FINE
  density, below the water level, and in a wet cell or a blocked cell
  touching one. The terrain's depth hides the sheet inside rock, so the
  water's edge is the mesh's own wall, not a 1 m staircase. A sliver of a
  dry pocket on the far side of a wall thinner than a cell can show water;
  accepted.
- **One function, every reader** -- as `ww_swash` is shared by the sheet,
  the wet ground and the swimmer. Drawing, physics, the near field and the
  wet look all ask the same "is this point water, and what is its level".

### The sea by construction

The search's targets are cells of the REAL sea: air above the generated
surface in a column whose generated height is below sea level, with no
edited solid over it to sea level (the "open by construction" cells of
sky access, for water). A search from an unlabelled air cell below sea
level goes through passable air below sea level; meeting a sea cell labels
everything it saw wet, exhausting the component labels it dry. Like sky
access it needs a reach cap, and the cap must not decide the answer: a
search that hits the cap leaves its cells "not labelled yet" (dry, drawn
as before), never "dry for good". A flooded network longer than the reach
is the case to measure.

### Every sea-fed surface is at sea level

All water connected to the sea stands at exactly sea level, so drawing it
is the existing sheet at sea level with a 2D "water here" mask in place of
the column test -- no arbitrary water planes. Waves inside are damped by
the search's own by-product, the PATH DISTANCE to open sea (as sky access
keeps distance to the sky): exp(-d / L), so a channel's mouth swells and
its far end and every tunnel are glass. The trains' depth factor and the
swash keep reading the generated column for the open sea, unchanged.

### Air pockets: water fills only air that can vent

A pocket of air under a dome has nowhere to go, so water entering from
below stops. Water fills a cell only if the cell's air can escape --
which is sky access's question. A player's own tunnel is almost always
vented through its entrance, so it floods to sea level, as expectation 1
asks; only a dome or a pocket with no other way out becomes a diving bell.

- **Venting must use the true open/sealed label, not the distance.** Sky
  access step 2 caps its distance at the reach (96 m) and reports "255"
  both past the reach AND when sealed. A tunnel 150 m long, open at its
  land entrance, would read unvented and refuse to flood when breached --
  expectation 1 failing where it matters. Keep step 1's sealed bit beside
  the distance and test that.
- **The bell compresses** (optional, cheap): the air is squeezed by depth,
  about half its volume 10 m down, so the water rises part way into it.
  The search already knows the pocket's volume per height.

### The fill is shown, not simulated

The label is the steady state. On a breach, the component's water level
starts at its floor and rises to sea level at a rate set by the breach's
open cross-section and the component's volume below sea level -- both of
which the search can report. Physics and drawing read the rising level,
so the water rushes in and there is a reason to run. The worker's 0.1-0.6 s
delay before an answer lands is hidden inside the rise.

## Stored water: a pocket's volume

Water depending only on geometry saves nothing -- until a flooded pocket
loses its path to the sea. Then pure recomputation makes it vanish
(expectation 5), and on reload it vanishes even if the session kept it.
So sealed flooded pockets need a record, saved with the edits:

- **Store VOLUME, not level.** A pocket's water is a volume attached to a
  component. Its level is derived: the height at which the component's air
  volume below equals it (the search's per-height air histogram). That one
  number handles merging (volumes add, the level settles over the joined
  shape -- a sealed flooded pocket dug into from a dry tunnel levels out
  into it instead of standing as a wall), splitting (each part keeps the
  water below the split), and later draining (the volume falls).
- **Identity**: re-key on every relabel rather than look for a stable key.
  A record holds a cell of its water; a new component inherits every
  record whose cell it contains. The lowest wet cell is a good choice.
- **Detection**: an edit forgets the labels of every component next to it,
  so the pocket WILL be forgotten and searched again. The worker notes
  which cells were wet (and their component's water volume) before
  forgetting; a component that was wet and now finds no sea path keeps its
  water as a record.

**Today nothing can seal a pocket.** Every dig carves, and the edit store
keeps the deeper of old and new, so air only grows and components only
merge. Sealing needs a way to put solid back -- the store can fill
(`edit_sphere` with positive strength) but no tool does, and player
constructions are expected to be their own layer, not voxels (gaps.md,
the NPC note). **If constructions are not voxels, the search must count
them as blockers**, or water can never be sealed out by a wall the player
builds. That is the first question the construction layer has to answer
for water.

## Stages

1. **Swimming in dry tunnels (now, independent).** Interim fix before any
   search: the character is in the sea only if nothing solid lies between
   the feet and sea level above them -- a short march up through the edited
   density at the feet. Under-sea tunnels become dry today; stage 3's
   query replaces it. **Done**: `PhysicsWorld::set_sea_cover` over
   `Renderer::solid_between`, from 0.5 m over the feet, ~6 us a step;
   `engine.exe --seabed-tunnel` tests it (a sealed tunnel under the
   shallows dry and standing, open water and an open pit in the bed still
   sea), and fails under `BIOMES_SEA_COLUMN_ONLY=1`, the old rule.
2. **Underwater rendering (now, independent).** Extinction and in-scatter
   toward the water's colour, the surface seen from below, light dimming
   with depth. Already a gap for the open sea; flooding has nothing to show
   without it. **Done** (first version): the eye under the sea by physics'
   rule and stage 1's roof test; post.hlsl's `cs_underwater` (the view and
   sunlight legs through the water, the body's glow in closed form, 0.053
   ms at 1440p); the composite lays no air over surfaces; the surface from
   below as Snell's window and total internal reflection of the seabed;
   no rain or bolt underwater. `--seabed-tunnel` case D. Not done: the
   waterline across the lens, caustics, light shafts (gaps.md).
3. **The connectivity flood**: the search, the conservative cells, the
   mask and sheet, the shared point query for physics, wave damping by path
   distance, venting and air pockets, the shown fill. Water a function of
   geometry only. A placed cave that is sealed under the seabed stays dry
   from the same labels; one whose carve meets the sea floods.
4. **Stored volumes**: when anything can seal a pocket (a fill tool, or
   constructions that block water). Sealed flooded pockets keep their
   water, saved with the edits.
5. **Draining as a mechanic**: bailing, then pumps, lowering a record's
   volume. A breach costs a tunnel; plugging it and draining it gets the
   tunnel back -- a disaster turned into a project, which fits a world
   where digging is encouraged.

## Also to carry through

- **Light through water.** The near field's probes and sky access should
  dim through wet cells rather than treat them as air; a flooded tunnel
  should be darker than a dry one, and its surface should let daylight in.
- **The wet world.** Ground a flood has receded from (stage 5) is wet; the
  puddle and wetness code reads the weather's history today, not water.
- **Foliage.** A flooded pit's floor already grows nothing (dug ground is
  bare); a beach channel's banks are left as they are.
- **Validate stays untouched.** Water is downstream of the golden hash:
  derived from the field and the edits, like sky access, never fed back.

## Rejected

- **One flat sea level everywhere** (any air below sea level is water).
  Cheapest, and moats fill anywhere -- but it floods every sealed tunnel and
  every generated cave below sea level, the opposite of expectation 2, and
  undoes the underground's "sealed is sealed".
- **A flow simulation** (cellular water). The sea is effectively infinite,
  every transient is state to save, and the connectivity labels with a
  shown fill give the feel at a fraction of the cost.
- **Seepage** through soil or sand, by material or by depth under the
  generated surface. Realistic for a beach hole, but it asks the player to
  learn which ground leaks; "solid blocks water" needs no explanation.
