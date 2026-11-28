# Known gaps

Open items to revisit, gathered in one place. Each line is the short form;
the section of CLAUDE.md named in brackets has the measurement and the why.
None of these blocks anything that works today. Ordered by area, not by
priority.

Last gathered: 2026-11-21, after the exploration fog; creatures added
2026-11-27, combat the same day.

## Gameplay: survival, the inventory, forage

- **The window loop's play has not been played.** Every rule is tested
  headless (`--survival-test`) and the HUD and inventory are checked in
  stills, but the live parts -- Tab and 1-8 through the script, sprint
  drain and a jump's cost under real physics, fall damage from a real
  landing (`character_supported()` as "on the ground"), the respawn, the
  clock running -- have not been driven by hand. [Survival]
- **Death keeps the inventory** and drops nothing: Valheim leaves a
  tombstone. No death screen either: the player wakes at once. [Survival]
- **Snapshots (F8) carry neither the player nor the picks**, and nothing
  saves on quit: F6 only. [Survival]
- **Tools have no models in hand** -- the axe swings an empty fist, and the
  torch is a light over the fist -- and no durability. [Survival]
- **Only two foods exist** (strawberries, glowing mushrooms), so the third
  food slot and "too full" are reachable only in the test table. [Survival]
- **Forage plants cast no shadow and do not sway**; they pop between ripe
  and flowering (no growth stages). The flowers are the only cue a picked
  plant will bear again. [Forage]
- **Nothing collides with a plant**, and a dig under one leaves it floating
  (rocks and trees settle; plants do not). [Forage]
- **The engine's draw of a species is one generator** (a berry rosette);
  a second species that should not look like a strawberry needs a `form`.
  [Forage]

## Visibility and atmosphere

- **The exploration fog has no shadows.** Its first 150 m (`glow_from`)
  take the horizon's mean so the shade round the eye does not glow with
  the sun's aureole; past that, a hill or a wood between the eye and a low
  sun does not shade the fog behind it. At dawn and dusk the valley layer
  and the mist lay a warm veil over a wood's floor (the spawn wood at 18:30
  meters ~1.9x what it does with the fog off, with the low-sun x0.5) -- by
  design, but judged only in stills. [The exploration fog]
- **The exploration fog's base is sea level everywhere.** A high plateau
  (highland, glacier: 90-110 m base, relief to 300 m and more) sits in clear
  air and sees further than the lowland. Next lever if it shows: a regional
  base from the fog map, blurred over a few km. [The exploration fog]
- **The far terrain rings, the far terrain shadow and the canopy shell still
  draw to their old reach**, which the exploration fog now hides. Cutting
  the draw distance to what can be seen would pay for the fog. [The
  exploration fog]
- **Lightning and local lights do not light the exploration fog**; the
  froxel mist carries them near the camera. [The exploration fog]
- **No fog that lifts where the player has been**, the exploration layer
  ADR-004 L4 proposed; `FogMap` is the hook. [Mist and volumetric light]
- The froxel mist's reprojection goes through float world space (TAA's no
  longer does); low-frequency and not seen to shake. [Gotchas: TAA shook]

- **Dark vision's floors and constants were set from timed headless runs,
  not judged in play**: night -0.5 (+1.0 is the ~2 stops darker a
  survival night wants), underground +1.5, lit -2, the lit key 1.5 stops,
  0.8 s / 0.3 s. The light presence measures at the ground under the eye
  with physics rays; a fire round a corner lighting a wall you see does
  not count (no clear line to it). [Dark vision]
- **After walking into a tunnel its unlit depths still darken for ~5 s**
  (the meter's 10th percentile 0.08 -> 0.007 nits, 20 m in at 11:00).
  NOT the probes' history any more: with every covered probe traced every
  other frame it is unchanged, and so with the heightfield GI rebuilt
  every frame, and with the mist and height fog off. Its curve follows the
  eye's own adaptation (adapted EV100 2.2 -> 1.5 over the same seconds,
  the same shape) -- something exposure-dependent in what the meter reads
  is the next suspect. [Near field; Dark vision]
- **The dark floor's colour fade is by each pixel's own luminance**, not
  by the eye's adaptation: a dim wall beside a bright one greys even while
  the eye is light-adapted. A real eye's rods take over only once it is
  dark-adapted. [Dark vision]

- **The near field's per-pixel reader is no longer a cost worth chasing**:
  since the visibility volume took its march out, the terrain pass with
  the reader is BELOW `BIOMES_NF_NOREAD`'s on every view measured (chamber
  2.65 against 2.71 ms, dug tunnel 1.47 against 1.54) -- the heightfield's
  covered fallback it replaces costs more. Left: eight probes' meta and
  SH, 40 loads a pixel. [Near field]
