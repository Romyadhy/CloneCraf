# CrafClone — Checklist

## v1 (done)
- [x] Step 1 — Foundation: 1280x720 window, loop, FPS, clean close
- [x] Step 2 — Fly camera + one chunk of colored cubes + crosshair
- [x] Step 3 — Chunk meshing: one Mesh/chunk, face culling, GPU upload
- [x] Step 4 — Infinite terrain: hash value-noise heightmap, 8-chunk render distance, recycle
- [x] Step 5 — Player physics: gravity, jump, AABB collide, fly toggle F, void reset
- [x] Step 6 — Break / Place: DDA raycast, wireframe highlight, LMB/RMB, block picker
- [x] Step 7 — Atlas + face shading + `world.dat` save/load
- [x] Self-check: `CrafClone.exe -selftest`
- [x] Build clean: `cmake --build build` → `build/bin/CrafClone.exe`

## v2 (done)
- [x] Step 8.5 — Fullscreen. F11 toggles borderless-windowed at desktop resolution
      (`ToggleBorderlessWindowed` + `SetWindowSize`), restores 1280x720 on toggle back.
      Crosshair and HUD now use `GetScreenWidth/Height` instead of hardcoded 640/360.
- [x] Step 8.6 — Fixed strafe direction bug: `RightVec` returned the left vector, so A went
      right and D went left. Now `cross(forward, up)`. Regression check added to `-selftest`
      (verified it fails on the old formula).
- [x] Step 8.1 — Chunk-gen stutter fixed. Split gen and mesh into two budgeted queues; a chunk is
      meshed only after all 4 neighbors exist, so no 5x remesh cascade.
      `GEN_BUDGET=1`, `MESH_BUDGET=2` per frame.
      Measured: worst streaming frame 791 ms → ~25 ms. `CrafClone.exe -bench` (Release, uncapped FPS).
- [x] Step 8.2 — Trees + caves. Deterministic per-chunk tree pass (trunk + trimmed leaf canopy),
      3D value-noise cave carving, sand beaches at sea level, bedrock floor.
      Caves/trees groups kept away from chunk borders.
- [x] Step 8.3 — Vertex ambient occlusion. Per-corner AO in the mesh builder (0-3 occluders),
      sampled across chunk borders via the neighbor cache. Baked into vertex color, no shaders.
- [x] Step 8.4 — Hotbar. 7 blocks (Grass, Dirt, Stone, Sand, Wood, Leaves, Plank), atlas icons,
      1-7 keys + mouse wheel, selection outline.

## M1 "feel" (done)
- [x] Step 9.1 — Day/night cycle. `dayTime` advances over 600 s; `DayBrightness()` peaks at
      noon, bottoms at midnight. Sky colour lerps night→dusk→day and the **same** brightness
      tints the world through `mat.maps[DIFFUSE].color` (the `colDiffuse` uniform), so it dims
      terrain free on the default pipeline. Grid highlight flips black/white for legibility.
      `-selftest` pins noon > midnight.
- [x] Step 9.2 — Fog. Custom GLSL 330 shader (`LoadShaderFromMemory`, no asset files) replaces
      the material's default shader. Vertex passes world-space `fragPos` via `matModel`; fragment
      does linear `mix(fogColor, texel, f)` with `fogColor` = sky colour, so terrain dissolves
      into the sky and pop-in at the render edge is gone. `fogStart/End` scale with render
      distance (0.55x..1.0x). Uniforms set per frame with `SetShaderValue`.
- [x] Step 9.3 — Procedural audio. `InitAudioDevice` + noise-burst sounds synthesised into a
      `Wave` (`MakeNoiseSound`), 4 `LoadSoundAlias` voices per type so repeats overlap, random
      pitch jitter. Hooks: footstep (distance-accumulated), break, place, jump. Guarded by
      `IsAudioDeviceReady()`; silently off if there is no device.
      Bug found and fixed: aliases share sample data in raylib 5.5, so the base must be freed
      with `UnloadSound` and aliases with `UnloadSoundAlias` — the wrong order was heap corruption.
- [x] Step 9.4 — Break particles. 8 small cubes per break, colour jittered from the block's
      atlas tile, light gravity, timer death. `DrawCube` in the 3D pass.
- [x] Step 9.5 — Held-item view. Selected block drawn as a textured quad bottom-right with a
      click swing; 2D so no depth fighting.
