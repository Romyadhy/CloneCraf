# CrafClone — PlanM (plan status ledger)

Every plan item ever written down, with its current status. One line each, no prose —
the detail lives in `ROADMAP.md` (forward), `checklist.md` (done), `plan.md` (v1/v2 log).

**Status legend**

| Status | Meaning |
|---|---|
| `DONE` | shipped and verified (`-selftest` / `-bench` / screenshot) |
| `ONGOING` | started, not finished |
| `PENDING` | not started, still on the plan |
| `EDITED` | shipped, but in a different form than planned (reason given) |
| `DELETED` | removed from the plan — either moved to another milestone or judged unnecessary |
| `DEFERRED` | deliberately not built; has a named trigger for when it becomes worth it |

Next up: **M3 — Survival loop**. Steps 1–2 (`Health + hunger bars`, `Damage sources`) are
**done**; step 3 candidate is `Death + respawn` (0 HP must actually do something now that a fall
can kill you). `Eating` needs a food item, so it comes after or with `Death + respawn`.

---

## v1 — core engine (plan.md Steps 1–7)

| # | Plan item | Status | Note |
|---|---|---|---|
| 1 | Window, loop, FPS, clean close | `DONE` | 1280×720 |
| 2 | Fly camera + one chunk of cubes + crosshair | `DONE` | |
| 3 | Chunk meshing, face culling, GPU upload | `DONE` | |
| 4 | Infinite terrain, hash noise, render distance, recycle | `DONE` | |
| 5 | Player physics: gravity, jump, AABB collide, void reset | `DONE` | jump was broken until M1, see below |
| 6 | Break/place: DDA raycast, highlight, block picker | `DONE` | |
| 7 | Atlas + face shading + `world.dat` save/load | `DONE` | |
| — | `-selftest` self-check | `DONE` | exit 0 gates every step |

## v2 — fixes and polish (Steps 8.1–8.6)

| # | Plan item | Status | Note |
|---|---|---|---|
| 8.1 | Chunk-gen stutter fix (two budgeted queues) | `DONE` | 791 ms → ~25 ms on the old harness |
| 8.2 | Trees + caves + beaches + bedrock | `DONE` | |
| 8.3 | Vertex ambient occlusion | `DONE` | |
| 8.4 | Hotbar, 7 blocks, keys + wheel | `DONE` | later replaced by the real inventory, see 10.3 |
| 8.5 | Fullscreen (F11 borderless) | `DONE` | |
| 8.6 | Strafe direction fix | `DONE` | `RightVec` was negated; regression-checked |

## M1 — feel (Steps 9.1–9.7)

| Plan item | Status | Note |
|---|---|---|
| Day/night cycle | `DONE` | 600 s cycle; brightness reused as the `colDiffuse` tint |
| Fog shader | `EDITED` | planned "mix toward sky colour" — shipped **linear**; exponential is `DEFERRED` |
| Audio | `EDITED` | planned `LoadSound` of assets → shipped **procedural** noise bursts, 4 aliases per SFX |
| Break particles | `DONE` | timer death, no collision (`DEFERRED` until it looks wrong) |
| Held-item view model | `EDITED` | planned `DrawCube` → shipped **2D textured quad** (no depth fighting) |
| Pause menu + settings | `EDITED` | `GetCharPressed` text fields **not** built — +/- buttons cover it. Settings persisted |
| Crosshair + HUD polish | `EDITED` | planned crosshair **texture** → shipped outlined `DrawLine`s (no asset to ship) |
| *(found while wiring jump)* | `DONE` | `player.onGround = false` ran before the jump test → jump never fired. Fixed |
| *(found at shutdown)* | `DONE` | raylib 5.5 sound aliases freed with `UnloadSound` → heap corruption. Now `UnloadSoundAlias` |

## M2 — interaction depth (Steps 10.1–10.6, 11.1–11.5) — **complete**

| Plan item | Status | Note |
|---|---|---|
| `enum class Item` separate from `Block` | `EDITED` | **not** built. Block items reuse `Block` ids 1–7; non-block ids from 16. Less code, nothing to sync |
| Inventory model (`ItemStack`, `inv[36]`, add/take/count) | `DONE` | `AddItem` returns the leftover so nothing is silently dropped |
| Inventory UI (`E`, 9×4, LMB/RMB, cursor stack) | `DONE` | mouse-look + physics disabled while open |
| Mining time + progress bar | `EDITED` | planned **crack overlay** texture → shipped plain bar. Crack overlay `DELETED` (no gameplay value) |
| Tool classes (pick/axe/shovel) + 4× speed | `DONE` | `BreakTime(block, held)` |
| Tool **tiers** (wood/stone/iron speed tiers) | `DELETED` | replaced by one class-match bonus. Add when a second tier exists |
| Stone requires a pickaxe | `DONE` | `BreakTime` returns `-1`, so it cannot be brute-forced |
| Durability | `DONE` | 64 points, 1 per block, tool destroyed at 0, never merges |
| Per-material durability table | `DEFERRED` | one value for all tools; 3-line change when tiers exist |
| Tool atlas icons | `DONE` | procedural `DrawToolTile`, `TILES` 9 → 12 |
| Crafting recipe table | `EDITED` | **shaped only**. Shapeless form `DELETED`; 3×3 makes it redundant |
| Crafting 3×3 grid UI + result slot | `DONE` | consumed when the result is *taken* |
| Recipe **mirroring** (left/right-handed axe) | `PENDING` | small backlog; a `BoundingCells` variant |
| Ingredient tags (`any_plank`) | `DEFERRED` | write it at ~50 recipes, not 5 |
| Item drops (gravity, pickup, despawn) | `DONE` | |
| `BlockDrop`: grass→dirt, leaves→nothing | `EDITED` | planned cobblestone-from-stone and saplings → **same-block** drops. `DELETED` until those ids exist |
| Placement consumes the selected slot | `DONE` | |
| Creative toggle (`C`), persisted | `DONE` | backward-compatible 5th `settings.txt` field |
| Bench harness | `EDITED` | old time-scaled sweep measured its own feedback loop; now fixed 0.5/frame + `chunks=`/`meshed=` + `bench.png` |
| Torch recipe | `DELETED` | moved to **M4** — a torch without a light engine is a block that does nothing |
| Inventory / crafting-grid **save** | `PENDING` | `world.dat` stores block edits only. Belongs with M3's player save |
| Stick slot icon | `PENDING` | only item still falling back to shrinking text |