- **Three campfires in a pit at 4K cost ~1.6 ms**: terrain +0.9, grass +0.7
  (every lit pixel walks its cluster, each light with its shadow cube).
  Grass is the bigger share for its overdraw. [Local lights]

## The near field's two levels (built; what is left)
- **Far sunlit walls still arrive second-hand.** The coarse level's direct
  sun at a hit fades from 6 to 24 m of ray (`kNfCoarseSunFar`): unfaded,
  its 4 rays a bin found a sunlit patch 20-40 m down a tunnel by chance,
  and the unlit tunnel's 90th percentile wandered 4x over seconds (its
  standard deviation 70% of its mean). That light now comes through the
  fine probes beside the patch, one bounce later. Sampling sunlit surfaces
  as lights (next-event estimation) is what would bring it back.
  [Near field]
- **Some fine rays find no coarse probe** and march to 48 m themselves,
  with the old fade (one side of a 4 m tunnel whose blocks put their probe
  near the other wall; `BIOMES_NF_DBG=1` paints them green). [Near field]
- **The merge's parallax is a visibility test**, not the cascades'
  bilinear fix: a coarse probe that sees the ray's 6 m point (and, where
  its own 6 m point is air, that too) lends its bin. A probe on the
  surface seen through a tunnel's mouth can still lend its view along a
  ray the tunnel would have closed. [Near field]