- [x] Step 9.6 — Pause menu + settings. `SetExitKey(KEY_NULL)` frees Esc; Esc opens a menu that
      pauses simulation and shows the cursor. Editable render distance, FOV, sensitivity, volume
      (+ fullscreen / resume / save-and-quit). Persisted to `settings.txt` (`SaveFileText` +
      `sscanf`), loaded and clamped on start.
- [x] Step 9.7 — HUD polish. Crosshair is a white core over a dark outline; hotbar fades to dim
      when idle and brightens on selection change.
- [x] **Fixed a pre-existing bug:** `player.onGround = false;` ran immediately before
      `if (... && player.onGround)`, so **jump never fired**. Now the previous frame's grounded
      state is captured first (`wasGrounded`). Found while wiring the jump sound.

## M2 pass 1 "item loop" (done)
- [x] Step 10.1 — Items without a parallel enum. Block items reuse the `Block` ids (1..7), so
      placing is a plain cast and there is no conversion table to drift out of sync. Non-block
      items start at 16 (`ITEM_STICK`, `ITEM_PICKAXE`, `ITEM_AXE`, `ITEM_SHOVEL`). Tools exist
      as ids now so the UI can already render them; they have no behaviour until pass 2.
- [x] Step 10.2 — Inventory model. `ItemStack { item, count, damage }` + `inv[36]`
      (0–8 hotbar, 9–35 main). `AddItem` tops up partial stacks before empty slots and returns
      the leftover; `TakeItem` drains across slots; `CountItem`; `MAX_STACK` 64.
- [x] Step 10.3 — Inventory UI (`E`). 9×4 grid, main rows on top / hotbar row at the bottom.
      LMB take-place-swap, RMB half/one, hover name tooltip, carried stack follows the cursor.
      Closing returns the carried stack (drops at your feet only if there is truly no room).
      `-selftest` pins add/take/merge/overflow exactness.
- [x] Step 10.4 — Mining time. `Hardness()` per block (leaves 0.25 s, dirt/sand/grass 0.55 s,
      wood/plank 1.6 s, stone 3.0 s). Holding LMB accumulates on the aimed block, breaks at
      100%, progress bar under the crosshair. Retargeting resets progress. Creative is instant.
- [x] Step 10.5 — Item drops. `Drop { pos, vel, item, count, age }` + gravity, settles on the
      first solid block below, bobs, picked up within 1.6 m, despawns after 120 s.
      `BlockDrop()` maps grass→dirt and leaves→nothing; creative spawns no drops.
- [x] Step 10.6 — Placement consumes the **selected** slot (not the first matching stack, so the
      hotbar visibly ticks down). `C` toggles creative mode — instant mining, no drops, no
      consumption — persisted as a backward-compatible 5th `settings.txt` field.
- [x] **Bench harness fixed.** The sweep moved `40*dt` per frame, so faster frames travelled less
      and streamed fewer chunks — the "worst frame" fed back on itself and runs were
      incomparable. Now a fixed 0.5 units/frame with the camera angled at the ground, and it
      reports `chunks=` / `meshed=` so a half-loaded world can't masquerade as fast.
      `-bench` also writes `bench.png` (last frame) so the render can be eyeballed, not just
      timed. This caught a real bug: my first sweep outran `GEN_BUDGET=1` and was timing a
      world that was rendering nothing but sky.
- [x] Fixed while reviewing: title/help text overlapped in the inventory; tool labels overflowed
      their slot; mouse-look and physics no longer run while the inventory is open; drops could
      accumulate forever in creative (nothing is consumed there, so nothing was ever picked up).

## M2 pass 2 "tools & crafting" (done)
- [x] Step 11.1 — Tool atlas icons. `TILES` 9 → 12, `BLOCK_TILES = 9` keeps the speckled
      block tiles separate from the clean tool glyphs. `DrawToolTile()` draws a T-bar pickaxe,
      a wedge axe and a square scoop shovel procedurally into the atlas, so there is still no
      PNG asset. `ItemTile()` routes `ITEM_PICKAXE/AXE/SHOVEL` to those tiles, which removed
      the "shrink the label until it fits" fallback for tools. Verified by screenshot at 48 px.
- [x] Step 11.2 — Tool speed + tier gate. `ToolClass` (PICK/AXE/SHOVEL) and parallel
      `ToolClassOf(item)` / `ClassOfBlock(block)` lookups. `BreakTime(block, held)` returns
      seconds, or **-1 for "cannot be harvested"**:
      - a matching tool divides `Hardness` by `TOOL_SPEED = 4` (stone 3.0 s → 0.75 s);
      - a *wrong* tool gives no bonus at all (axe on stone == bare hands);
      - stone is gated behind the pickaxe, which is M2's "stone needs the pickaxe". `-1` means
        the loop stops accumulating, so it cannot be brute-forced by holding the button.
      The progress bar now divides by the same `BreakTime` — it previously used `Hardness()`
      and stalled at 25% whenever a tool was held. `-selftest` pins all three branches.
