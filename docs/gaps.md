# Known gaps

Open items to revisit, gathered in one place. Each line is the short form;
the section of CLAUDE.md named in brackets has the measurement and the why.
None of these blocks anything that works today. Ordered by area, not by
priority.

Last gathered: 2026-11-21, after the exploration fog.

## Visibility and atmosphere

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
- **After walking into a tunnel its unlit depths still darken for 10-20
  s** (the meter's 10th percentile, 0.2 -> 0.02 nits) as the near field's
  bounce settles. The exposure no longer follows them (it keys on the lit
  part when a light is present; unlit, the floor holds it). Not run down:
  neither shorter memories nor clearing stale probes removed it.
  [Near field; Dark vision]
- **The dark floor's colour fade is by each pixel's own luminance**, not
  by the eye's adaptation: a dim wall beside a bright one greys even while
  the eye is light-adapted. A real eye's rods take over only once it is
  dark-adapted. [Dark vision]

- **The near field's per-pixel reader costs ~1 ms at 4K on a screen of
  tunnel wall** (terrain 5.85 -> 4.83 ms with `BIOMES_NF_NOREAD`): eight
  probes a pixel, each checked for rock in between by a 0.5 m march
  (`nf_clear`). Cheaper visibility (a precomputed per-probe mask, or the
  march at half rate) would win most of it back. [Near field]
- **Three campfires in a pit at 4K cost ~1.6 ms**: terrain +0.9, grass +0.7
  (every lit pixel walks its cluster, each light with its shadow cube).
  Grass is the bigger share for its overdraw. [Local lights]

## The next lighting pass (planned)
- **Two probe levels in the near field, radiance-cascades style** (Split
  Radiance Cascades, Freeman, Sannikov and Pitt-Francis, the PDF in
  docs/): the 2 m probes keep short rays (a few metres) and a second level
  (4-8 m spacing, ~256 directions) takes 6-48 m, merged as RC merges
  intervals. It replaces the sun-at-hits fade (`kNfSunNear/Far`), which
  drops real light from far sunlit patches to kill their noise. The paper
  as written needs hardware ray tracing and ~10 ms; only its core idea
  fits here. [Near field]
- **Local lights back in the probes, properly**: their bounce is an
  instant per-light fill today (20%, shadowed by the light's own map). In
  the probes it lagged through their history and pulsed when a ray struck
  rock beside a flame; a near level with short rays and a clamp on the
  inverse square near the light are the candidates. Emitters (below) want
  the same path. [Near field; Local lights]

## Shadows and light

- **Moving casters past 28 m shadow at 30 or 15 Hz**: the cascades are
  time-sliced. Before creatures, built structures or particles move out
  there: cache each cascade's static map and draw dynamic casters over a
  copy every frame. [Shadows]
- **Woods are a little lighter under the cascades than they were under RT
  shadows**; the casters were matched to the real leaves, not re-checked
  since RT left. [Shadows; Hardware ray tracing: REMOVED]
- **The heightfield GI**: crowns do not bounce green, local lights are not
  in the bounce, nothing past 256 m, and a big cavern sees only its
  nearest opening's light. [Heightfield GI]
- The canopy map is static in the wind; leaf flutter is raster only.
  [Weather]
- **The lightning flash casts no shadows**, the sea's glint does not see
  it, no sheet lightning across cloud tops from above, and no thunder
  (no audio; the report gives the delay). [Lightning]

## Weather, water and wet

- **Rain draws under the sea**, and there are no drops on the camera lens.
  [Volumetric clouds ... not done]
- **No underwater rendering**: a submerged camera sees the seabed lit like
  dry land. [The sea]
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
  with it. [Local lights]
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
- Surface scans: hex-tiling is on the dominant ground candidate's
  dominant plane only -- rock faces and steep ground still tile -- and
  turns directional scans at random (the seabed's sand ripples); ground has
  no parallax under it. No second ground material per biome; no BC
  compression, none for grass, leaves or rocks; parallax has no
  silhouettes. The heightfield GI's top-down albedo (`ps_top`) does not
  see the macro variation or far detail. [Surface texture; Bare ground:
  hex-tiling and far detail]

## Terrain

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
