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

- **Grass grows through rock footprints and puddles**, and floats over dug
  holes: the bake is cached per 16 m tile and sees neither rocks, puddles
  nor edits. [Rocks; Ground cover; The wet world]
- **Bushes are still leaf cards at arm's length**: better arranged, but a
  real mesh (as the trees have) would be the next step; the sparse kind
  can read as a lone sprig. [Ground cover]
- **Root flares assume flat ground**: on a steep slope the downhill side
  can float. [Trees]
- The level cross-fades and the far fade still dither on the screen, and
  the grass's clump cards fade against the blades the same way. [Trees]
- The rocks' LOD switch pops (no cross-fade); no GPU far cards for single
  trees on a skyline from the air. [Rocks; Far forest]
- Surface scans: no anti-tiling beyond the mips and the retint, no BC
  compression, none for grass, leaves or rocks; parallax has no
  silhouettes. [Surface texture]

## Terrain

- **The river carve steps up to 0.37 m** where two segments of different
  widths meet; all five cases were within 35 m of an axis, unexplained.
  [Gotchas: the river carve]
- The near-black patches on distant coarse-LOD terrain are geometry, not a
  fault, but their cause is not settled. [The engine's own checks]
- `--adaptive-budget`'s CPU cost that grows with chunks a frame was never
  found. [Gotchas: the per-frame mesh budget]