- [x] Step 11.3 — Durability. `ItemStack.damage` is wear; `TOOL_MAX_DAMAGE = 64`, one point per
      block broken in survival. `WearTool(ItemStack&)` returns true when the tool just broke,
      so it is testable without an inventory; `WearHeldTool()` points it at the selected slot.
      At the cap the stack is cleared and the tool is gone. `AddItem` and `ClickSlot` both
      refuse to merge tools — merging would silently mix two different wear values. `DrawSlot`
      draws a green→red bar under the icon once `damage > 0`. Creative breaks instantly and
      wears nothing (consistent with "creative consumes nothing").
- [x] Step 11.4 — Crafting model (no UI). `Recipe { name, in[9], out, outCount }` +
      `RECIPES[]` (planks, sticks, pickaxe, axe, shovel). Matching is **bounding-box
      normalized**: `BoundingCells()` extracts the occupied sub-rectangle top-left-aligned, so
      a one-log recipe matches in any corner of the 3×3, while a near-miss of the wrong
      dimensions is rejected on size alone. `ConsumeGrid()` takes one from every non-empty
      cell and clears emptied cells. Ingredients are consumed **when the result is taken**, not
      when arranged. `-selftest` pins: empty grid, match, shifted match, extra ingredient,
      wrong-width near-miss, and both consumption paths.
- [x] Step 11.5 — Crafting UI. 3×3 grid + arrow + result slot + recipe name, in a strip above
      the inventory inside the same panel. Grid cells reuse `ClickSlot`, so the interaction is
      identical to inventory cells. `craftGrid[9]` persists across `E`, and `ReturnCraftGrid()`
      returns its contents to the player at shutdown — the same "never destroy items" rule as
      the cursor stack. Layout bug found by looking at `bench.png`: the result slot was
      right-aligned off the panel edge and the inventory title overlapped the crafting cells.
      Left-aligned the strip and gave it its own title band.
- [x] Bench after pass 2: worst streaming frame 8.85 / 9.59 ms (261 chunks, 211 meshed) — no
      regression from the item work, as expected (it touches no per-frame world code).

## M3 step 1 "health + hunger" (done)
- [x] Step 12.1 — Survival stats. `Player.health`/`hunger` on the MC 0–20 scale.
      `TickSurvival(dt, moved, &health, &hunger)` is a **pure function taking references** —
      it touches no globals, so `-selftest` exercises the whole metabolism directly.
      Rules: hunger drains at `HUNGER_IDLE_RATE` + `moved * HUNGER_WALK_RATE` (real distance
      travelled, so an obstacle you're pressed against costs nothing — `UpdatePlayer` now
      *returns* the moved units it already had, instead of re-measuring); hunger ≥ 18 and not
      full HP → regen 0.5 HP/s, costing 0.8 hunger per HP; hunger 0 → 0.2 HP/s starvation.
      Bars: 10 cells of 2 units, half-cell granularity, `DrawStatusBars` draws both from one
      loop over a `{value, x, bg, fg}` table — adding a third bar is one row.
      Creative hides the bars and skips the tick (consistent with "creative consumes nothing").
      **Deliberately not in this step:** reaching 0 HP has no effect — that is the "Death +
      respawn" item, and a death screen before there is any damage source would be untestable.