## M3 — survival loop — **pending, nothing started**

| Plan item | Status |
|---|---|
| Health + hunger bars | `DONE` — `TickSurvival`, regen/starvation/walk-drain, HUD bars |
| Damage sources (fall / drowning / invuln window) | `EDITED` — shipped fall damage + 0.5 s invuln window + 1.5 s regen hold, all through one `Hurt()`; **drowning deferred to M4** (no water block to test) |
| Eating (right-click food) | `PENDING` — hunger exists and is the thing food must restore |
| Death + respawn (drop inventory, death screen) | `PENDING` — health hits 0 with no consequence until this lands |
| Mob base (`Entity`, reuse per-axis collision) | `PENDING` |
| Passive mobs (random-walk AI) | `PENDING` |
| Hostile mobs (chase, contact damage, knockback) | `PENDING` |
| Spawning (darkness, distance, burn in daylight) | `PENDING` — depends on M4 light levels |
| Player save (health/hunger/**inventory**) | `PENDING` — the two floats from step 1 belong in this write |

## M4 — world content — **pending**

| Plan item | Status |
|---|---|
| Block shape system (slab/stairs/cross) | `PENDING` — blocks torches, plants, doors, ladders |
| Light propagation (per-block level, flood fill) | `PENDING` — largest item in the milestone |
| Torch block | `PENDING` — moved here from M2; needs both items above |
| Fluids: water, then lava | `PENDING` |
| Biomes (temperature/humidity noise) | `PENDING` |
| Ores (veins by depth) | `PENDING` |
| Plants (cross billboards on grass) | `PENDING` |
| Structure pass (trees crossing chunk borders) | `PENDING` |
| Villages | `PENDING` |

## M5 — performance and architecture — **pending**

| Plan item | Status |
|---|---|
| Frustum culling | `PENDING` — documented as the next lever |
| Greedy meshing | `PENDING` |
| Multi-threaded gen/mesh | `PENDING` — removes `GEN_BUDGET`/`MESH_BUDGET` |
| Split `src/main.cpp` into modules | `PENDING` — after the systems exist, not before |
| Chunk save format (store generated chunks / gen version) | `PENDING` |
| Frustum culling *now* | `DEFERRED` — `-bench` is 8–10 ms at RD 8, not a problem yet |

## M6 — multiplayer — **pending**

| Plan item | Status |
|---|---|
| World as a render-free library | `PENDING` |
| Headless server loop | `PENDING` |
| Transport + protocol (Winsock / small dep) | `PENDING` — the one place a new dependency is justified |
| Client sync (input, edits, chunk stream) | `PENDING` |
| Prediction + interpolation | `PENDING` |

---

## Deleted from the original plan on purpose

| Item | Why it's gone |
|---|---|
| Separate `enum class Item` | one id space with blocks as a sub-range is the same separation for less code |
| Mining crack overlay texture | a bar communicates the same thing; texture work with no gameplay value |
| Cobblestone / sapling drop variants | needs block ids and a drop table with more than 2 rows |
| Tool tier speed table | only one tool material exists |
| Shapeless recipes | every current recipe fits the 3×3 shaped form |
| Torch in M2 | needs M4's light engine to be more than decoration |
| Text fields in the settings menu | +/- buttons cover 4 numeric settings, no text input code |
| 3D held-item cube | 2D quad avoids depth fighting entirely |
| Crosshair texture asset | outlined lines, no file to ship |
| Particle-vs-block collision | timer death looks fine |
| Exponential fog | linear fog already hides pop-in |

## Explicitly not planned (unchanged)

Redstone, enchanting, potions, extra mob variants, maps/levels. Not on the path to a good
voxel game; add only if the game turns out to need them.

---

## Standing rules (from ROADMAP, they still apply)

- `-selftest` stays green after every change; non-trivial logic leaves one runnable check.
- Re-run `-bench` after any perf-relevant change and record the number in `checklist.md`.
- Reuse before writing: atlas, DDA raycast, per-axis collision, mesh builder, `DrawSlot`,
  `ClickSlot` — items, mobs and drops should call them, not clone them.
- Keep `DOCUMENTATION.md` §22 honest as ceilings change.
