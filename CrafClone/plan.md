# CrafClone — Plan (vibecode path)

Goal: playable Minecraft creative fast. Single file `src/main.cpp`, no learning detours.
Build after every step: `cmake --build build` → run `build/bin/CrafClone.exe`.
Rule: one prompt = one runnable .exe. Never stack features.

Setup already works: CMake + `libs/raylib` + `build/bin/CrafClone.exe`.

## Step 1 — Foundation ✅ done
Prompt: `Fix src/main.cpp: init 1280x720 window "CrafClone", game loop with WindowShouldClose, BeginDrawing/EndDrawing, DrawFPS, CloseWindow, SetTargetFPS 60. Keep single file.`
Check: window opens, FPS shows, closes clean.

## Step 2 — Fly camera + cubes ✅ done
Prompt: `Add Camera3D first-person fly (WASD + mouse, UpdateCamera CAMERA_FIRST_PERSON), DrawGrid, and a 16x16x16 Chunk with Block enum Air/Grass/Dirt/Stone. Render with DrawCube per block, flat ground height 4. Add crosshair.`
Check: fly around colored terrain.

## Step 3 — Make it fast (meshing) ✅ done
Prompt: `Replace per-cube DrawCube with one Mesh per chunk: only add exposed faces, UploadMesh, DrawMesh with white material. Rebuild mesh on edit. Keep in same file.`
Check: 60fps on 4x4 chunks.
Skipped: greedy meshing, atlas. Add when profiler says so.

## Step 4 — Infinite hilly terrain ✅ done
Prompt: `Add infinite terrain: hash-based value noise h(x,z), 8 render distance around player, generate/recycle chunks, layers grass top/dirt 3/stone rest. Add fog to hide pop-in. // ponytail: hash noise, FastNoiseLite if looks bad`
Check: hills go on forever.

## Step 5 — Real player (gravity + collide) ✅ done
Prompt: `Add Player with pos/vel, gravity 25, jump 8, AABB 0.6x1.8x0.6, per-axis collide vs voxels (move X resolve, Y resolve, Z resolve). Toggle fly with F for debug. Spawn on surface.`
Check: fall, land, jump, can't pass through blocks.

## Step 6 — Break / Place ✅ done
Prompt: `Add voxel raycast DDA 6m from camera, highlight with DrawCubeWires, LMB break, RMB place, keys 1-3 pick Grass/Dirt/Stone, rebuild chunk mesh on edit.`
Check: this is Minecraft creative. Stop here if happy.

## Step 7 — Look good + save ✅ done
Prompt: `Add atlas.png texture with grass top/side/dirt/stone UVs, face shading (top 1.0 side 0.8 bottom 0.6), and save/load world.dat with SaveFileData/LoadFileData.`
Check: restart keeps edits. v1 done.

## Skip for v1
survival, mobs, crafting, menus, lighting, multiplayer.

## When broken
`It crashes with [paste error]. Fix src/main.cpp, keep single file, rebuild with cmake --build build.`