- **Far hits skip a moving light**: the carried torch's light reaches
  walls past 6 m from a probe only second-hand, through the fine probes
  by it (the coarse level's 1 s memory would trail it). A campfire just
  placed fades in at far hits over that second. [Near field; Local lights]
- **Glowing mushrooms** (a test collectable): picks are not saved; a
  cluster on ground dug out from under it floats (rocks and trees settle,
  these do not); no inventory, only a count; a torch does not light them
  (they take no local light, or their own cluster's burnt them white);
  their clusters are ordinary local lights, so past the 24 shadowed ones
  they light through rock within 5 m and give no bounce -- the emitter
  plan (glowing cells the probes' rays see) is still the better design for
  many small lights. [Glowing mushrooms]
- **Fast lights are held fast for 60 frames**, not seconds: 0.4 s at 144
  fps; at most 8 a frame. [Near field]
- **Caves cost the near field what their covered air is** (measured with
  caves back on in a scratch biome table, 1440p): standing on the surface
  over sealed caves 8,022 live probes and 0.58 ms; in a 42 m chamber
  21,440 and 1.19 ms, of which the coarse level ~0.55 (its 48 m rays do not
  stop in open cave air); a screen of cave wall costs the reader 0.6 ms.
  Probes in sealed air with no light near no longer trace (`nf_gated`:
  1.17 -> 0.11 ms in the chamber), but OPEN cave air -- a sinkhole floor,
  a cave with any way out -- still traces every covered probe (the trench
  floor 6,484 live, 0.44 ms). The merge's per-ray test now runs for open
  coarse probes only, which took that to 0.26 ms; what is left is the fine
  probes' own rays. Priority by distance from the camera is next.
  [Near field; Sky access]
- **The sky-access flood is a CPU flood on its own thread**: answers
  trail the camera and the edits. A dig forgets the distances of every
  cell within the reach of it (~670k cells over the tunnel at spawn), so
  digging steadily keeps the worker busy (840 ms over 40 digs, against
  48 when it only labelled open or sealed) and its answers a second or
  so behind; the GPU keeps the old ones meanwhile, so breaking into a
  cave lets the light in that much later. Forgetting only the cells
  whose path can pass through the edit (their distance plus the straight
  distance to it within the reach) would cut it. A roof the player
  builds over open air is only seen by the bricks with edits in them
  (`open_above` reads `column_top` per brick); `sky_floor` reads it for
  the column. [Sky access]
- **The air past the volume is judged by the camera's own access** (step
  2): from inside a cave whose mouth is in view, the far air is lit by the
  camera's access, not by where along the cave it is -- the flood only
  answers inside the volume. A distance volume larger than the near
  field's, coarser, would carry it. The directional part of the plan
  (light arriving from the mouth's side) was not built: inside the volume
  the probes carry direction, and past it nothing was seen to need it.
  [Near field; Mist; Sky access]
- **The live list has no priority**: past 8,192 covered probes (a big cave
  system, when caves come back) it is walked in longer turns, near and far
  alike, and every probe's history covers its turn. The camera's
  surroundings first would be the next step. [Near field]
- **A probe dug fresh keeps its old light until its turn** (a frame or
  two at stride 2), then starts over. [Near field]
- **A light jump is judged from the sun and the sky only**: a storm
  rolling in, or the clouds' shadow arriving, is followed at the probes'
  memory (0.35 s fine, 1 s coarse), not at once. [Near field]

## Shadows and light

- **Moving casters past 28 m shadow at 30 or 15 Hz**: the cascades are
  time-sliced. Before creatures, built structures or particles move out
  there: cache each cascade's static map and draw dynamic casters over a
  copy every frame. [Shadows]
- **Woods are a little lighter under the cascades than they were under RT
  shadows**; the casters were matched to the real leaves, not re-checked
  since RT left. [Shadows; Hardware ray tracing: REMOVED]
- **The heightfield GI**: crowns do not bounce green, local lights are not
  in its bounce (in the open their fill stands in; under cover the near
  field carries them), nothing past 256 m, and a big cavern sees only its
  nearest opening's light. [Heightfield GI]
- The canopy map is static in the wind; leaf flutter is raster only.
  [Weather]
- **The lightning flash casts no shadows**, the sea's glint does not see
  it, no sheet lightning across cloud tops from above, and no thunder
  (no audio; the report gives the delay). [Lightning]

## Weather, water and wet

- **No drops on the camera lens.** [Volumetric clouds ... not done]
- **Underwater is a first version**: no waterline across the lens (the
  view switches whole as the eye crosses the surface, so a swimmer's eye in
  a crest flips it for a frame), no caustics, no light shafts, no
  particles; in a shallow storm the sheet clamped over the bed in a trough
  shows from below as dark patches on the sand. [Under the water]
- **Water is a half-space** (everything under sea level), drawn by the
  depth test alone, with the sea's waves weighted by the GENERATED depth: a
  dredged harbour or a channel dug across a beach stays calm and never gets
  the sea's waves (by choice); a deep narrow pit's water reflects the sky
  where SSR finds no wall on screen; light in a flooded tunnel is the near
  field's air (no light through water). Air pockets, seepage and filling
  were dropped on purpose (ADR-006, superseded). [Water is a half-space]
- **A dark band on a swell's back near the shore in a gale**, at a grazing
  view from just over the water (`--cam 604 1.6 -310 3.2 -0.25 --sea-state
  0.8`, seed 0): a straight-edged dark slab across the water, round at one
  end. Not the foam, not dug water, not SSR (`--no-ssr` keeps it), and the
  displacement and normals there are smooth: most likely the "reflection
  closed by the next wave" term (`open` in water.hlsl), which shades a back
  slope with the water's own dark colour. To fix. [The sea]
- A faint strip just under the horizon where the sea's SSR misses land.
  [The sea]
- **Puddle reflections are screen space** (nothing off screen or behind a
  nearer object), a far hit is fogged twice, nothing runs down a slope or
  drips off a rock, and wetness is the same everywhere the sky reaches.
  [The wet world]
- The swash has only been judged in stills, no sandy beach checked, and
  wet sand has no drying time. [The shoreline]
- No rain, cloud or fetch shelter behind islands. [Weather]
- Clouds: no 1/16-pixel-a-frame rendering (the cheap path), and the base
  can read as a flat ceiling from the ground. [Volumetric clouds]
- One unexplained init failure, "ocean detail pso cs_cols", once, not
  reproduced. [The sea's ripples]

## Ground cover, trees and rocks

- **A stump cannot be chopped out.** A felled tree leaves one for good: it
  is static, has no health, and gives no wood. Valheim's gives a little.
  [Felling]
- **The crown's clumps are spheres to physics however they are drawn.** A
  band of branches lands and settles like a beach ball, so a clump can
  come to rest standing where a tangle of branches would have sprawled;
  and a clump that falls on the player pushes them. They are gone in a few
  seconds, which is why it has not been worth a hull. [Felling]
- **A felled tree's pieces do not burn, build or stack in the world.** The
  wood goes into the inventory and stops there -- there is no construction
  yet, and a campfire (V) takes no fuel. [Felling; Survival]
- **Debris stamps nothing and shades no grass**: it is left out of the
  trample map and the navmesh on purpose (a rebuild for something that
  lives five seconds), so a crown lies ON the sward rather than in it.
  [Felling; Ground cover]
- **Every piece of every felled tree keeps a body for ever.** Logs always
  did; breaking one up makes four of them, each a sleeping dynamic body
  and a draw, however far away the player walks. Nothing despawns and
  nothing is pooled. [Felling]
- **A knocked tree still kicks its butt out.** Only a tree the player cuts
  hinges on its stump; one brought down by another is a free body, so the
  look the hinge was built to fix is still there whenever a chain runs.
  A hinge released as soon as it is fought, or a two-axis joint, would do
  it. [Felling]
- **The crown no longer props a falling tree.** Its physics is a trunk-thick
  capsule now, so a tree lies flat instead of coming to rest on its
  branches at about 75 degrees. The clumps do appear the moment that
  capsule's crown band touches anything, so what is missing is the prop,
  not the timing. [Felling]
- **A falling tree hangs up on what it fails to fell, for ever.** A blow
  that is not enough simply stops the faller against the standing trunk's
  static capsule, which is right -- that is what a hung-up tree is -- but
  nothing resolves it: it is not felled, it cannot be pushed on, and
  chopping it is chopping a log that happens to be at forty degrees. A
  real woodcutter's problem with a real answer (a winch, a second cut),
  and none of it exists. [Felling]
- **Debris cannot hit the player.** The crown's clumps are not Pieces and
  are left out of the blow test, so the one part of a falling tree that
  arrives after everything else has stopped is harmless. They weigh ~44 kg
  and would be under the energy floor anyway, which is why it has not been
  worth plumbing. [Felling]
- **Nothing else dynamic can hit the player.** Rocks are static and
  creatures have their own `attack` block, so "a physics object with
  momentum hurts you" reaches exactly as far as felled wood. A rockfall,
  a cart or anything thrown would each need wiring of their own. [Felling]
- **A blow's damage ignores WHERE up the struck trunk it landed.** A
  cantilever fixed in the ground bends by the moment, so the same blow
  near the top is worth far more than one at the base, and a thick trunk
  resists as the cube of its radius where its health is about linear in
  it. The energy is honest about the faller and says nothing about the
  victim's shape. [Felling]
- **A falling trunk passes through its neighbours' crowns**, which carry no
  collision at all -- only trunks do. It meets their trunks, and the
  analytic blow (which does use a crown sphere) is what decides damage, so
  nothing is missed; it is the picture that is thin. [Felling]
- **Nothing exercises the sustained heave.** Everything in the test world
  is under the character's 0.4 m step-up, so the player climbs it rather
  than leaning on it, and the knock does the work. The heave only matters
  against a bole too tall to clamber -- a big oak's is 0.7 m across -- and
  no test stands next to one. [Felling]
- **The player does not rebound off what they bump.** Jolt's character
  hands the log its momentum and stops dead: no stagger, no knockback, no
  cost for charging a three-tonne oak at a sprint. [Felling]
- **The clumps of crown are convex**, so a tangle of branches lands and
  settles like a rolled die rather than sprawling. Twenty faces stop it
  rolling away, which was the complaint; the shape is still wrong.
  [Felling]
- **Nothing but the player can shove a log.** Creatures get Jolt's default
  100 N and no bump of their own, so a boar cannot roll one, and nothing
  pushes a log but the player or a blow from another falling tree.
  [Felling; Creatures]
- **A falling tree hurts trees and nothing else.** Not the player, not a
  creature, not a rock, not another log: the test is against the standing
  trunks only. A tree dropped on a mimic should flatten it. [Felling;
  Creatures]
- **Grass grows through rock footprints and puddles**: the bake is cached
  per 16 m tile and sees neither rocks nor puddles (it does see dig edits
  now). [Rocks; Ground cover; The wet world]
- **Underground walls bounce with one fixed albedo** (0.26, 0.23, 0.20):
  a dug wall in the badlands sends back the same grey-brown as one in a
  meadow. The mesher's per-biome rock material (`terrain_material`) at the
  hit would tint it. [Near field]
- **Emitters are not a kind of light yet.** The plan is that glowing
  things (algae pools, lit particles) are local lights -- dim, unshadowed,
  no visible model -- so they light walls directly and through the probes
  like a torch. Nothing places one, and the local light list (256, sorted
  by importance; 24 shadowed, the rest light through rock) has not been
  sized for many small ones. [Near field; Local lights]
- **`--no-near-field` leaves a sealed space's mist glowing**: the old path
  lights the air near the eye by the camera's sky share, which the
  heightfield cannot see is zero 8 m underground. Only the A/B switch
  takes that path near the camera (the volume always holds the eye).
  [Near field]
- **A light past the 24 shadowed ones lights through rock**, its fill
  with it. The near field's probes leave it out (they would carry it
  through rock), so under cover such a light has no bounce at all.
  [Local lights; Near field]
- **The near field reaches 64 m round the camera and 32 m up and down**:
  past it, the heightfield's old covered branch answers (a search for a
  way out, else black). A mine deeper than 32 m below the eye is fine (the
  volume follows the camera); a tunnel seen from far above is not.
  [Near field]
- **A filled edit is not in the near field the way a dug one is**: its
  1 m points take the edit's value only where the 0.5 m lattice point falls
  on one, so a fill thinner than a metre can be missed. [Near field]
- **A dug pit's floor reads as turf**: the terrain shader picks the
  biome's ground scan by slope, so a flat floor two metres down takes the
  same grass-soil scan as the meadow over it (with no blades). An
  edit-aware material (bare soil where the edits are) would sell a hole.
  [Digging]
- **Bushes are still leaf cards at arm's length**: better arranged, but a
  real mesh (as the trees have) would be the next step; the sparse kind
  can read as a lone sprig. [Ground cover]
- **A dense wood still costs ~5.5 ms of a 4K frame** after the depth
  prepass (12.3 ms against 6.8 without trees; 24.1 before). Good enough for
  now. If it has to come down again, the candidates, measured with
  `--gpu-profile` at seed 0's densest stand (`tools/profile/prof_ab.ps1`),
  roughly in order of payoff for risk -- none built:
  1. **Cascade 0's LOD 0 casters past its 28 m slice on the card path**,
     as its LOD 1 casters already are. Most of cascade 0's 1.3 ms. Their
     shadows reach the near ground only when looking into a low sun, where
     the dapple would turn into the dithered average: judge that view.
  2. **A baked leaf-coverage texture** instead of the procedural leaf cells:
     the test is still ~60% of the prepass (2.5 ms) and of cascade 0's
     casters. A few variants of the smooth coverage field, bilinear and cut
     at 0.5 like a distance field; check near cards at 4K stay sharp and
     the impostor bake still matches.
  3. **Fewer, denser leaf cards per crown**: 15 surviving layers per covered
     pixel where ~5 at 55% already block 98% of the sky. Cuts the prepass,
     the casters and the vertex work together, but it is a tree-mesh change
     (`tree_mesh.cpp`): judge the crowns' look and the canopy map's shade.
  4. **Front-to-back instance order within a level** (sort the visible list
     or the tiles by distance): more of the prepass rejected by early depth.
     Cheap to try; a single crown's own layers are most of the overdraw, so
     expect a modest gain.
  5. **The grass and bushes through the same prepass** if foliage ever
     grows: today 0.27 ms, not worth it.
  6. **The post chain**, 3.3 ms at 4K, was not looked at here: AO, TAA,
     the composite and the sea scale with pixels.
  [Trees: What a wood costs at 4K]
- **Root flares assume flat ground**: on a steep slope the downhill side
  can float. [Trees]
- The level cross-fades and the far fade still dither on the screen, and
  the grass's clump cards fade against the blades the same way. [Trees]
- The rocks' LOD switch pops (no cross-fade); no GPU far cards for single
  trees on a skyline from the air. [Rocks; Far forest]
- Surface scans: ground hex-tiling is on the dominant ground candidate's
  dominant plane only -- steep ground still tiles -- and turns directional
  scans at random (the seabed's sand ripples); ground has no parallax under
  it. Terrain rock is hex-tiled with a far layer (Cliffs), but only the
  heaviest rock candidate, and the placed rocks not at all; cliffs have no
  procedural strata or streaking yet, and the worst case (a cliff filling
  half a 1440p frame at 20-40 m) costs +0.38 ms. No second ground material per biome; no BC
  compression, none for grass, leaves or rocks; parallax has no
  silhouettes. The heightfield GI's top-down albedo (`ps_top`) does not
  see the macro variation or far detail. [Surface texture; Bare ground:
  hex-tiling and far detail; Cliffs]

## Terrain

- **The CPU and GPU disagree by up to 2 cm in layer 6's overhang term** on
  v18's rougher ground (`--probe-field --probe-at -4000 1500`; 0.2 mm with
  `--no-overhang`, 1.6 mm on v17's layer 0 at the same spot). A tolerance,
  not a hash, by invariant 8 -- but the trees and rocks stand on the CPU's
  surface and the mesh is the GPU's. Not looked into. [The engine's own
  checks]
- **The river carve steps up to 0.37 m** where two segments of different
  widths meet; all five cases were within 35 m of an axis, unexplained.
  [Gotchas: the river carve]
- The near-black patches on distant coarse-LOD terrain are geometry, not a
  fault, but their cause is not settled. [The engine's own checks]
- `--adaptive-budget`'s CPU cost that grows with chunks a frame was never
  found. [Gotchas: the per-frame mesh budget]
- **Flat sites are not marked or used by anything yet**: no map marker, no
  spawn preference, no build mode. `SeedWorld::flats` is the list.
  [Micro-relief and flat sites]
- **The forest map and canopy shell do not know the sites**: past the
  trees' draw distance a site's glade is covered by the shell's canopy
  (sites are 1-3 of its 20 m texels). [Micro-relief and flat sites]
- **The 4-16 m band is filled (v21); 1-4 m is not.** The fine detail field
  stops at a 4 m period, which is eight voxels at LOD 0 -- going finer
  starts costing LOD agreement (0.73 of a voxel, limit 1.00, is where it
  stands) and would have to be faded against the voxel more carefully than
  another octave at gain 0.5 does for free. Below 1 m is the scans' and
  POM's job, not geometry's; see why tessellation was removed.
  [Micro-relief and flat sites; Bare ground]
- **The fine band's amplitudes are capped by the flat sites' shoulder, not
  by taste.** At 0.54 rise/run against a limit of 0.6 there is little
  headroom, and the cap bites hardest in exactly the three biomes that
  grow sites. `build_flats` tests the shoulder end-to-end (`dh` against
  `max_grade * outer`) and does not bound its local gradient at all, so
  bounding that is the prerequisite for a bumpier meadow.
  [Micro-relief and flat sites]
- **Nothing suppresses the fine band inside a flat site's pad.** It does
  not need to -- `flat_apply` pulls the pad to its own mean afterwards --
  but the pad's mean is computed from 33 samples of a now-bumpier surface,
  so a site's level wanders a little more from seed to seed than it did.
  Unmeasured. [Micro-relief and flat sites]

- **Land exists where no biome cell does, and it is the bare sand.** Layer 0
  decides land by noise; layer 3 assigns biomes to CELLS. On v18's
  archipelago the two come apart: at (1099, 26.9, -252) -- dry ground 365 m
  from the spawn, reachable on foot -- `--pick` finds all FOUR nearest cells
  are ocean, and the viewer's inspector agrees, printing `biome none (sea)`.
  Everything downstream then defaults: `terrain_height`'s `layer4_blend` gets
  no land weight, so the shape is pure layer 0 (smooth, featureless, no
  profile and no micro-relief); `terrain_material` takes its `wl == 0` branch
  and paints the seabed whatever `seabed.above_sea` says; and the foliage
  bake returns early, so nothing grows. That is what the hundreds of metres
  of empty sand on the spawn island ARE -- not a material bug, a hole in the
  biome coverage. It was invisible until the seabed fade greened the land
  round it, and now reads as a hard stepped line where the fourth cell stops
  being land. The levers: scatter or assign cells so every land point has a
  land cell in reach, or let the blend fall back to the nearest land cell
  however far (both move the heights, so a generator version bump).
  [Terrain surface materials; Micro-relief and flat sites]
- **An hour went into the wrong diagnosis of that line.** It is straight and
  quantised to the mesh, so it read as an iso-height contour being
  rasterised, and a wander term was added to the seabed fade's threshold to
  break it up. The line did not move (a 30 m wander only scattered sandy
  blobs through the grass elsewhere, which is how the term was shown to be
  live at all). `--pick` on the sand side, printing the four nearest cells,
  was what settled it in one run. The wander was removed. Pick the pixel
  before theorising about the shader. [Terrain surface materials]

## Placed caves (v19)

- **Only meadows and forest have caves**, and nothing is in them yet: no
  resources, no danger, no sealed pockets (`sealed` is built, 0 everywhere,
  waiting for a way to find one).
- **Floors are the capsules' rounded bottoms**, not levelled; walkable, but a
  wide cavern's floor is a shallow bowl.
- **Two biomes' caves can overlap** (each biome its own grid); rare at
  700-1000 m cells, not checked.
- **The forest density is 1.83 a km2 for 2** (meadows 0.98 for 1): what
  fails is mostly thin strips of forest on a coast or a border, where no
  spot passes. Raise `per_km2` a little if the target must be met exactly.
- **The canopy shell does not know a mouth's trees are gone**: from past the
  trees' draw distance a forest mouth is under canopy.
- **One flight worst frame of 14 ms** with caves on (7 ms off) over the
  forest caves, not reproduced or run down.

## The player character

- **No starts, stops or turns**: locomotion is one blend by speed, so a
  stop is the stride fading into idle and turning on the spot is the body
  rotating over still feet. `Jump_Start` is unused (physics leaves the
  ground at once). [The player character]
- **The axe swing is the library's `Sword_Attack`**, one-handed, on the
  upper body; nothing holds an axe. [The player character]
- **No foot IK**: the lowest heel or ball is planted on the capsule's
  ground, so on a slope the uphill foot sinks and the downhill one floats. [The player character]
- **TAA has no motion vectors**: a character the camera follows moves
  against the reprojection. Judged only in stills so far. [The player
  character]
- The free kit has no clothing beyond the base body. [The player
  character]

## UI and scripting

- **Interaction is checked only through `--shot --ui`**: every screen and
  tab renders, and `engine.set` from Lua reaches the renderer, but the
  mouse and keyboard paths (clicks, slider drags, Esc, F1, recapture on a
  click past the panel) and switching to borderless and back had not been
  exercised by hand when the layer landed. [UI and scripting]
- The UI renderer draws no clip masks, layers, filters or shaders:
  `transform`, `filter`, `mask-image`, `box-shadow` and gradients either do
  nothing or, for `transform`, stop overflow boxes clipping. No image
  loading (`LoadTexture` returns nothing), so no `<img>` or image
  decorators. Selects have no arrow glyph. [UI and scripting]
- The tuning panel is not saved; there is no "write these to
  archetypes.json". Fog layer edits move the layers live but not the
  per-biome mist map, which is baked once. [UI and scripting]
- Keys are fixed: the controls screen is a table in `menu.lua`, not a
  binding table. The engine's own hotkeys stay in `wnd_proc`. [UI and scripting]

## Creatures (ADR-007)

- **Every phase of ADR-007 is built**, and the chase has been played in the
  window (four mimics at once); the blows have not. Each part is tested
  headless (`--mob-test`, `--nav-report`, `--move-test`, `--mimic-test`,
  `--combat-test`). [Creatures and stealth]
- **Combat is one blow each way and nothing round it.** Built: a creature's
  windup, reach and arc, its recovery, the player's axe, death both ways,
  `on_hurt`, the HUD's red wash. Not built, in rough order of what a fight
  wants next:
  - **No stagger and no knockback**: a creature takes a hit without its
    swing or its path being interrupted, so two creatures in reach are an
    unbroken stream of blows. A hit that interrupts a windup is the one
    change that would most change how a fight feels.
  - **No blocking, dodging or parrying**, and no armour: the only defence
    is to be out of reach or behind it when the blow lands.
  - **One weapon** (the axe, 15 damage, 2.6 m, impact 0.47 s, recover
    0.33), no durability, no other damage types, no hit location. A tool's
    `tool` block is the hook, and it already carries the swing's timing and
    clip, so a second weapon is data: nothing has been tuned against the
    axe's numbers, and no clip but `Sword_Attack` has been tried.
  - **The dig (right mouse) still acts on the press**, with no swing, no
    animation and no buffer -- only the swing of a tool is in two parts.
    Clicks ARE buffered now (`swing_buffer_s` 0.3 s), but there is no
    hold-to-swing: chopping a tree is still click after click.
  - **The clip blends in over its first 0.1 s**, so the pose lags the
    swing's own clock slightly; `impact_s` was measured on the clip, not
    on what is drawn. Nothing marks the impact in the picture either (no
    hit spark, no sound, no pause).
  - **Nothing drops**: a dead creature is simply removed -- no corpse, no
    trophy, no meat. The player's death still keeps the inventory
    (no tombstone, gaps above).
  - **A hit is silent to perception**: the axe's swing is already 82 dB but
    a landed blow adds nothing, so hitting a creature does not draw others.
  - **The creature's swing is the library's `Sword_Attack`** on the action
    layer, the player's own clip: no windup pose of its own, and nothing is
    in its hands. The telegraph is therefore weaker than its 0.6 s.
  - **No creature health is shown** -- no bar, no hit flash on the body:
    the only feedback is the message line.
  - **A windup is not saved**: a creature frozen mid-swing thaws and swings
    afresh (health is saved). [Creatures and stealth]
- **Sound passes through rock**: hearing is distance and level only, so a
  creature hears a jog through a hill. Sight is blocked by terrain,
  trunks and rocks (physics), not by crowns, bushes or grass.
  [Creatures and stealth]
- **A behaviour's session memory is lost on a freeze** (the mimic's
  listening and looking timers, its 3 s grace): it resumes its saved state
  and mark, and starts those afresh. The body turns to look; no head turn.
  [Creatures and stealth]
- **Movement**: no jumping, climbing or turning on the spot (the body
  turns as it walks); far-off creatures ignore a rolling log until it
  rests and is stamped; no creature steers round the player (by choice:
  a body slides round theirs); the sidestep is local (2 m), so a wide
  obstacle the mesh has not seen is given up after 4; a far-off swimmer
  floats at a fixed depth, with no waves; creatures do not tire, drown or
  take falls; a stuck creature only gives up its goal after 3 s.
  [Creatures and stealth]
- **Spawn ground falls back to the 2.5D generated height** where no nav
  tile is in yet (a fresh start, the edge of the area), and a creature dug
  under does not settle. [Creatures and stealth]
- **Navmesh**: only the human size class is built; no in-engine overlay
  (the report's map only); a tile costs ~335 ms of a worker, all field
  evaluations -- sampling at 0.25 m for a finer mesh would cost 4x, and
  interpolating the 0.5 m lattice instead is the cheaper road; crowns,
  bushes and grass do not block; a flooded cave or a pit below sea level
  gets no swim surface (only a column's top floor does); a tree that
  settles into a dig is stamped where its tile last saw it; overhang and
  cave floors are in the spans but no test walks one. [Creatures and stealth]
- **A moving creature does not redraw a cached light face**: it is drawn
  into faces redrawn anyway, so a torch's shadow of a creature that has
  walked can lag up to 240 frames. Only the player's body has the
  bookkeeping. [Creatures and stealth]
- **No culling of creature bodies**: all drawn (up to 31) in every pass and
  cascade. Fine at the mimic's cap of 4. [Creatures and stealth]
- **Snapshots (F8) do not carry creatures.** [Creatures and stealth]
- **Crouching keeps the capsule's height**: no crawling under a low roof.
  [Creatures and stealth]
- **The light on the player is approximate**: the cloud cover's mean
  (not the shadow map where they stand), the CAMERA's open sky, and crowns
  do not shade it -- standing in a wood at noon counts as full sun.
  [Creatures and stealth]
- **No ambient masking yet**: wind and rain do not cover footsteps (the
  perception call takes `ambient_db`, passed 0). No audio at all: sound is
  a gameplay number. [Creatures and stealth]
- **Creatures that cannot bear light** (blinded by a torch, hiding by day)
  need a gain CURVE, not `dark_gain`; foliage and crowns do not block a
  creature's sight. [ADR-007]

## Future design: NPC simulation by distance (nothing built)

*ADR-007 settles this for CREATURES: a zone that leaves the active area
freezes its creatures as they are and saves them, with no catch-up (a
player cannot shake a chase by leaving). The catch-up below is for
persistent NPCs with schedules, still to come.*

A note from 2026-11-23, before any NPC code exists, so the first NPC work
does not simulate every NPC in the world. NPCs and player constructions
will NOT edit voxels; constructions are expected to be their own layer
(not the brick store, maybe not the tree/rock instance streamers either --
undecided).

- **Simulation tiers by distance, as the terrain has LODs.**
  - *Full* (~0-200 m, about physics' LOD 0-2 reach): physics capsule,
    animation, real pathing, perception, combat. A hard cap on how many.
  - *Reduced* (~200 m-1 km): no physics or animation; moves along a coarse
    nav graph, ticked a few times a second round robin under a fixed
    per-frame budget (as the mesh queue's budget). Drawn cheaply if at all
    -- the exploration fog hides most of this band.
  - *Abstract* (the rest): no per-frame update. A zone holds a compact
    record (who lives there, what they are doing, stocks, construction
    progress) and the time it was last simulated.
  - *Never visited*: no state. Population and layout are a pure function
    of seed and zone, as trees and rocks are of their cell.
- **Catch up, do not tick.** Waking a zone resolves the elapsed time in one
  step -- the weather's and the wet world's trick (state from the clock,
  nothing stored). Schedules (sleep, work, travel) make "where is NPC n at
  time t" a function to evaluate on activation. Cap the catch-up (a day or
  so) so a week away is not a burst of work.
- **Boundaries**: hysteresis -- wake at ~1.2 km, sleep at ~1.5 km -- or a
  walk along the line thrashes it (the LOD hold's problem). Promote NPCs
  into plausible spots out of view. A traveller between sleeping zones is
  an event with an arrival time, not a moving entity.
- **What a zone is**: a fixed grid of a few hundred metres aligned to the
  64 m tiles, or -- since v18's archipelago -- a landmass, where travel
  between them is rare and slow and so easy to abstract; big landmasses
  split by grid.
- **Ambient vs persistent NPCs**: ambient ones (wildlife, generic
  passers-by) spawn procedurally round the player and are dropped out of
  range, like grass, never saved. Only persistent ones (named, owners of
  constructions, anyone the player has dealt with) carry records through
  the tiers. Keeping most of the population ambient keeps the persistent
  set small.
- **Constructions ride the same zones**: waking a zone loads its buildings
  and its people together, and its catch-up advances construction too.
