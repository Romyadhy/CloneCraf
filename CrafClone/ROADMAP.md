# CrafClone — Roadmap

Forward plan. `plan.md` is the historical v1/v2 build log; `checklist.md` is what is done.
This file is what is **next**, as checklists. `PlanM.md` is the status ledger of every plan
item ever written (done / pending / edited / deleted / deferred).

## Current state (done)

Playable voxel game: infinite terrain, caves, trees, face-culled chunk meshing, vertex AO,
walk/jump/fly, break/place, save/load, fullscreen. M1 (feel) is complete: day/night, fog,
procedural audio, break particles, held item, pause menu + persisted settings.
**M2 is complete**: inventory + UI, mining time, item drops, placement consumption, creative
toggle, tool classes with a 4× speed bonus and a stone/pickaxe gate, tool durability, and a
3×3 crafting grid with planks/sticks/pickaxe/axe/shovel. See `checklist.md`. Verified by
`CrafClone.exe -selftest` and `-bench`.

## How to use this file

- Milestones are ordered by **dependency and value**, not by size.
- Each milestone ends in a state you can play. Do not start the next one mid-milestone.
- **M1 and M2 are done. M3 (survival) is next**; everything below it is a menu, not a to-do list.
- Every item is `[ ]` until it runs.

---

## M1 — Feel: make the existing game juicy

Goal: no new systems. The game already works; this makes it feel like a game.
Highest return per hour of anything on this page. **Done — see `checklist.md` "M1 'feel'".**

- [x] **Day/night cycle** — a `float dayTime` advancing with `dt`; lerp `ClearBackground`
      between sky blue and night blue, and set `mat.maps[MATERIAL_MAP_DIFFUSE].color` to a
      grey brightness value. One line per frame, dims the whole world for free.
      → Sky and terrain darken and brighten over a ~10 min cycle.
- [x] **Fog** — `LoadShaderFromMemory(vs, fs)` with a fragment shader mixing toward the sky
      colour by distance; `GetShaderLocation` + `SetShaderValue` for the colour and distance.
      Assign to `mat.shader`. This is the one place raylib 5.5 needs real shader work.
      → Chunk pop-in at the render-distance edge is invisible.
- [x] **Audio** — `InitAudioDevice()` at start, `LoadSound`/`PlaySound`/`SetSoundPitch`,
      `UnloadSound` + `CloseAudioDevice()` at exit. Random pitch per play so repeats do not
      sound mechanical. Guard with `IsAudioDeviceReady()`.
      → Footsteps, break, place, jump all make sound.
- [x] **Break particles** — a small `std::vector<Particle>` (pos, vel, life), spawn a few
      on break with colours from the block's atlas tile, `DrawCube` them with gravity.
      → Breaking a block throws a puff of its own colour.
- [x] **Held-item view model** — draw the selected block as a small `DrawCube` in the
      bottom-right of the screen after `EndMode3D`, with a swing offset when clicking.
      → You can see what you are holding and it moves when you use it.
- [x] **Pause menu + settings** — `Esc` opens a menu instead of quitting (replaces raylib's
      default quit; call `SetExitKey(KEY_NULL)`). Settings: render distance, FOV, mouse
      sensitivity, fullscreen toggle. `GetCharPressed` if you add text fields.
      → Esc pauses; changing render distance takes effect; settings persist.
- [x] **Crosshair + HUD polish** — the current crosshair is two `DrawLine`s; use a proper
      crosshair texture with a transparent centre, and fade in the hotbar on selection change.

**M1 done when:** the game runs with fog, sound, particles, a pause menu, and no visible pop-in.
**Status: done.** Held item shipped as a textured 2D quad rather than a 3D cube (simpler, no
depth fighting); crosshair shipped as outlined lines rather than a texture (same result, no asset).

---

## M2 — Interaction depth: mining, items, inventory

Goal: the bridge to survival. Turns "place coloured cubes" into "collect and build".

Split into two shippable passes so each ends in a playable state. **Both are done.**

### M2 pass 1 — the item loop ✅

- [x] **Items without a parallel enum** — block items reuse the `Block` ids (1..7), so placing
      is a direct cast and there is no conversion table to keep in sync. Non-block items start
      at 16 (`ITEM_STICK`, `ITEM_PICKAXE`, `ITEM_AXE`, `ITEM_SHOVEL`). *Deviation from the
      original plan (`enum class Item` vs `Block`):* one id space with blocks as a sub-range is
      the same separation for less code. Revisit only if a block is ever *not* an item.
- [x] **Inventory model** — `struct ItemStack { unsigned char item; count; unsigned short damage; }`
      and `inv[36]` (0–8 hotbar, 9–35 main). The hotbar is a view into the first 9. `AddItem`
      tops up partial stacks before using empty slots and returns the leftover; `TakeItem`
      drains across slots; `MAX_STACK` 64.