## M3 step 2 "damage sources" (done)
- [x] Step 12.2 - Fall damage, invulnerability window, and the regen pause.
     `FallDamage(dist)` = `dist > 3 ? dist - 3 : 0` - three blocks free, 1 HP per block after,
     which is MC's rule.
     `FallDistance(peakY, landY)` = `peakY - landY`. The drop is measured **apex to landing**
     instead of summing per-frame `vel.y * dt`: summing over-counts by a frame of travel
     (a 12-block drop read as 12.3) and invents distance while sliding down a wall, where Y never
     moves but velocity does. Peak tracking is the same line count and exact.
     `fallPeakY` is pinned to the player's feet on the ground and to every new high while airborne,
     so a jump only counts from its apex and landing on a ledge charges only to that ledge.
     Reset on fly-toggle and on the void respawn, or a teleport would inherit the old drop.
     `Hurt(amount)` is **the only place a hit reduces health**. It is the single guard for the
     i-frame window (`INVULN_TIME` 0.5 s) and the regen hold (`REGEN_HOLD_TIME` 1.5 s), so a
     new damage source cannot forget either. Starvation deliberately bypasses it - it is a DoT and
     MC lets it tick through i-frames.
     `TickSurvival` gained a `hurtCooldown` parameter: without it, 0.5 HP/s regen out-heals chip
     damage and the i-frame window becomes irrelevant.
     Feedback: four red screen-edge bands driven by `hurtFlash`. The first version used **flat**
     rectangles and looked wrong — over a uniform sky a solid band recolours the whole top of the
     screen and reads as weather, not damage. Switched to `DrawRectangleGradientV/H` (already in
     raylib) so the edge is darkest and fades inward. Thud reuses `sfxJump` pitch-shifted down -
     no new asset.
     Creative is **immune in `Hurt` itself**, not at the call site: there is one choke point, so a
     mob added in M3 cannot forget the rule. The main loop also stops ticking the damage timers in
     creative so a stale flash cannot survive the mode toggle.
     **Deliberately not in this step:** drowning - it needs M4 water, and there is no water block
     to test against. Death at 0 HP is still the next item.

## Known ceilings
- `-bench` worst frame ~8–10 ms at render distance 8 (261 chunks, 211 meshed). **This is not
  comparable to the old ~25 ms figure**: the old harness used a time-scaled sweep, so it was
  partly measuring its own feedback loop. Numbers from before Step 10 are on a different scale.
  The metric is noisy on this machine — occasional 50–200 ms outliers are OS/AV scheduling
  jitter, verified by re-running with no code change. Take the median of several runs.
  Fix when it matters: frustum culling, then greedy meshing.
- Fog is **linear**; the band can look flat on long sightlines. Add an octave/exponential term
  in `FOG_FS` if so.
- **`bench.png` is not a 1:1 pixel capture.** The window is 1280×720 but the file is 1920×1080,
  and drawn geometry does not map onto those pixels linearly (a 120-logical-px band measured
  120 px across X and ~480 px down Y in the same image). Day/night also advances between runs, so
  pixel diffs against a second run are meaningless without pinning the clock. Use it to answer
  "does it look right" (which caught the crafting-panel overflow and the flat-band vignette), not
  to measure coordinates — read the geometry from `GetScreenWidth` instead. Fix the capture size
  if exact extents are ever needed.
- Ambient audio is positional-less (no `SetSoundPan` by direction) and SFX are synthesised blips,
  not recorded samples.
- Render-distance changes rebuild the load queue immediately, which can hitch briefly.
- Atlases are generated at runtime, not loaded from PNG. Add a file when you want real textures.
- Mesh indices are `unsigned short` → 65,536 vertices per chunk. Greedy meshing lowers this;
  a taller chunk raises it.
- **Tools have one durability value each (64) and one speed multiplier (4×).** No material
  tiers. `ponytail:` deliberate — the per-material table is a 3-line change to `ToolMaxDamage`
  once a second tier actually exists.
- **Recipes have no mirroring, no shapeless form, no ingredient tags.** The axe must be built
  left-handed; there is no "any plank" tag. Fine for 5 recipes, needs a real ingredient type
  before there are 50.
- **Inventory and crafting grid are not saved.** `world.dat` holds block edits only; the
  inventory is rebuilt at startup with the starter stack. Should be written together with
  M3's health/hunger so the player file format is defined once.
- `ItemTile()` has no icon for `ITEM_STICK`, so it falls back to shrinking text in a slot. The
  only item in the game still doing that.

## Backlog (not done)
**M2 leftovers (small):** a drawn stick tile; mirror matching in `MatchRecipe`; a torch recipe
(would need a light emitter, so it belongs with M4 lighting).
Then M3 survival: health/hunger, damage, eating, death/respawn, mobs.
Then M4 content: block shapes, light propagation, torches, water, biomes, ores, plants,
structures. Then M5 perf: frustum culling, greedy meshing, threads, file split, chunk save
format. Then M6 multiplayer.
Also small: `PendingMesh::attempts` is unused; `BLOCK_COUNT` is only a sentinel.

## Build notes
- Config is now `Release`. Reconfigure with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`.
- Verification: `build\bin\CrafClone.exe -selftest` and `-bench`. Both must exit 0.
- `settings.txt` and `world.dat` are written next to the exe at runtime.