- [x] **Inventory UI** — `E` opens a 9×4 grid (main rows on top, hotbar row at the bottom).
      LMB takes/places/swaps a whole stack, RMB takes half / places one, hover shows the item
      name, the carried stack follows the cursor. Closing returns the carried stack to the
      inventory (drops it at the player's feet only if there is genuinely no room).
- [x] **Mining time** — `Hardness()` per block (leaves 0.25 s, dirt/sand/grass 0.55 s,
      wood/plank 1.6 s, stone 3.0 s). Holding LMB accumulates progress on the aimed block and
      only breaks at 100%, with a progress bar under the crosshair. Changing target resets it.
- [x] **Item drops** — a `Drop` entity (pos, vel, item, count, age) with gravity, settles on the
      first solid block below it, bobs, and is picked up when the player walks within 1.6 m.
      Breaks spawn one; `BlockDrop()` maps grass→dirt and leaves→nothing.
- [x] **Block placement consumes items**; breaking gives them. `C` toggles creative mode
      (persisted in `settings.txt` as a backward-compatible 5th field) which mines instantly,
      gives no drops, and places without consuming.

### M2 pass 2 — crafting + tools ✅

- [x] **Tool icons** — three new atlas tiles so tools stop rendering as text labels.
      `TILES` 9 → 12 with `BLOCK_TILES = 9` separating the speckled block tiles from the clean
      tool glyphs. `DrawToolTile()` draws them procedurally (still no PNG assets).
- [x] **Tools** — `ToolClass` (PICK/AXE/SHOVEL) with `ToolClassOf(item)` / `ClassOfBlock(block)`.
      `BreakTime(block, held)` divides `Hardness` by `TOOL_SPEED = 4` on a class match, gives
      **no** bonus on a mismatch, and returns `-1` for stone without a pickaxe. One wear point
      per block broken, `TOOL_MAX_DAMAGE = 64`, tool destroyed at zero; it never merges with
      another tool so wear values can't mix.
- [x] **Crafting** — `Recipe { in[9], out, outCount }` table plus a 3×3 grid UI, an arrow, a
      result slot and the recipe's name. Matching is bounding-box normalized, so position in the
      grid doesn't matter and a wrong-width near-miss is rejected. Ingredients are consumed when
      the result is taken. Foundational recipes: planks, sticks, pickaxe, axe, shovel.

**M2 done when:** punching a tree slowly yields wood, crafting a pickaxe works, and stone
needs the pickaxe. — **all three hold.**

**Shipped deviations in pass 2:**
- Recipes are **shaped only** and exact — no shapeless form, no mirroring, no ingredient tags
  (`any_plank`). With 5 recipes a linear scan + bounding box is the correct amount of machinery;
  the ingredient-tag abstraction is worth writing when there are ~50 recipes.
- The axe is left-handed only; a right-handed player pattern won't match. Mirroring is a
  `BoundingCells` variant, deferred.
- **Torch** from the original recipe list was dropped: it needs a light emitter, which is M4's
  light engine, so shipping the recipe now would produce a block that does nothing.
- The tool gate is binary (stone: pickaxe or nothing) rather than a per-tier requirement table.
- Items still aren't saved — see the checklist's ceilings.

**Shipped deviations in pass 1:** the progress bar is a plain bar, not a crack overlay
(texture work with no gameplay value yet); drops always yield the same block (no cobble,
no sapling); tools already exist as ids so `ItemName`/`DrawSlot` can render them, but they
have no behaviour until pass 2.

---

## M3 — Survival loop

Goal: a reason to play. Depends on M2 (health damage comes from mobs, hunger from items).

- [x] **Health + hunger bars** — two floats, drawn as HUD bars. Health regenerates when
      hunger is high; hunger drains over time and with sprinting.
      *Shipped as:* `TickSurvival(dt, moved, health, hunger)` — a pure function, so the whole
      metabolism is `-selftest`-pinned. Drain scales with **walking distance actually moved**,
      not "is sprinting" (there is no sprint key yet; the moved-units term covers sprinting the
      day it exists). Half-unit icon granularity, rects instead of heart sprites. Hidden in
      creative, where stats don't run. **Reaching 0 HP does nothing yet** — that is the
      "Death + respawn" item below.
- [x] **Damage sources** — fall damage from the existing `vel.y` at landing; drowning when
      a head-cell is water (after M4 water); a short invulnerability window after any hit.
      *Shipped as:* fall damage only — `FallDamage(dist) = dist - 3` (3 blocks free, MC's rule),
      measured apex-to-landing via `FallDistance` rather than summed per frame, so it can't
      drift or over-count. All hits route through one `Hurt(amount)` that owns the 0.5 s i-frame
      window and a 1.5 s regen hold; a new source in M3 can't forget either. Red edge gradients for
      feedback. **Drowning is deferred to M4** — it needs a water block to test against.
- [ ] **Eating** — right-click a food item to consume it, refilling hunger.
- [ ] **Death + respawn** — at 0 health: drop inventory, show a death screen, respawn at the
      world spawn with full health. Reuse the void-reset logic.
- [ ] **Mob base** — `struct Entity { pos, vel, yaw, type, health }` in a vector; reuse the
      player's per-axis collision so mobs do not fall through the world.
- [ ] **Passive mobs** — random-walk AI: pick a direction, walk, pause, repeat. Despawn far away.
- [ ] **Hostile mobs** — chase when the player is within range, attack on contact, take
      knockback when hit. Simple line-of-sight check with `RaycastVoxel`.
- [ ] **Spawning** — hostile mobs spawn in darkness/low light on solid ground at a distance
      from the player, and burn/despawn in daylight.

**M3 done when:** you can be chased, take damage, kill a mob, and die and respawn.

---

## M4 — World content

Goal: the world stops being a uniform hillside. Independent of M2/M3 — can be done earlier
if you get bored of survival.

- [ ] **Block shape system** — a per-block "shape" (full cube, slab, stairs, cross). The
      mesh builder gains a shape switch. **Required before torches/doors/ladders/plants**,
      because those are not cubes and the current mesher assumes a cube everywhere.
- [ ] **Light propagation** — a per-block light level, flood-filled from the sky and from
      light sources, stored per chunk. Bake the result into the mesh's vertex colour the same
      way AO already is. This is the real Minecraft light engine; it is the largest item here.
- [ ] **Torch block** (needs both items above) — emissive light source, non-cube shape.
- [ ] **Fluids** — water first: a `WATER` block that is non-solid, translucent, in a second
      render pass after opaque geometry, with a flowing-spread update. Lava reuses it.
- [ ] **Biomes** — a second noise pair (temperature, humidity) selecting per-column surface
      block and tree density. Reuses `ValueNoise`.
- [ ] **Ores** — scatter veins by depth with 3D noise, harder than stone, drops a resource.
- [ ] **Plants** — flowers/tall grass as cross-shaped billboards, spawned on grass.
- [ ] **Structure pass** — fix the current chunk-locked trees (they cannot cross borders,
      see `DOCUMENTATION.md` §12) by generating structures from a global position with a
      "spills into neighbours" pass. Then villages.

**M4 done when:** you can dig to lava, find a lit cave, and see several biome types.

---

## M5 — Performance and architecture

Goal: needed before the world gets big, and before any of M6.

- [ ] **Frustum culling** — test each chunk's AABB against the camera frustum before
      `DrawMesh`. Documented as the next lever in `DOCUMENTATION.md` §22.
- [ ] **Greedy meshing** — merge coplanar same-texture faces into large quads. The big
      triangle-count win; replaces the inner loop of `BuildMesh`.
- [ ] **Multi-threaded gen/mesh** — the queues in `ProcessQueues` are already shaped for this:
      hand chunks to worker threads and only upload on the main thread (`UploadMesh` and any
      GL call must stay on the main thread). Removes the `GEN_BUDGET`/`MESH_BUDGET` caps.
- [ ] **Split `src/main.cpp`** — into `world.*`, `mesh.*`, `player.*`, `render.*`, `ui.*`.
      Do this *after* the systems exist, not before; premature splitting is scaffolding.
- [ ] **Chunk save format** — currently only edits are stored, so changing the generator
      moves old worlds. Store generated chunks (or a generator version number) so worlds
      survive code changes.

**M5 done when:** `-bench` worst frame is well under 16 ms at render distance 16.

---

## M6 — Multiplayer

Goal: two players in one world. The biggest jump here by a wide margin — treat it as its
own project, and only after M5 has decoupled logic from rendering.

- [ ] **World as a render-free library** — extract everything except drawing into files with
      no raylib-graphics dependency, so it can run in a server loop with no window.
- [ ] **Server** — a headless loop owning the authoritative world, ticking entities.
- [ ] **Transport + protocol** — a socket layer plus a message format: handshake, chunk
      stream, block edit, entity update. Raylib has no networking; this needs a socket
      library (Winsock on Windows) or a small dependency — the one place a new dependency
      is justified.
- [ ] **Client sync** — send input, apply server block edits, stream chunks to the client.
- [ ] **Prediction + interpolation** — move the local player immediately and reconcile with
      the server; smooth other players between updates. Without these it feels unplayable.

**M6 done when:** two clients see each other build and move.

---

## Cross-cutting rules for all of it

- After any change: `CrafClone.exe -selftest` must stay green.
- After any perf-relevant change: re-run `-bench` and record the number in `checklist.md`.
- Non-trivial logic leaves one runnable check. The selftest is the place.
- Reuse before adding: the atlas, the DDA raycast, the per-axis collision and the mesh
  builder are all written to be reused — items, mobs and drops should use them, not clones.
- Keep `DOCUMENTATION.md` honest as ceilings change.

## Explicitly not planned

Modded-style content (redstone, enchanting, potions), mob variants beyond the base, and
maps/levels. Add only if the game needs them; none are on the path to "a good voxel game".