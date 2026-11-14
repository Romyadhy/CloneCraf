# CrafClone — Code Documentation

A line-by-line / function-by-function explanation of `src/main.cpp`: what each piece does,
which library call it relies on, why that call was chosen, and whether the approach is the
standard one for this class of game.

Verified against **raylib 5.5** (`libs/raylib/include/raylib.h`) and **raymath** as vendored
in this repo.

---

## Table of contents

1. [What this program is](#1-what-this-program-is)
2. [Includes and why each one](#2-includes-and-why-each-one)
3. [World constants](#3-world-constants)
4. [The Block enum](#4-the-block-enum)
5. [Integer helpers](#5-integer-helpers)
6. [Noise and terrain height](#6-noise-and-terrain-height)
7. [Texture atlas](#7-texture-atlas)
8. [Face geometry tables](#8-face-geometry-tables)
9. [Chunk struct and global state](#9-chunk-struct-and-global-state)
10. [Block read/write](#10-block-readwrite)
11. [Meshing — BuildMesh](#11-meshing--buildmesh)
12. [World generation](#12-world-generation)
13. [Chunk lifetime and dirty tracking](#13-chunk-lifetime-and-dirty-tracking)
14. [Chunk streaming (the stutter fix)](#14-chunk-streaming-the-stutter-fix)
15. [Player, movement, collision](#15-player-movement-collision)
16. [Voxel raycast](#16-voxel-raycast)
17. [Save / load](#17-save--load)
18. [Items, mining, inventory, crafting](#18-items-mining-inventory-crafting)
19. [SelfTest](#19-selftest)
20. [main()](#20-main)
21. [Is this the standard implementation?](#21-is-this-the-standard-implementation)
22. [Known ceilings and known bugs](#22-known-ceilings-and-known-bugs)
23. [Build and verify](#23-build-and-verify)

---

## 1. What this program is

A single-file Minecraft-like: an infinite procedural voxel world rendered in first person,
with walking/jumping physics, block breaking and placing, a 36-slot inventory, timed mining
with tools and durability, and a 3×3 crafting grid.

The core idea of any voxel game:

> The world is a 3D array of small cubes ("voxels"). The player is a small box. Everything
> else — rendering, collision, building — is working with that array.

The hard problems, and where this file handles them:

| Problem | Handled in |
|---|---|
| Storing an effectively infinite world without infinite memory | Section 12 + 14 (chunks + streaming) |
| Drawing millions of cubes at 60 fps | Section 11 (meshing + face culling) |
| A player that collides believably with an array of cubes | Section 15 |
| Turning blocks into items, tools and recipes without a second data model | Section 18 (one id space) |

---

## 2. Includes and why each one

```cpp
#include "raylib.h"
#include "raymath.h"
```

- **`raylib.h`** — windowing, input, drawing, textures, meshes, file IO. This one header
  covers all of raylib's API. It also pulls in the platform/OpenGL layer (rlgl) internally.
- **`raymath.h`** — vector and matrix math helpers (`Vector3`, `MatrixTranslate`, `Lerp`,
  `Vector3Normalize`, …). It is header-only and normally included *after* `raylib.h`
  because it uses types like `Vector3` that raylib defines.

```cpp
#include <algorithm>     // std::sort, std::unique, std::remove, std::remove_if
#include <cmath>         // floorf, fabsf, fminf, fmaxf, sinf, cosf
#include <cstdint>       // uint32_t, uint64_t — fixed-width ints for the hash and file magic
#include <cstring>       // memcpy for packing/unpacking the save file and mesh buffers
#include <unordered_map> // chunks is an unordered_map keyed by chunk coords
#include <vector>        // dynamic arrays for blocks, vertices, queues
```

Every one of these is standard library. **Nothing here is a new dependency.** That is
deliberate: a voxel game needs exactly these six and no more. In particular there is no
math library and no noise library — the noise is hand-written (Section 6).

---

## 3. World constants

```cpp
const int CX = 16, CZ = 16, CY = 96;   // chunk dims
```

Chunk dimensions in blocks. **16×16 is the conventional choice** — it is what Minecraft
itself uses, and it is a good tradeoff:

- Big enough that per-chunk overhead (one draw call, one mesh) is amortized.
- Small enough that a chunk is cheap to generate and mesh.
- A power of two, so coordinate math stays cheap.

`CY = 96` is the world's vertical extent. It is fixed (no infinite Y) because the player
never excavates to the bottom in this version. This is a simplification versus real
Minecraft, which also has a fixed build height (256 or 384), so it is a normal constraint.

```cpp
const int RENDER_DIST = 8;   // chunks around player
```

The world is loaded as a `(2*RENDER_DIST+1)²` square of chunks centered on the player —
17×17 = 289 chunks at distance 8. Every chunk is a fixed size, so the loaded region is a
fixed memory and draw-call budget. Standard technique; Minecraft's own render distance
setting is the same concept.

```cpp
const int GEN_BUDGET = 1;    // chunks generated per frame (stutter knob)
const int MESH_BUDGET = 2;   // chunk meshes built per frame (stutter knob)
```

The two performance knobs. They exist because generating and meshing a chunk is
expensive, and doing all of them in one frame causes a visible freeze. See Section 14.

```cpp
const int SEA_LEVEL = 24;
```

Used only to decide where beaches are (sand instead of grass near this height). There is no
water block yet — a real implementation would fill everything below `SEA_LEVEL` with water.

```cpp
constexpr int CHUNK_VOL = CX * CZ * CY;
```

`16*16*96 = 24576` — the number of blocks in one chunk. Used when allocating/clearing the
block array. `constexpr` means it is computed at compile time and can size things statically.

---

## 4. The Block enum

```cpp
enum Block : unsigned char {
  AIR = 0, GRASS, DIRT, STONE, SAND, WOOD, LEAVES, PLANK, BLOCK_COUNT
};
```

- **`: unsigned char`** means each block value is one byte. With 24576 blocks per chunk, one
  byte instead of four saves ~72 KB per chunk. Standard practice.
- **`AIR = 0`** is deliberate: zero-initialized memory is "empty". `memset`/`assign(..., AIR)`
  clears a chunk, and any uninitialized or out-of-bounds block naturally reads as air.
- The rest auto-increment (GRASS=1, DIRT=2, …). The numeric values do not matter beyond
  `AIR == 0`; they are only compared for equality and used to index the atlas.
- **`BLOCK_COUNT`** is a sentinel that always equals one past the last block. It is currently
  unused, but it is the conventional way to size arrays by block type later.

**Is this standard?** Yes. A one-byte `enum`-as-block-id is the normal minimal representation.
Performance-critical engines go further and **bit-pack** (4 bits per block, or "palette
compression" where a chunk stores a small palette + indices). That is an optimization, not a
correctness requirement, and this file deliberately skips it — see Section 22.

---

## 5. Integer helpers

### `Idx(x, y, z)`

```cpp
static inline int Idx(int x, int y, int z) { return x + CX * (z + CZ * y); }
```

Converts a 3D voxel coordinate inside a chunk into a 1D index into the flat `blocks` vector.
A chunk is stored as a flat `std::vector<unsigned char>` of `CHUNK_VOL` bytes.

The layout is **Y-major**: moving 1 in `y` jumps a whole XZ-layer (`CX*CZ`), moving 1 in `z`
jumps one row (`CX`). This layout is chosen so that iterating a horizontal layer is
contiguous in memory — friendly to the CPU cache during meshing, which loops y-outer.

`static inline` means "local to this file, and expand at call sites rather than a function
call". This function is called millions of times; the compiler must be able to inline it.

### `FloorDiv(a, b)`

```cpp
static inline int FloorDiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }
```

**This is the single most important subtlety in the whole file.** In C++, integer division
truncates *toward zero*, but voxel math needs *floor* division (round toward negative
infinity).

Concretely: `-1 / 16` is `0` in C++, but the floor is `-1`. That matters because the world
extends into negative coordinates. If you use `x / 16` to find which chunk a block belongs
to, then block `x = -1` would be assigned to chunk `0` instead of chunk `-1` — the wrong
chunk. Since `Idx` would then compute a local coordinate of `-1 - 0*16 = -1`, you would
index *before the start of the array*. That is a memory-corruption read, not just a visual
glitch.

So `FloorDiv` is used anywhere a world coordinate becomes a chunk coordinate or a local
coordinate. It is covered by a self-test.

**Is this standard?** Yes — this exact pattern appears in essentially every voxel engine, and
getting it wrong is a classic bug. Python's `//` and GLSL's `floor(a/b)` are floor-division
natively; C++ is the odd one out.

### `ChunkKey(cx, cz)`

```cpp
static inline uint64_t ChunkKey(int cx, int cz) {
  return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
```

Packs two 32-bit chunk coordinates into one 64-bit key for the `unordered_map`. This is
faster and hashable without writing a custom hash struct, versus using a `pair` or a struct
as the key.

**Is this standard?** Yes, a common idiom. (A `std::pair<int,int>` also works but needs a
custom hasher; packing is fewer lines.)

### `IsSolid(b)`

```cpp
static inline bool IsSolid(unsigned char b) { return b != AIR; }
```

Named predicate instead of scattering `!= AIR` everywhere. It is the one place to change if
blocks later gain properties (e.g. water is non-solid, leaves are transparent-but-solid).

**Known limitation:** right now any non-air block is solid. Water, torches, and glass would
all need special cases, so this function is currently a simplification that happens to be
exact for the blocks that exist.

---

## 6. Noise and terrain height

Terrain shape comes from **value noise**. There is no noise library dependency — the whole
thing is about 40 lines.

### `Hash2(x, z)` and `Hash3(x, y, z)`

```cpp
static inline float Hash2(int x, int z) {
  uint32_t n = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  n = (n ^ (n >> 13)) * 1274126177u;
  n ^= n >> 16;
  return (float)(n & 0xFFFFFF) / (float)0xFFFFFF;
}
```

Turns integer coordinates into a pseudo-random float in `[0, 1)`. The constants are large
primes; the `^` and shifts are an xorshift-style mix so nearby inputs give very different
outputs. The final line masks to 24 bits and divides, giving a float in [0,1).

Why hash instead of `rand()`: it is **stateless**. `Hash2(5, 9)` is the same value every time,
on every machine, forever. That is exactly what a procedural world needs — the same
coordinate must always produce the same terrain, or the world would shimmer.

**Is this standard?** Yes in structure. This particular constant set is a common "integer
hash" family. Most engines use Perlin or Simplex noise instead for the *final* terrain
because they have nicer gradient properties, but value noise is the standard first thing to
write and is used here deliberately (see Section 22's `ponytail:` note).

### `SmoothT(t)`

```cpp
static inline float SmoothT(float t) { return t * t * (3.0f - 2.0f * t); }
```

The **smoothstep** function. It eases an interpolation factor so that the value and its
slope match at both ends. Without it, linear interpolation of noise leaves visible diagonal
creases ("Mach bands") on the terrain. This is the classic Perlin smoothstep.

### `ValueNoise(x, z)`

```cpp
static float ValueNoise(float x, float z) {
  int xi = (int)floorf(x), zi = (int)floorf(z);
  float tx = SmoothT(x - xi), tz = SmoothT(z - zi);
  float a = Hash2(xi, zi), b = Hash2(xi + 1, zi);
  float c = Hash2(xi, zi + 1), d = Hash2(xi + 1, zi + 1);
  return Lerp(Lerp(a, b, tx), Lerp(c, d, tx), tz);
}
```

2D value noise: hash the four corners of the integer cell containing `(x,z)`, interpolate
along X twice, then interpolate those two results along Z. `Lerp` is from `raymath.h`
(`start + (end-start)*amount`) — reused rather than re-written.

**Is this standard?** This is textbook value noise. "Value noise" vs "gradient noise
(Perlin)" is a real distinction: value noise hashes a value per lattice point; Perlin hashes
a gradient. Value noise is blockier-looking, which is acceptable here.

### `CaveNoise(x, y, z)`

The 3D version — eight corners of a cube, four interpolations along X, two along Y, one
along Z. Same idea one dimension up. Its output is compared against a threshold to carve
air. Uses `Hash3`.

Note the comment in the code: it is a **single octave**. Fractal (multi-octave) 3D noise
gives more interesting winding tunnels; one octave gives blobby pockets.

### `TerrainHeight(wx, wz)`

```cpp
static float TerrainHeight(int wx, int wz) {
  float h = 0, amp = 1, freq = 1.0f / 64.0f, sum = 0;
  for (int o = 0; o < 4; o++) {
    h += ValueNoise(wx * freq, wz * freq) * amp;
    sum += amp;
    amp *= 0.5f;
    freq *= 2;
  }
  return SEA_LEVEL + (h / sum) * 18.0f - 6.0f;
}
```

**Fractal Brownian Motion (fBm)** — the standard way to make noise look natural. Four
octaves: each octave doubles the frequency (finer detail) and halves the amplitude (less
influence). Small bumps ride on big hills, which is how real terrain reads.

- Dividing by `sum` normalizes the result back to roughly `[0,1]`.
- `* 18.0f - 6.0f` maps it to about ±18 blocks around `SEA_LEVEL`.

It takes **world** coordinates, not local ones, so neighbouring chunks line up seamlessly —
the terrain is continuous across chunk borders by construction. This is why terrain has no
seams, while trees/caves needed care (Section 12).

**Is this standard?** Yes — fBm with 4 octaves and halving amplitude is the canonical recipe.

---

## 7. Texture atlas

A **texture atlas** is one image containing all block textures side by side; blocks reference
a sub-rectangle of it. One texture means one material and one possible draw call per chunk,
instead of a draw call per texture. This is the standard solution to "many small textures".

### `MakeAtlas()`

```cpp
Image img = GenImageColor(TILE * TILES, TILE, WHITE);
```

Allocates a CPU-side image (`Image` = pixel data in RAM) of `TILES` slots × 16px tall.

The atlas holds two different kinds of tile, split by `BLOCK_TILES`:

- **`0 .. BLOCK_TILES-1`** — block faces, each filled with a base colour plus speckle noise.
- **`BLOCK_TILES .. TILES-1`** — item icons (pickaxe, axe, shovel), drawn clean with no
  speckle, because a 16px glyph covered in noise is unreadable.

`TILES` went from 9 to 12 for the tool icons. The two constants are kept separate so the
speckle loop iterates only over the block range — one number controls both the texture width
and the loop, so they cannot disagree.

`ImageDrawRectangle`, `ImageDrawPixel` fill in each tile's base colour, then add 60 darker
speckles per tile so the texture is not a flat colour (flat colours look like untextured
geometry). `Hash2` drives the speckle positions so it is deterministic — the same every run,
and no random seed to manage.

```cpp
ImageDrawRectangle(&img, 1 * TILE, 0, TILE, 5, base[0]);   // grass_side green strip
```

Paints a green band on the top of the grass-side tile, so the side of a grass block looks
like grass over dirt. This is the classic Minecraft grass-side look, done with 5 rows of
pixels.

### `DrawToolTile()`

Drawn into the three item slots with `ImageDrawRectangle` / `ImageDrawLine` only:

| Tile | Shape | Reads as |
|---|---|---|
| `TILE_PICKAXE` | T-bar head over a vertical handle | pickaxe |
| `TILE_AXE` | wedge blade on a handle | axe |
| `TILE_SHOVEL` | square scoop on a handle | shovel |

Procedural glyphs are the lazy correct choice here: three 16×16 icons are ~20 lines of
rectangles, versus shipping, loading and remembering three PNG files. They are legible at the
48px slot size, verified by screenshot. Swapping in real art is one `LoadTexture` per icon
when that art exists.

```cpp
Texture2D t = LoadTextureFromImage(img);
UnloadImage(img);
SetTextureFilter(t, TEXTURE_FILTER_POINT);
```

- `LoadTextureFromImage` uploads the raster to the GPU (an `Image` is CPU/RAM, a `Texture2D`
  is GPU/VRAM — raylib separates these deliberately).
- `UnloadImage` frees the CPU copy immediately; it is no longer needed once uploaded.
- **`TEXTURE_FILTER_POINT`** is important: it disables bilinear smoothing, giving crisp
  pixel-art edges. With the default filter you would get blurry, bleeding seams between
  atlas tiles. This is the standard setting for voxel/pixel textures.

**Because the atlas is generated at runtime, there are no image assets to ship.** That is a
convenience (nothing to load, nothing to lose) at the cost of not being able to hand-draw
nicer textures. A production game loads a PNG here; that is a one-line change
(`LoadTexture("atlas.png")`).

### `TileFor(block, face)`

Maps `(block type, which face)` to an atlas tile index. Grass gets tile 0 on top, tile 2 on
the bottom (so you never see grass from below), tile 1 on the sides. Wood gets tile 6 on
top/bottom ("wood top" with rings) and 5 on the sides (bark). This is a **per-face texture**
lookup, which is what lets grass have a different top and side — a defining Minecraft detail.

---

## 8. Face geometry tables

A cube has 6 faces, each 4 corners. These tables describe all of it as data, which is why
`BuildMesh` is a single loop instead of hundreds of lines of `push_back`.

```cpp
// face order: 0:+Z 1:-Z 2:+Y 3:-Y 4:-X 5:+X
static const int FACE_N[6][3] = {...};
```

`FACE_N` is the **outward normal** of each face — i.e. which neighbour block would occlude
it. Face 2 is `+Y` (top), face 3 is `-Y` (bottom). The order is arbitrary but must be
consistent across all the tables.

```cpp
static const float FACE_V[6][4][3] = {...};
```

`FACE_V` is the 4 corner positions of each face, in the unit cube, as offsets from the block
origin. The **corner order is chosen to be counter-clockwise when viewed from outside**,
which matters: graphics APIs default to culling back faces, so a wrongly-wound corner order
makes faces invisible from the front and visible from behind.

```cpp
static const float FACE_UV[6][4][2] = {...};
```

`FACE_UV` is the texture coordinate for each corner, in tile-local `[0,1]`. These are
ordered to match `FACE_V` so the texture is not rotated or mirrored on any face. (Notice the
top and bottom faces use a different pattern from the sides — that is intentional, to keep
textures upright.)

```cpp
static const float FACE_SHADE[6] = {0.75f, 0.75f, 1.0f, 0.5f, 0.85f, 0.85f};
```

**Baked directional shading.** The top is brightest (1.0), the bottom darkest (0.5), sides in
between. This fakes a sun without any lighting system, and is what makes a flat-lit voxel
world readable — you can instantly tell which surfaces face up. The values are tuned by eye,
not physics. This is a **standard trick** (Minecraft does exactly this with per-face shade
multipliers).

```cpp
static const int FACE_UA[6] = {0, 0, 0, 0, 2, 2};
static const int FACE_WA[6] = {1, 1, 2, 2, 1, 1};
```

"Which two axes are tangent (along the surface) for this face." Used by the ambient
occlusion sampling in `BuildMesh`: for a `+Z` face (`FACE_UA=0`, `FACE_WA=1`) the surface
spans X and Y, so the AO samples step along X and Y. For `+Y` the surface spans X and Z, so
the axes are X and Z. This is what makes AO work on every face orientation with one code path.

---

## 9. Chunk struct and global state

```cpp
struct Chunk {
  int cx, cz;                        // this chunk's coordinate in chunk space
  std::vector<unsigned char> blocks; // CHUNK_VOL block ids
  Mesh mesh{};
  bool hasMesh = false;
};
```

One chunk = its coordinate, its block array, and its GPU mesh.

`Mesh mesh{}` value-initializes to zeros; `hasMesh` guards against drawing or unloading a mesh
that was never built (an empty chunk has zero vertices and no GPU buffers).

```cpp
std::unordered_map<uint64_t, Chunk *> chunks;
```

The loaded-chunk table. `Chunk*` (heap-allocated) rather than `Chunk` by value because the
map must not move chunk data around — the mesh queues and dirty list hold raw `Chunk*`
pointers, and a rehash that moved a `Chunk` would dangle them.

`unordered_map` (hash table) gives O(1) lookup by chunk coordinate, versus O(log n) for an
ordered `map`. Chunk lookup happens constantly (every block access), so the constant factor
matters.

```cpp
struct Edit { int x, y, z; unsigned char b; int _pad; };
std::vector<Edit> edits;
```

Player modifications to the world, stored as a plain list of `(world position, block)`. See
Section 17 for why this is the save format.

`_pad` is explicit padding to make the struct 16 bytes rather than 13+, so the on-disk file
has a predictable layout. (The compiler would pad it anyway; making it explicit documents
the intent and keeps the file format stable.)

---

## 10. Block read/write

### `GetBlockWorld(x, y, z)`

```cpp
if (y < 0 || y >= CY) return AIR;
int cx = FloorDiv(x, CX), cz = FloorDiv(z, CZ);
auto it = chunks.find(ChunkKey(cx, cz));
if (it == chunks.end()) return AIR;
return it->second->blocks[Idx(x - cx * CX, y, z - cz * CZ)];
```

Turns a **world** coordinate into a block id. Three steps: reject vertical out-of-range
(returns air — the world's floor and ceiling are "empty"), find the owning chunk, convert
world→local with `x - cx*CX`.

**Unloaded chunks read as air.** That is a deliberate choice: it means a distant chunk that
has not loaded yet appears as empty space rather than crashing. The cost is that you can see
a sliver of missing faces at the far edge of the loaded region — normally hidden behind fog
in a finished engine (see Section 22).

### `SetBlockWorld(x, y, z, b, record)`

```cpp
it->second->blocks[Idx(lx, y, lz)] = b;
```

Writes one block, then if `record` is true:

```cpp
edits.push_back({x, y, z, b, 0});
MarkDirty(cx, cz);
if (lx == 0) MarkDirty(cx - 1, cz);
if (lx == CX - 1) MarkDirty(cx + 1, cz);
if (lz == 0) MarkDirty(cx, cz - 1);
if (lz == CZ - 1) MarkDirty(cx, cz + 1);
```

- Records the edit so it survives a chunk unload/reload and gets saved.
- Marks the edited chunk dirty, so its mesh is rebuilt.
- **Also marks the neighbour dirty if the block sits on a border**, because the neighbour's
  mesh contains faces that were culled *against* this block. Change a border block and the
  neighbour's border faces must be reconsidered. Forgetting this is a classic voxel bug that
  shows as holes or double-walls at chunk seams.

`record = false` exists for internal writes that should not be persisted (used only during
tests). Note that even when `record` is false, the block itself is still written.

**Is this standard?** Yes — "mark the chunk and its border neighbours dirty" is exactly what
real voxel engines do.

---

## 11. Meshing — BuildMesh

This is the heart of the renderer. Goal: turn a chunk's block array into **one** mesh with
as few triangles as possible.

The naive alternative (which this project started with) is `DrawCube` per block: correct but
slow, because it is ~24,000 draw calls per chunk. One mesh per chunk is one draw call.

### Neighbour cache

```cpp
Chunk *nb[3][3];
for (int dz = -1; dz <= 1; dz++)
  for (int dx = -1; dx <= 1; dx++) { ... nb[dz + 1][dx + 1] = ...; }
```

Grabs the 3×3 neighbourhood of chunks once, up front. Meshing repeatedly needs to know about
blocks just outside this chunk (to cull border faces and to sample AO), and re-looking-up the
hash map for every sample would be slow.

### `at(lx, ly, lz)` — the neighbour-aware read

```cpp
auto at = [&](int lx, int ly, int lz) -> unsigned char {
  if (ly < 0 || ly >= CY) return AIR;
  int dx = lx < 0 ? -1 : (lx >= CX ? 1 : 0);
  int dz = lz < 0 ? -1 : (lz >= CZ ? 1 : 0);
  Chunk *n = nb[dz + 1][dx + 1];
  if (!n) return AIR;
  return n->blocks[Idx(lx - dx * CX, ly, lz - dz * CZ)];
};
```

Reads a block by **local** coordinate, transparently crossing into the correct neighbouring
chunk when the coordinate is out of `[0,CX)` or `[0,CZ)`. The `lx - dx*CX` wraps the local
coordinate into the neighbour's local space. If that neighbour is not loaded, it reads air.

This is the function that makes chunk borders correct. Without it, every chunk would look
like an isolated island with a wall around it.

### The main loop and face culling

```cpp
for (int y = 0; y < CY; y++)
  for (int z = 0; z < CZ; z++)
    for (int x = 0; x < CX; x++) {
      unsigned char b = c->blocks[Idx(x, y, z)];
      if (b == AIR) continue;
      for (int f = 0; f < 6; f++) {
        ...
        if (IsSolid(at(nx, ny, nz))) continue;   // face culling
```

**Face culling is the single biggest win in voxel rendering.** A cube has 6 faces, but any
face touching another block is invisible (it is inside solid rock). Only faces adjacent to
air are emitted. In a solid chunk this throws away the vast majority of geometry — a fully
buried block contributes nothing at all.

This is the standard first-line voxel optimization and it is not optional; without it you
would draw an order of magnitude too many triangles.

### UV mapping with a half-pixel inset

```cpp
float u0 = ((TileFor(b, f) * TILE) + 0.02f) / (TILE * TILES);
float u1 = (((TileFor(b, f) + 1) * TILE) - 0.02f) / (TILE * TILES);
float v0 = 0.02f / TILE, v1 = (TILE - 0.02f) / TILE;
```

Converts an atlas tile index into a normalized `[0,1]` UV range. The `0.02f` inset pulls the
sample points slightly inside the tile.

**Why the inset:** at exactly the tile boundary, floating-point rounding plus texture
filtering can sample the *neighbouring* tile, producing a one-pixel seam of the wrong colour
along block edges. Nudging inward avoids it. This is a well-known atlas issue and the inset
is a standard cheap fix.

### Vertex ambient occlusion

```cpp
int su = FACE_V[f][k][FACE_UA[f]] > 0.5f ? 1 : -1;
int sw = FACE_V[f][k][FACE_WA[f]] > 0.5f ? 1 : -1;
int s[3] = {0, 0, 0};  s[FACE_UA[f]] = su;
int t[3] = {0, 0, 0};  t[FACE_WA[f]] = sw;
int vx = x + FACE_N[f][0], vy = y + FACE_N[f][1], vz = z + FACE_N[f][2];
bool o1 = IsSolid(at(vx + s[0], vy + s[1], vz + s[2]));
bool o2 = IsSolid(at(vx + t[0], vy + t[1], vz + t[2]));
bool oc = IsSolid(at(vx + s[0] + t[0], vy + s[1] + t[1], vz + s[2] + t[2]));
int occl = (o1 && o2) ? 3 : ((int)o1 + (int)o2 + (int)oc);
float shade = FACE_SHADE[f] * (1.0f - 0.22f * occl);
```

**Ambient occlusion (AO)** darkens a face corner when solid blocks sit in the way of light
near that corner. It is what makes voxel worlds look like they have contact shadows in
creases, and it is the single biggest visual-quality-per-line-of-code win available.

For each of the face's 4 corners, it samples three neighbours in the layer *in front of* the
face: the two edge neighbours (`o1`, `o2`) and the diagonal corner (`oc`). Per `su`/`sw` it
picks the direction (±1) that points toward that specific corner.

```
occl = (o1 && o2) ? 3 : (o1 + o2 + oc)
```

The count of occluders, 0–3. The `o1 && o2 → 3` case is the standard **"corner fix"**: if
both edges are blocked, the corner is fully dark regardless of the diagonal, otherwise the
shading has an ugly discontinuity when looking at a corner-on view.

Then it is multiplied into the vertex colour:

```cpp
unsigned char cc = (unsigned char)(255 * shade);
cols.push_back(cc); cols.push_back(cc); cols.push_back(cc); cols.push_back(255);
```

**How does a vertex colour become shading?** raylib's default shader ends with:

```glsl
gl_FragColor = texelColor*colDiffuse*fragColor;
```

(verified in `rlgl.h`, the embedded default fragment shader). `texelColor` is the atlas
sample, `colDiffuse` is the material tint, and `fragColor` is the interpolated **vertex
colour**. So baking shade into vertex colour multiplies the texture, per vertex, with
smooth interpolation across the face — and **no custom shader is needed.** That is a
genuine win: it keeps the whole renderer inside raylib's default pipeline.

`0.22f` per occluder level and `FACE_SHADE`'s values are hand-tuned by eye. They are
calibration constants, not derived values.

**Is this standard?** The technique is exactly what Minecraft and every voxel clone do. The
specific implementation here is the **simpler, "flat" AO**: AO is computed per corner and
the face is split as a fixed quad (two triangles, `base,base+1,base+2` and `base,base+2,base+3`).

The *proper* version, which this deliberately does not do, is:
- **Flipped-quad triangulation**: choose the quad diagonal that avoids the "AO seam" artifact
  where two triangles meet at different brightnesses. It is a real visual artifact around
  corners and the standard fix is a few more lines.
- **Per-vertex smoothing without the corner fix** (or with anisotropy) for softer results.

So: correct technique, simplified execution. Documented as a ceiling rather than hidden.

### Mesh assembly and GPU upload

```cpp
Mesh m = {0};
m.vertexCount = (int)(verts.size() / 3);
m.triangleCount = (int)(idx.size() / 3);
if (m.vertexCount == 0) return m;
m.vertices = (float *)MemAlloc(verts.size() * sizeof(float));
memcpy(m.vertices, verts.data(), verts.size() * sizeof(float));
... texcoords, normals, colors ...
m.indices = (unsigned short *)MemAlloc(idx.size() * sizeof(unsigned short));
memcpy(m.indices, idx.data(), idx.size() * sizeof(unsigned short));
UploadMesh(&m, false);
```

Builds the raylib `Mesh` structure. Points worth knowing:

- Geometry is accumulated into `std::vector` first, then copied into raylib-allocated
  buffers. The `std::vector` is scratch memory; the `MemAlloc`'d buffers are owned by raylib
  and freed later by `UnloadMesh`. This split is required because raylib's `Mesh` stores raw
  `float*`/`unsigned char*`, not vectors.
- **`colDiffuse`/vertex colour is what carries AO and face shading** (see above).
- `UploadMesh(&m, false)` — the `false` means *static* (not expected to change every frame).
  Chunk meshes are rebuilt wholesale rather than edited in place, so static is correct and
  lets the driver put them in the fastest memory.
- **`unsigned short` indices** cap a chunk at 65,536 vertices. With 24,576 blocks a
  worst-case checkerboard chunk could exceed that. `triangleCount`/`vertexCount` are plain
  `int` so the numbers are tracked correctly, but a very pathological chunk could overflow
  the index buffer. In normal terrain the count stays far below. Worth knowing; a real fix
  is `unsigned int` indices or splitting the chunk.

**Is this hand-rolled vertex buffer assembly standard?** Yes — this is what every custom
voxel engine does. It is the reason voxel games do not use model files.

---

## 12. World generation

### `ApplyEdits(c)`

Walks the global `edits` list and re-applies any that fall inside this chunk. This is why a
saved build survives: chunks are always regenerated fresh from noise, then player edits are
painted on top. Ordering matters — generation first, edits last, so edits always win.

It is an **O(edits)** scan per chunk. With thousands of edits and hundreds of chunks that is
quadratic-ish. Documented as a ceiling; the standard fix is to bucket edits by chunk
coordinate.

### `GenChunkBlocks(c)` — terrain, caves, trees

```cpp
c->blocks.assign(CHUNK_VOL, AIR);
```

Clear to all-air (`AIR == 0`), so anything not explicitly written is empty.

**Terrain column:**

```cpp
int h = (int)floorf(TerrainHeight(wx, wz));
if (h < 1) h = 1;
if (h > CY - 2) h = CY - 2;
```

Clamped so the surface always leaves at least one air block above it — otherwise a
mountaintop could reach the world ceiling and generate a wall of solid to the top.

```cpp
bool beach = h <= SEA_LEVEL + 1;
for (int y = 0; y <= h; y++) {
  unsigned char b = (y == h) ? (beach ? SAND : GRASS)
                             : (y >= h - 4 ? DIRT : STONE);
  c->blocks[Idx(x, y, z)] = b;
}
```

The classic **layer stack**: grass on top, a few dirt below, stone under that. Low ground
becomes sand (beach). This is exactly Minecraft's grass/dirt/stone pattern.

**Caves:**

```cpp
for (int y = 1; y < h; y++)
  if (CaveNoise(wx * 0.09f, y * 0.13f, wz * 0.09f) > 0.74f)
    c->blocks[Idx(x, y, z)] = AIR;
```

Carves air where 3D noise exceeds a threshold. Note it samples **world** coordinates
(`wx`, `wz`), not local ones, which is vital: caves must be continuous across chunk borders.
If it used local coordinates every chunk would have its own unrelated cave set and you would
see caves stop dead at chunk edges.

The `0.13f` on Y makes the vertical frequency different from horizontal, producing
horizontally-stretched caves rather than cubes of noise. The threshold `0.74` controls how
much is carved — calibrated by eye.

`y` starts at 1, and the bedrock line is set after, so caves never punch through the world
floor.

**Trees:**

```cpp
for (int t = 0; t < 4; t++) {
  float r = Hash2(c->cx * 7919 + t, c->cz * 104729 + t * 31);
  if (r > 0.35f) continue;
  ...
}
```

Up to 4 candidate trees per chunk, kept with probability ~35%. But the *position* is derived
from **this chunk's** coordinate:

```cpp
int x = 2 + (int)(Hash2(...) * (CX - 4));
```

**This is the key design decision, and it introduces a real limitation.** Because tree spawn
is decided per-chunk, a tree is always generated by, and lives inside, exactly one chunk.
That makes it simple and stateless (no "did the neighbouring chunk already plant a tree
here?" problem), but it forces trees to be **at least 2 blocks from every border** (hence
`2 + ... * (CX - 4)`) so the canopy cannot spill into a neighbour. Without that inset, a tree
at the edge would be cut in half — you would see half a trunk in one chunk and nothing in
the next.

The tradeoff: trees never straddle chunk boundaries, and there is a distinct "column" where
trees never appear along chunk edges.

**Is this standard?** For a hobby engine, yes — many do exactly this. Bigger engines
generate structures with a global pass or a "structure starts in chunk X, spills into
neighbours" system that lets trees cross borders naturally. This is a documented, deliberate
simplification.

The trunk/canopy:

```cpp
int trunk = 4 + (int)(Hash2(...) * 3);
for (int y = h + 1; y <= h + trunk && y < CY; y++) c->blocks[Idx(x, y, z)] = WOOD;
for (int dy = -2; dy <= 1; dy++)
  for (int dz = -2; dz <= 2; dz++)
    for (int dx = -2; dx <= 2; dx++) {
      if (abs(dx) == 2 && abs(dz) == 2) continue;   // trim corners
      ...
      if (c->blocks[Idx(xx, yy, zz)] == AIR) c->blocks[Idx(xx, yy, zz)] = LEAVES;
    }
```

A 4–6 block trunk and a 5×5×4 leaf blob with the 4 corner columns removed (leaves are not
cubes in real Minecraft — the corner trim makes the canopy read as roundish). Leaves only
replace air, so they never punch through terrain.

Trees are also rejected on beaches and anywhere the surface is not grass:

```cpp
if (h <= SEA_LEVEL + 1 || c->blocks[Idx(x, h, z)] != GRASS) continue;
```

That check is why `height[x][z]` was cached in a local array during the terrain pass instead
of recomputing `TerrainHeight` — reuse, and it also lets the tree check know the surface.

### `LoadChunk(cx, cz)`

```cpp
GenChunkBlocks(c);          // blocks only; meshing is a separate budgeted phase
chunks[ChunkKey(cx, cz)] = c;
meshQueue.push_back({c, 0});
```

**Allocates, generates blocks, registers, and queues for meshing — but does not mesh.** This
split is the fix for the stutter described in Section 14. Before, `LoadChunk` also built the
mesh and rebuilt four neighbours, which is what froze the frame.

---

## 13. Chunk lifetime and dirty tracking

### `UnloadChunk(c)`

```cpp
if (c->hasMesh) UnloadMesh(c->mesh);
dirty.erase(std::remove(dirty.begin(), dirty.end(), c), dirty.end());
meshQueue.erase(std::remove_if(meshQueue.begin(), meshQueue.end(),
                               [&](const PendingMesh &p) { return p.c == c; }),
                meshQueue.end());
delete c;
```

Frees the GPU mesh (`UnloadMesh` — you must do this, GPU memory is not freed for you), then
**removes the chunk from both queues before deleting it**.

Those two `erase` calls are the important part. The dirty list and the mesh queue hold raw
`Chunk*`. If a chunk is unloaded while still queued, the queues keep a **dangling pointer**
and the next `FlushDirty()`/`ProcessQueues()` would touch freed memory — undefined behaviour,
typically a crash that looks random. Removing it from the queues is what makes unload safe.

This is the kind of bug that is invisible until it crashes at a random moment, which is why
it is called out here.

### `RebuildChunk(c)`

```cpp
if (c->hasMesh) { UnloadMesh(c->mesh); c->mesh = Mesh{0}; c->hasMesh = false; }
c->mesh = BuildMesh(c);
c->hasMesh = c->mesh.vertexCount > 0;
```

Releases the old mesh and builds a new one. Reset-to-`Mesh{0}` before rebuilding so a
half-updated state can never be drawn.

### `MarkDirty(cx, cz)` and `FlushDirty()`

```cpp
static void MarkDirty(int cx, int cz) {
  auto it = chunks.find(ChunkKey(cx, cz));
  if (it != chunks.end()) dirty.push_back(it->second);
}

static void FlushDirty() {
  std::sort(dirty.begin(), dirty.end());
  dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
  for (Chunk *c : dirty) RebuildChunk(c);
  dirty.clear();
}
```

`MarkDirty` looks up an existing chunk and queues it (silently ignoring unloaded ones).

`FlushDirty` **sorts and deduplicates** first. If you place a block on a chunk border, that
chunk and its neighbour both get marked; if you break a block next to a corner, the same
chunk can be marked several times in one frame. Removing duplicates means each chunk is
rebuilt at most once per frame rather than N times. `std::sort` + `std::unique` is the
standard idiom for deduplication on a small vector.

Note ordering: `FlushDirty` runs **once per frame**, after all edits for that frame have
accumulated. Rebuilding immediately on every click means a fast click-drag rebuilds the same
chunk dozens of times.

**Is this standard?** Yes — "mark dirty, deduplicate, rebuild once per frame" is the normal
pattern.

---

## 14. Chunk streaming (the stutter fix)

### The problem this solves

The original version did, inside `LoadChunk`, three expensive things:

1. generate blocks,
2. build the mesh,
3. **rebuild all four neighbours' meshes.**

Step 3 is the killer. A neighbour was meshed while this chunk was missing, so it culled its
faces against "air" and drew faces that should have been hidden. Once the data arrives those
neighbours must be re-meshed. Loading 5 chunks in one frame cost roughly 5× a chunk mesh,
plus hash lookups.

Measured worst frame while flying into fresh terrain: **~790 ms**. The GAME FROZE for most of
a second.

### The fix, in two parts

**Part 1 — never mesh without all four neighbours.** `HasAllNeighbors`:

```cpp
static bool HasAllNeighbors(Chunk *c) {
  return chunks.count(ChunkKey(c->cx - 1, c->cz)) &&
         chunks.count(ChunkKey(c->cx + 1, c->cz)) &&
         chunks.count(ChunkKey(c->cx, c->cz - 1)) &&
         chunks.count(ChunkKey(c->cx, c->cz + 1));
}
```

A chunk waits in the mesh queue until all four neighbours exist in the map. Then, when it is
finally meshed, `at()` (Section 11) can read the real neighbour data, so border faces cull
correctly **and AO reads across the seam correctly** — first time, no remesh required.

This turns a 5-mesh cascade into **1 mesh per chunk**, and it is why border seams are now
correct by construction.

`chunks.count()` returns 0 or 1, and `&&` short-circuits, which is why it reads as a boolean
expression.

**Part 2 — budget the work per frame.** `ProcessQueues`:

```cpp
for (int i = 0; i < GEN_BUDGET && !loadQueue.empty(); i++) {
  auto [cx, cz] = loadQueue.back();
  loadQueue.pop_back();
  if (chunks.find(ChunkKey(cx, cz)) == chunks.end()) LoadChunk(cx, cz);
}
int meshed = 0;
for (auto it = meshQueue.begin(); it != meshQueue.end() && meshed < MESH_BUDGET;) {
  if (!HasAllNeighbors(it->c)) { ++it; continue; }   // wait for neighbors
  RebuildChunk(it->c);
  it = meshQueue.erase(it);
  meshed++;
}
```

At most `GEN_BUDGET` chunks generated and `MESH_BUDGET` chunks meshed per frame. The cost per
frame is now **bounded and predictable** instead of "however many chunks happened to cross
the border this frame".

`meshQueue.erase(it)` returns the next valid iterator — erasing while iterating correctly
requires this (you cannot `++` an erased iterator, that is a dangling iterator bug).

The `if (chunks.find(...) == chunks.end())` re-check in the gen loop guards against a
duplicate being queued twice.

**Result: worst frame ~790 ms → ~25 ms.** (The remaining ~25 ms is GPU draw work for ~260
chunks, not generation — generation measured ~1 ms.)

### `RefreshChunks(pcx, pcz)`

```cpp
for (auto it = chunks.begin(); it != chunks.end();) {
  Chunk *c = it->second;
  if (abs(c->cx - pcx) > RENDER_DIST + 1 || abs(c->cz - pcz) > RENDER_DIST + 1) {
    UnloadChunk(c);
    it = chunks.erase(it);
  } else { ++it; }
}
```

Called only when the player crosses a chunk boundary. It (1) unloads chunks beyond the render
distance, keeping memory bounded, and (2) rebuilds the load queue for chunks that are now
missing.

Unload uses `RENDER_DIST + 1` (one chunk of slack): without the slack, a chunk would unload
mediately when you step one chunk over the line, then be needed again the moment you step
back — thrashing generate/free. One chunk of hysteresis avoids that. This is the same
"hysteresis margin" idea used in caches everywhere.

### The load queue and nearest-first sorting

```cpp
std::sort(loadQueue.begin(), loadQueue.end(), [&](const std::pair<int,int> &a,
                                                  const std::pair<int,int> &b) {
  int da = (a.first - pcx)*(a.first - pcx) + (a.second - pcz)*(a.second - pcz);
  int db = (b.first - pcx)*(b.first - pcx) + (b.second - pcz)*(b.second - pcz);
  return da > db;   // descending: pop_back() gives the nearest chunk
});
```

Sorts by **squared distance** from the player, descending, so `pop_back()` yields the nearest
chunk first. Since the gen budget is only 1–2 chunks per frame, load order is what the player
actually notices: the ground under their feet should appear before a hill 100 blocks away.

Squared distance avoids a `sqrt` — comparing squares preserves ordering, so the square root is
pure waste. Standard optimization.

There is no `std::priority_queue` here because the whole list is rebuilt on boundary crossing
and 289 entries sorts in negligible time; a heap would be more machinery for no gain.

**Is this streaming approach standard?** Yes conceptually — every open-world game loads
around the player with a per-frame budget rather than all at once. The specific detail (two
queues, neighbours-complete gate) is a clean way to get it right.

---

## 15. Player, movement, collision

### The struct and constants

```cpp
struct Player {
  Vector3 pos{8, 40, 8};
  Vector3 vel{0, 0, 0};
  float yaw = 0, pitch = 0;
  bool fly = false, onGround = false;
  float health = 20.0f, hunger = 20.0f;   // MC's 0..20 scale (10 hearts / 10 haunches)
};
```

Position, velocity, look angles, two state flags, and the two survival stats. `yaw` is
rotation around the vertical axis (left/right), `pitch` is up/down.

The damage state is **not** on the struct: `fallPeakY`, `invulnTimer`, `regenHold` and
`hurtFlash` are plain globals next to `player`, in the same style as `stepTimer` and `mineT`.
None of them is per-player, because there is one player. `player.health` is the only health a
hit changes.

```cpp
const float PW = 0.3f, PH = 1.8f, EYE = 1.62f, GRAVITY = 25.0f, JUMP = 8.0f;
const float WALK = 5.0f, FLY_SPEED = 14.0f, SENS = 0.003f;
```

| Constant | Meaning | Why this value |
|---|---|---|
| `PW = 0.3` | Player half-width | Full width 0.6 — narrower than a block, so you fit in a 1-block gap. Minecraft uses 0.6 too. |
| `PH = 1.8` | Player height | Just under 2 blocks, so you fit through a 2-block-tall gap. Matches Minecraft's 1.8. |
| `EYE = 1.62` | Camera height above feet | ~90% of height. Minecraft's exact value. |
| `GRAVITY = 25` | Blocks/s² | Tuned to feel snappy, not real gravity (9.8). Games always exaggerate gravity. |
| `JUMP = 8` | Initial jump velocity | With gravity 25 this gives a ~1.3-block jump — enough to climb one block. |
| `WALK = 5` | Blocks/s | Minecraft is ~4.3 walking. Tuned for feel. |
| `SENS = 0.003` | Mouse radians per pixel | Tuned. Real games let you change this — the calibration knob. |

These are **all hand-tuned for feel, not derived from physics.** That is normal and correct;
game feel is empirically calibrated. They are the constants to tweak.

### `SolidAt`, `CollidesAt`, `AABBOverlapsBlock`

```cpp
static bool SolidAt(int x, int y, int z) { return GetBlockWorld(x, y, z) != AIR; }

static bool CollidesAt(Vector3 p) {
  const float e = 0.001f;
  int x0 = (int)floorf(p.x - PW), x1 = (int)floorf(p.x + PW - e);
  int y0 = (int)floorf(p.y),        y1 = (int)floorf(p.y + PH - e);
  int z0 = (int)floorf(p.z - PW),   z1 = (int)floorf(p.z + PW - e);
  for (int x = x0; x <= x1; x++)
    for (int y = y0; y <= y1; y++)
      for (int z = z0; z <= z1; z++)
        if (SolidAt(x, y, z)) return true;
  return false;
}
```

`CollidesAt` is the core collision test: does a player box centered at `p` overlap any solid
voxel? It converts the box's min/max corners into the **integer range of voxels it touches**
and checks each one.

The `const float e = 0.001f` is an epsilon shrink on the max side. This matters: if the
player's max edge lands exactly on an integer boundary, `floorf` would include the *next*
voxel, causing the player to collide with a block they are only touching. The tiny epsilon
nudges the range back so a box that exactly abuts a block does not collide with it. This is
essential for per-axis collision to work (below), and is a classic off-by-one fix.

```cpp
static bool AABBOverlapsBlock(Vector3 p, int bx, int by, int bz) {
  return (p.x - PW < bx + 1 && p.x + PW > bx && p.y < by + 1 &&
          p.y + PH > by && p.z - PW < bz + 1 && p.z + PW > bz);
}
```

The standard **AABB vs AABB overlap test** (min < other's max on every axis) against one
specific block. Used to prevent placing a block *inside* the player — otherwise you would
place a block in your own body and get permanently stuck.

### `ForwardVec` and `RightVec`

```cpp
static Vector3 ForwardVec(float yaw, float pitch) {
  return Vector3{cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
}
// screen-right = cross(forward, up); for forward=(sin,0,cos) that is (-cos,0,sin)
static Vector3 RightVec(float yaw) {
  return Vector3{-cosf(yaw), 0, sinf(yaw)};
}
```

`ForwardVec` is the unit direction the camera looks, from yaw/pitch. At `yaw=0, pitch=0` it
is `+Z`.

`RightVec` is the horizontal vector pointing to the screen-right. It is derived as
`cross(forward, up)`:

- `up = (0,1,0)`
- `forward_flat = (sin(yaw), 0, cos(yaw))`
- `cross(forward_flat, up) = (0*0 - cos*1, cos*0 - sin*0, sin*1 - 0*0) = (-cos(yaw), 0, sin(yaw))`

**This function was the strafe bug.** It originally returned `(cos, 0, -sin)` — the exact
negation, i.e. the *left* vector — so D moved left and A moved right. The fix is the sign
flip above, and the self-test pins the behaviour so it cannot silently regress.

The comment records the derivation deliberately: this is a place where a future reader would
otherwise "helpfully" flip the signs back.

### `UpdatePlayer(dt)`

**Look:**

```cpp
Vector2 md = GetMouseDelta();
player.yaw -= md.x * SENS;
player.pitch -= md.y * SENS;
player.pitch = fmaxf(-1.5f, fminf(1.5f, player.pitch));
```

`GetMouseDelta()` is raylib's per-frame mouse movement. It requires `DisableCursor()`
(called in `main`) so the OS pointer is locked and does not hit the window edge — the standard
FPS mouse-look setup.

Yaw subtracts so moving the mouse right turns right. Pitch is **clamped to ±1.5 rad (~86°)**
so you cannot rotate past straight up/down, where the camera's up-vector would become
ambiguous and the view would flip. Every FPS game clamps pitch. `fminf`/`fmaxf` are the
float versions of `min`/`max`.

**Wish direction:**

```cpp
Vector3 fwd = ForwardVec(player.yaw, player.pitch);
Vector3 flat = Vector3Normalize(Vector3{fwd.x, 0, fwd.z});
Vector3 right = RightVec(player.yaw);

Vector3 wish{0, 0, 0};
if (IsKeyDown(KEY_W)) wish = Vector3Add(wish, flat);
if (IsKeyDown(KEY_S)) wish = Vector3Subtract(wish, flat);
if (IsKeyDown(KEY_D)) wish = Vector3Add(wish, right);
if (IsKeyDown(KEY_A)) wish = Vector3Subtract(wish, right);
```

`flat` is `fwd` with Y zeroed and re-normalized — so walking does not veer into the sky when
you look up. Inputs accumulate into `wish` (the desired direction) and are normalized later,
so moving diagonally is not faster than moving straight (W+D would otherwise be √2 times
speed). That normalization is a small detail that is *very* noticeable when missing.

`Vector3Normalize` from `raymath.h`.

**Fly vs walk:**

```cpp
if (player.fly) {
  if (IsKeyDown(KEY_SPACE)) wish.y += 1;
  if (IsKeyDown(KEY_LEFT_CONTROL)) wish.y -= 1;
  if (Vector3Length(wish) > 0) wish = Vector3Normalize(wish);
  player.vel = Vector3Scale(wish, FLY_SPEED);
} else {
  if (Vector3Length(wish) > 0) wish = Vector3Normalize(wish);
  player.vel.x = wish.x * WALK;
  player.vel.z = wish.z * WALK;
  player.vel.y -= GRAVITY * dt;
  player.onGround = false;
  if (IsKeyDown(KEY_SPACE) && player.onGround) player.vel.y = JUMP;
}
```

- **Fly** sets velocity directly from input (`Space`/`Ctrl` add vertical). No gravity. A debug
  convenience, toggled with F.
- **Walk** sets horizontal velocity from input, applies **gravity as an acceleration**
  (`vel.y -= GRAVITY * dt` — this is `v += a·t`, so faster frames do not change physics
  because `dt` scales it), and allows a jump only when `onGround`.

`player.onGround = false` is reset here every frame and re-set during collision resolution.
That "reset, then let collision confirm contact" ordering is what makes jumping reliable.

`dt` scaling everywhere is **essential for frame-rate independence.** Without it, the game
would run at different speeds on different hardware (and after the FPS cap change in the
bench mode).

**Per-axis collision resolution:**

```cpp
for (int axis = 0; axis < 3; axis++) {
  float d = axis == 0 ? player.vel.x * dt
                      : axis == 1 ? player.vel.z * dt
                                  : player.vel.y * dt;
  if (d == 0) continue;
  Vector3 old = player.pos;
  if (axis == 0) player.pos.x += d;
  else if (axis == 1) player.pos.z += d;
  else player.pos.y += d;
  if (CollidesAt(player.pos)) {
    player.pos = old;
    if (axis == 0) player.vel.x = 0;
    else if (axis == 1) player.vel.z = 0;
    else {
      if (d < 0) player.onGround = true;
      player.vel.y = 0;
    }
  }
}
```

**This is the standard simple voxel collision, and the per-axis split is the reason it
works.** The algorithm: move along one axis, test; if it collides, undo that axis only.

Why per-axis: if you moved all three axes at once and tested, a collision on any axis would
undo the whole move — so walking into a wall would also cancel your horizontal slide along it,
and you would stick to walls instead of gliding. By testing X, then Z, then Y separately, a
collision on X does not affect Z. That is what lets you press into a wall and slide along it.

The axes are ordered X, Z, then **Y last** on purpose:

- Y last means `onGround` is determined after horizontal motion, which is what you want.
- `if (d < 0) player.onGround = true` — only downward motion sets grounded. Moving *up* into
  a ceiling must not mark you as standing on the floor.

This is `vel = 0` on the blocked axis rather than a bounce or a slide-projection; for this
game's speeds it is indistinguishable from the fancier versions.

The `d == 0 continue` skips work when an axis is not moving — a free micro-optimization that
also avoids a pointless collision test.

**Void reset:**

```cpp
if (player.pos.y < -20) { player.pos = Vector3{8, 60, 8}; player.vel = Vector3{0, 0, 0}; }
```

Safety net. Because unloaded chunks read as air, a player can fall forever if they fall out
of the loaded region. This teleports them back to the spawn area. Every voxel game needs a
void-reset like this. It also re-pins `fallPeakY`, or the teleport would be charged as a fall.

**Fall tracking.** Two lines, in the gravity branch and after the collision loop:

```cpp
fallPeakY = fmaxf(fallPeakY, player.pos.y);                       // while airborne
if (!player.fly && player.onGround) {                             // on landing
  Hurt(FallDamage(FallDistance(fallPeakY, player.pos.y)));
  fallPeakY = player.pos.y;
}
```

Tracking lives here rather than in a separate pass because `UpdatePlayer` already knows both
the previous grounded state and the resolved position — anything else would recompute one or
stub out the other. `!player.fly` keeps flying from billing you for descending.

Note the landing branch fires on *every* grounded frame, not on a transition. That is
deliberate: `FallDistance` returns 0 when you are already on the floor, `FallDamage(0)` is 0,
and `Hurt(0)` returns without setting the i-frame timer. A transition flag would be one more
piece of state to reset in three places. The cost is one no-op call per frame.

**Return value:**

```cpp
Vector3 startPos = player.pos;
// ... per-axis move and resolve ...
float moved = sqrtf((pos.x-startPos.x)*(pos.x-startPos.x) + (pos.z-startPos.z)*(pos.z-startPos.z));
return moved;
```

`UpdatePlayer` returns the **horizontal distance actually travelled**, which is not the same as
`speed * dt`: pressing W into a wall gives full speed and zero distance. Hunger should charge
for walking, not for leaning on a wall, so the caller gets the corrected number instead of
re-deriving it (and the footstep timer already worked this way).

**Is this standard?** Yes. Per-axis AABB resolution is the textbook approach for voxel games
and is what most tutorials and clones use. Larger engines use swept-AABB or capsule colliders;
for a block world at these speeds, per-axis discrete resolution is the correct lazy choice
and does not tunnel at 5 blocks/s with 16 ms steps.

### Survival stats: `TickSurvival`

```cpp
static void TickSurvival(float dt, float moved, float hurtCooldown, float &health, float &hunger)
```

Pure — takes references, touches no globals. That is what lets the whole metabolism be tested
by `-selftest` without a window, a world, or a player.

```cpp
hunger = Clamp(hunger - dt * (HUNGER_IDLE_RATE + moved * HUNGER_WALK_RATE), 0.0f, MAX_STAT);
if (hunger <= 0.0f)
  health = Clamp(health - dt * 0.2f, 0.0f, MAX_STAT);
else if (hunger >= REGEN_MIN_HUNGER && hurtCooldown <= 0.0f && health < MAX_STAT) {
  float gain = fminf(dt * REGEN_RATE, MAX_STAT - health);
  health += gain;
  hunger = Clamp(hunger - gain * REGEN_HUNGER_COST, 0.0f, MAX_STAT);
}
```

Four rules, in the order they matter:

1. **Hunger always drains**, at a base rate plus a per-unit-of-distance term. Standing still is
   cheap but not free — otherwise hunger would never move in a game where the player idles.
2. **Starving at 0 hunger costs health.** Health is the reserve that runs out; there is no
   separate "you lose" state, which is why 0 HP is currently a no-op (death/respawn is a later
   M3 item).
3. **Regen needs a nearly full belly** (`hunger >= 18` of 20) and charges hunger for the HP it
   restores. Without the gate, healing would be free and hunger meaningless; without the cost,
   a player could stand at 18 hunger forever and never need food.
4. **Regen pauses right after a hit** (`hurtCooldown > 0`). Without this, 0.5 HP/s of healing
   outruns chip damage and the invulnerability window below becomes pointless — you would be
   better off taking a hit and healing than avoiding it.

The cooldown is a *parameter* rather than a global read, so `TickSurvival` stays pure and the
pause is testable the same way the rest is. `Hurt` is what sets it; the function only honours it.

`fminf(dt * REGEN_RATE, MAX_STAT - health)` caps the gain at the missing HP *before* charging
hunger for it, so a tick that overshoots full health does not overcharge. Same reason the
starvation branch clamps at 0.

**Drawing.** `DrawStatusBars` uses one loop over a table:

```cpp
const struct { float v; int x; Color bg, fg; } bars[2] = {
    {player.health, x0, {40, 0, 26, 255}, {215, 30, 30, 255}},
    {player.hunger, x0 + totalW - CELLS * (seg + gap), {40, 26, 0, 255}, {200, 140, 40, 255}},
};
```

Health grows from the left, hunger from the right, both 10 cells of 2 units. `ceilf(value)`
per cell gives half-cell granularity — a 1 HP player sees a half-heart, not an empty bar. A
third bar is one row in this table. Rectangles rather than heart/drumstick sprites: identical
information, nothing to draw or ship.

The bars are skipped in creative, matching the tick being skipped there — a bar that never
moves is noise.

### Damage sources: `FallDamage`, `FallDistance`, `Hurt`

```cpp
static float FallDamage(float dist)                  { return dist > FALL_SAFE ? dist - FALL_SAFE : 0.0f; }
static float FallDistance(float peakY, float landY)  { return peakY > landY ? peakY - landY : 0.0f; }
static bool  Hurt(float amount);                     // the only place a hit touches health
```

**`FallDamage`** is the whole Minecraft rule in one expression: the first three blocks of drop
are free, every block after costs 1 HP. It is pure so the threshold — the part that is easy to
get off by one — is pinned by `-selftest` rather than eyeballed in game.

**`FallDistance` measures an apex, it does not integrate.** The obvious implementation adds
`-vel.y * dt` every frame while descending, and it is wrong twice:

- it over-counts by up to one frame of travel, because the velocity used on the landing frame
  is the velocity *before* the collision zeroed it (a real 12-block drop read as 12.3, i.e. a
  third of a block of phantom damage);
- it invents distance while sliding down a wall, where `pos.y` is pinned but velocity is still
  negative, so a player gliding down a cliff face takes damage for a fall they never had.

Remembering the highest Y reached and subtracting the landing Y is the same line count and is
exact. The counter `fallPeakY` is pinned to the player's feet whenever `onGround`, and raised to
any new high while airborne. Consequences fall out of that one rule rather than needing cases:

- a jump is charged from its apex (1.3 blocks — under the grace, always free);
- landing on a ledge partway down a cliff charges only to that ledge, which is what MC does;
- walking off a cliff starts the peak at the cliff top.

`fallPeakY` is reset on the fly toggle and on the void respawn. Both are teleports; without the
reset, the next landing would be charged for the distance the teleport crossed.

**`Hurt` is the single choke point for taking damage.** Every source — and every source M3 adds
later — calls this one function, which owns:

- the **invulnerability window** (`INVULN_TIME`, 0.5 s): a second hit inside it is dropped and
  `Hurt` returns `false`. One fall cannot damage you twice across two frames.
- the **creative exemption**: `cfg.creative` returns `false` before anything happens. This is
  the reason to have a choke point at all — the guard is written once, in the function every
  damage source must call, instead of at each source site where `UpdatePlayer` would have had
  to remember it.
- the **regen hold** (`REGEN_HOLD_TIME`, 1.5 s): feeds `hurtCooldown` into `TickSurvival`.
- the clamp at 0 HP and the red flash.

Two separate timers rather than one, because they want different values and merging them makes
one of them wrong. 0.5 s is the limit on taking hits; 1.5 s is the limit on regenerating.
Separating them is two floats; sharing one would be a tuning knob that cannot be set correctly.

Starvation deliberately does **not** go through `Hurt` — it is damage over time, and MC lets it
tick through i-frames. It writes health inside `TickSurvival` with its own rate.

**Feedback.** `Hurt` sets `hurtFlash = 1`, which decays at 2.5/s and drives four edge bands:

```cpp
Color hurt = Fade({160, 0, 0, 255}, 0.55f * hurtFlash), none = Fade(hurt, 0.0f);
DrawRectangleGradientV(0, 0, sw, band, hurt, none);            // top, darkest at the edge
DrawRectangleGradientV(0, sh - band, sw, band, none, hurt);    // bottom
DrawRectangleGradientH(0, 0, band, sh, hurt, none);            // left
DrawRectangleGradientH(sw - band, 0, band, sh, none, hurt);    // right
```

**The gradient is not decoration — a flat band was tried first and looked wrong.** Over a
uniform sky, a solid 120 px strip of tint reads as *the top of the screen changing colour*, like
a weather effect, not as damage. A gradient makes the screen edge the darkest point and falls
off to nothing inward, which is the shape the eye already associates with "hurt". Same four
draw calls, `DrawRectangleGradientV/H` are already in raylib, so it costs no shader and no
texture — but it is the difference between the effect reading or not.

`DrawRectangleGradient*` rather than a radial vignette shader for the same reason as the bars:
the corner shading of a real radial vignette is not worth a shader and an art asset.

The hit sound reuses `sfxJump` at a low pitch — there is no damage sample loaded, and a thud
is a thud.

The main loop also skips ticking the damage timers in creative. That is belt and braces — `Hurt`
already refuses — but it keeps a stale `hurtFlash` from being carried into survival mode when you
toggle back.

**What is not here:** drowning. The plan item pairs it with fall damage, but it needs a water
block to detect ("head cell is water") and water is M4. Shipped without it rather than inventing
a water concept in the damage step.

---

## 16. Voxel raycast

```cpp
struct RayHit {
  bool hit = false;
  int x, y, z;      // hit voxel
  int px, py, pz;   // voxel before it (placement spot)
};
```

`RaycastVoxel` answers "what block is the crosshair pointing at". Returns **both** the hit
block (to break) and the last empty cell before it (to place into). That second value is why
placement works at all.

```cpp
static RayHit RaycastVoxel(Vector3 o, Vector3 dir, float maxDist) {
  int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z);
  int sx = dir.x > 0 ? 1 : -1, sy = ... , sz = ...;
  float tdx = fabsf(dir.x) < 1e-8f ? 1e30f : fabsf(1 / dir.x);
  ...
```

**This is the Amanatides & Woo voxel traversal algorithm** (the paper is "A Fast Voxel
Traversal Algorithm for Ray Tracing", 1987). It is *the* standard algorithm for stepping a
ray through a voxel grid, and it is what every Minecraft clone uses.

The idea: a ray crosses a grid of cells. Track, for each axis, the ray-parameter `t` at which
it will next cross a cell boundary in that axis (`tmx`, `tmy`, `tmz`). Step into whichever
axis has the smallest `t`, add that axis's delta (`tdx`, …) to its value, repeat.

```cpp
float tdx = fabsf(dir.x) < 1e-8f ? 1e30f : fabsf(1 / dir.x);
```

`1/|dir.x|` is how far along the ray one cell of width 1 costs in the X axis. A **guard for
`dir.x ≈ 0`** sets it to a huge number, effectively "this axis will never be crossed". Without
that guard you get division by (near) zero and infinities/NaNs propagating through the
comparisons.

```cpp
float tmx = (sx > 0 ? (x + 1 - o.x) : (o.x - x)) * tdx;
```

Initializes the distance to the first X boundary. The `sx > 0` branch picks the near or far
face depending on direction, so the algorithm is correct regardless of which way the ray
points.

```cpp
while (t <= maxDist) {
  if (SolidAt(x, y, z)) { ... r.px = px; r.py = py; r.pz = pz; return r; }
  px = x; py = y; pz = z;                       // remember where we came from
  if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; }
  else if (tmy < tmz)        { y += sy; t = tmy; tmy += tdy; }
  else                       { z += sz; t = tmz; tmz += tdz; }
}
```

Step loop. `maxDist` (6.0 in use) is the reach — the standard Minecraft reach is ~4.5–5
blocks, so 6 is generous but reasonable.

The `px/py/pz` bookkeeping is a nice simplification: rather than reverse-stepping from the
hit to find the empty cell, it just records the previous cell each iteration. One assignment
per step instead of extra math on hit.

**Is this standard?** Yes, exactly. This is the canonical algorithm with the canonical
degenerate-axis guard.

---

## 17. Save / load

```cpp
const char *SAVE_FILE = "world.dat";
const uint32_t MAGIC = 0x31434357;   // "WCC1"
```

A magic number identifies the file format, so a garbage or foreign file is detected instead
of misinterpreted. `0x31434357` is ASCII `"WCC1"` (little-endian). Standard practice.

### `SaveEdits()`

```cpp
std::vector<unsigned char> buf(8 + edits.size() * sizeof(Edit));
uint32_t count = (uint32_t)edits.size();
memcpy(buf.data(), &MAGIC, 4);
memcpy(buf.data() + 4, &count, 4);
if (count) memcpy(buf.data() + 8, edits.data(), count * sizeof(Edit));
SaveFileData(SAVE_FILE, buf.data(), (int)buf.size());
```

**The design decision: only player edits are saved, not the world.**

This is why saves are tiny (a few bytes per placed block) regardless of how far you have
explored. The terrain is fully reproducible from `TerrainHeight`/`CaveNoise`/the tree hash —
it is a pure function of coordinates — so saving it would be storing data that can be
recomputed. On load, chunks are regenerated and `ApplyEdits` paints the edits back.

This makes the file format trivially small and version-tolerant. The tradeoff: **change the
world-generation code and all existing saves change terrain** (edits stay, but the hills move
under them). It also assumes the generator is deterministic across platforms, which the
integer hash guarantees while floating-point noise does not perfectly. Both are documented
ceilings.

The file layout: `[magic u32][count u32][Edit * count]`.

`memcpy` is used to write exact bytes regardless of struct padding — writing the struct
directly to disk would be platform-dependent.

### `LoadEdits()`

```cpp
unsigned char *data = LoadFileData(SAVE_FILE, &size);
if (!data || size < 8) { if (data) UnloadFileData(data); return; }
memcpy(&magic, data, 4);
memcpy(&count, data + 4, 4);
if (magic == MAGIC && (int)(8 + count * sizeof(Edit)) <= size) {
  edits.resize(count);
  if (count) memcpy(edits.data(), data + 8, count * sizeof(Edit));
}
UnloadFileData(data);
```

`LoadFileData` hands back a raylib-allocated buffer (and the size), which must be freed with
`UnloadFileData` — hence both the early-return free and the final free.

**The validation is the part that matters.** A missing file, a truncated file, or a corrupt
`count` field all lead to a safe no-op rather than a crash:

- `!data` — file absent.
- `size < 8` — cannot even hold the header.
- `magic != MAGIC` — not our format.
- `8 + count*sizeof(Edit) <= size` — **the count field is checked against the actual file
  size before trusting it.** Without this, a corrupt `count` of 4 billion would make
  `edits.resize(count)` attempt a gigantic allocation and then `memcpy` read off the end of
  the buffer. This is the boundary-validation that must not be skipped.

This is a trust boundary (data from disk), and the check is deliberately not simplified away.

---

## 18. Items, mining, inventory, crafting

This section covers everything the player does after the world is built: what a block turns
into when broken, what you hold, how you carry it, and how you turn wood into tools.

### 18.1 One id space, no conversion table

```cpp
enum Item : unsigned char {
  ITEM_NONE = AIR,
  ITEM_STICK = 16,
  ITEM_PICKAXE,
  ITEM_AXE,
  ITEM_SHOVEL,
};
```

The roadmap originally called for a separate `enum class Item` mirroring `Block`. That would
mean a lookup table in both directions, and every new block would need two edits plus a rule
for "does this block have an item?" — a table that can silently drift out of sync with the
`Block` enum.

Instead **a block *is* its own item**: block items reuse the `Block` ids `1..7`, so
"place this stack" is `SetBlockWorld(..., held.item)`. Non-block items start at `16` to leave
headroom for new blocks (1–15) without renumbering anything. `ITEM_NONE = AIR = 0`, so
"empty slot" and "nothing" are the same value and `count == 0` is the only other check needed.

The cost of this decision: `IsBlockItem()` (`id < BLOCK_COUNT`) is required anywhere that
distinguishes a placeable block from a tool. That one predicate is cheaper than a table.

### 18.2 ItemStack

```cpp
struct ItemStack {
  unsigned char item = ITEM_NONE;
  unsigned char count = 0;
  unsigned short damage = 0;
};
```

`item` is an id from the space above, `count` is 0–64, and `damage` is *wear* — 0 is pristine,
`TOOL_MAX_DAMAGE` means the tool breaks. Keeping wear on the stack rather than on a per-tool
object is what lets the same struct describe both a 64-stack of dirt and a nearly-dead pickaxe.

`inv[36]` is the whole inventory: slots `0–8` are the hotbar, `9–35` the main rows.
`MAX_STACK` is 64.

### 18.3 AddItem / TakeItem / CountItem

```cpp
static int AddItem(unsigned char item, int count);   // returns what did NOT fit
```

The return value is the point. "Inventory full" has to be observable so the caller can drop
the leftover on the ground instead of quietly deleting it. Drops, crafting results and closing
the inventory all rely on it.

Filling order is **top up existing stacks first, then use empty slots** — otherwise every
pickup would open a new slot and the inventory would fragment into ones and twos.

Tools are a special case:

```cpp
if (IsToolItem(item)) {
  for (...) { if (s.count != 0) continue; s = ItemStack{}; s.item = item; s.count = 1; count--; }
  return count;
}
```

A tool never merges, because merging would combine two different `damage` values and one
of them would be lost. Two half-worn pickaxes must stay two pickaxes. `ClickSlot`'s merge
branch excludes tools for the same reason.

### 18.4 Mining: hardness, tool classes, break time

```cpp
static float Hardness(unsigned char b)     // seconds bare-handed
```

| Block | Hardness |
|---|---|
| Leaves | 0.25 s |
| Grass / Dirt / Sand | 0.55 s |
| Wood / Plank | 1.6 s |
| Stone | 3.0 s |
| default | 1.0 s |

Bare-handed numbers only. The tool layer sits on top of them:

```cpp
enum ToolClass { TOOL_NONE, TOOL_PICK, TOOL_AXE, TOOL_SHOVEL };
static int ClassOfBlock(unsigned char b);   // STONE→PICK, WOOD/PLANK→AXE, GRASS/DIRT/SAND→SHOVEL
static int ToolClassOf(unsigned char item); // ITEM_PICKAXE→PICK, etc.
```

```cpp
static float BreakTime(unsigned char b, unsigned char held) {
  bool matching = ClassOfBlock(b) != TOOL_NONE && ClassOfBlock(b) == ToolClassOf(held);
  if (b == STONE && !matching) return -1.0f;
  return matching ? Hardness(b) / TOOL_SPEED : Hardness(b);
}
```

Three rules live in that function:

1. **A matching tool is 4× faster** (`TOOL_SPEED`). Stone drops from 3.0 s to 0.75 s.
2. **A wrong tool gives no bonus at all** — an axe on stone is exactly as good as hands.
   Real Minecraft has a small penalty for some mismatches; not worth the table here.
3. **`-1` means "cannot be harvested."** The only block with that rule is stone, which is
   M2's *"stone needs the pickaxe"* requirement. The caller treats `-1` as "no progress, no
   break" rather than "extremely slow", so there is no way to brute-force through it by
   holding the button.

The mining loop therefore reads:

```cpp
float need = BreakTime(b, inv[hotbarSel].item);
if (need < 0.0f) { mining = false; mineT = 0.0f; }
else {
  mineT += dt;
  bool done = cfg.creative || mineT >= need;
  ...
}
```

Retargeting (looking at a different block) resets `mineT`. The progress bar under the
crosshair divides by the **same** `BreakTime`, so the bar can never disagree with the actual
break. This was a real bug risk: when the bar still used `Hardness()`, holding a pickaxe
filled up to 25% and stopped, because the bar and the break timer were computing different totals.

`Hardness` and `BreakTime` are both pure functions of `(block, held item)`, which is why the
self-test can pin them without a window or a world.

### 18.5 Durability

```cpp
const int TOOL_MAX_DAMAGE = 64, TOOL_WEAR_PER_BLOCK = 1;

static bool WearTool(ItemStack &s);   // true = it just broke
```

One wear point per block broken, with the held tool, in survival. At `TOOL_MAX_DAMAGE` the
stack is cleared — the tool is destroyed. `WearTool` works on a stack reference rather than on
the inventory, so it is testable directly; `WearHeldTool()` is the one-line adapter that
points it at the selected slot.

Creative mode breaks instantly and still wears nothing (no consumption in creative is the
existing rule), so a creative player cannot accidentally destroy a tool.

`DrawSlot` draws a durability bar under the icon when `damage > 0`, green → red as it
approaches breaking. That is the only feedback the player gets that a tool is about to fail,
so it is drawn from `damage / ToolMaxDamage` — the same numbers the wear logic uses.

`ponytail:` one durability value for every tool. A tier table (wood/stone/iron) is a 3-line
change to `ToolMaxDamage` when tiers exist; writing the table now would be speculative.

### 18.6 Drops

```cpp
struct Drop { Vector3 pos, vel; unsigned char item; int count; float age; };
```

Breaking a block in survival spawns one drop at its centre with a small random outward
velocity. Drops fall with gravity, settle on the first solid block below, bob sinusoidally,
are picked up within 1.6 m, and despawn after 120 s so the vector cannot grow without bound.

`BlockDrop()` is the yield table: grass yields **dirt** (minecraft's rule), leaves yield
nothing, everything else yields itself. Stone yields stone rather than cobblestone — one less
id for no gameplay loss right now.

Creative spawns no drops at all. Since nothing is consumed in creative, drops would never be
picked up and would pile up forever — that was observed and fixed during pass 1.

`DrawDrops` subtly indexes the atlas with `TileFor(d.item, ...)`, which assumes a block. It
guards with `IsBlockItem` and falls back to the item's icon colour, because the drop list is
typed by item id and nothing structurally prevents a tool from ending up in it.

### 18.7 Placement

Right click places the selected hotbar slot's block into the empty cell the ray hit, provided
the player's AABB does not overlap it. Placement consumes from the **selected slot
specifically**, not from the first matching stack anywhere in the inventory — otherwise the
hotbar count would not visibly tick down and the player would have no idea what was happening.

### 18.8 Inventory screen (`E`)

9 columns: three main rows on top, then a 14 px gap, then the hotbar row. LMB takes, places,
or swaps the whole stack. RMB takes one item, or places one. The slot under the cursor is
outlined; hovering shows the item name above the panel. The carried stack follows the mouse
and is **returned to the inventory when the screen closes**, dropping at your feet only if
there is genuinely no room — losing items on `Esc` would be a data-loss bug, not a challenge.

While the screen is open, mouse-look and player physics are skipped. Without that, the camera
spins every time the player moves the pointer to click a slot.

Panel geometry is derived from one set of numbers (`cell`, the crafting strip height) so the
crafting grid above and the inventory grid below cannot drift apart.

### 18.9 Crafting model

```cpp
struct Recipe { const char *name; unsigned char in[9]; unsigned char out; unsigned char outCount; };
```

A recipe is a flattened 3×3 pattern, `0` meaning "must be empty". The table:

| Recipe | Pattern | Result |
|---|---|---|
| planks | 1 wood, anywhere | 4 planks |
| sticks | 2 planks stacked vertically | 4 sticks |
| pickaxe | `PPP` / `_S_` / `_S_` | 1 pickaxe |
| axe | `PP_` / `PS_` / `_S_` | 1 axe |
| shovel | `P__` / `S__` / `S__` | 1 shovel |

(`P` = plank, `S` = stick.)

**Position independence** is the interesting part. A player expects to be able to put the wood
in the corner or the middle. Matching therefore compares the *occupied bounding box* rather
than raw cells:

```cpp
static bool BoundingCells(const unsigned char st[9], unsigned char out[9], int &w, int &h);
```

It finds the min/max occupied row and column, copies that sub-rectangle top-left-aligned into
a scratch grid, and reports its size. `MatchRecipe` compares the boxed player grid against the
boxed pattern, and the two must agree on **both dimensions and every cell**. A one-log recipe
therefore matches anywhere; a two-log diagonal does not match it (one extra ingredient breaks
it, because the bounding box is 3×3 and the recipe's is 1×1).

That also gives free rejection of near-misses: a 2-wide plank pair cannot satisfy the 3-wide
pickaxe head, even though it contains the right item types.

**Consumption happens when you take the result, not when you place ingredients**, matching
Minecraft. `TakeCraftResult` puts the output onto the cursor stack, then `ConsumeGrid` removes
one item from each non-empty cell:

```cpp
if (--grid[i].count == 0) grid[i] = ItemStack{};
```

If the cursor is holding something else and cannot take the output, nothing is consumed.

`craftGrid[9]` is global and **survives closing the inventory**, so ingredients are not
silently destroyed by pressing `E`. On shutdown `ReturnCraftGrid()` drains it back into the
inventory (or onto the floor), the same safety rule as the cursor stack.

### 18.10 Crafting UI

The crafting strip is drawn above the inventory grid, inside the same panel:

- 3×3 slot grid on the left, 44 px cells.
- An arrow, then the result slot, vertically centred on the grid.
- The recipe's name next to the result, so an unfamiliar pattern is self-explanatory.
- Clicking a grid cell behaves exactly like an inventory cell (`ClickSlot` is shared).
- The result slot only reacts to LMB and only when `CurrentRecipe() >= 0`.

`CurrentRecipe()` re-evaluates the grid every frame. It is a 9-cell scan over 5 recipes, so
there is nothing to cache; caching would only add a way for the preview to go stale.

### 18.11 Hotbar

The hotbar is the bottom row of `inv`, drawn from the same `DrawSlot` as the inventory, so a
slot looks identical in both places and a durability bar or stack count appears in whichever
view you are looking at. `1–9` and the mouse wheel change `hotbarSel`; the bar brightens
briefly on change (`hotbarAlpha`) and dims when idle.

Slot icons come from `ItemTile()`, which maps an item id to an atlas tile index, or `-1` when
there is no icon. Block icons pick the face that reads best as a thumbnail (grass shows
`grass_top`, wood shows bark). Tools use the three procedurally drawn tool tiles
(`TILE_PICKAXE`, `TILE_AXE`, `TILE_SHOVEL`), generated into the atlas at startup alongside the
blocks — no PNG assets to ship or lose.

`DrawSlot` still has a text fallback for items without an icon; it shrinks the font until the
name fits rather than letting it overflow the slot.

---

## 19. SelfTest

```cpp
static int SelfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char *what) {
    if (!ok) { TraceLog(LOG_ERROR, "FAIL: %s", what); fail++; }
  };
  ...
```

A dependency-free test (no framework, no fixtures) that runs with `CrafClone.exe -selftest`
and returns a non-zero exit code on failure so it can gate a build.

`TraceLog(LOG_ERROR, ...)` prints to the console so a failure is visible, and the returned
`fail` count becomes the process exit code.

What it pins down, and why each one is worth a permanent check:

```cpp
check(FloorDiv(-1, 16) == -1 && FloorDiv(-16, 16) == -1 && FloorDiv(15, 16) == 0, "FloorDiv");
```

Negative-coordinate floor division — the memory-corruption risk from Section 5.

```cpp
check(Idx(0, 0, 0) == 0 && Idx(1, 0, 0) == 1, "Idx");
```

The flat-index layout.

```cpp
Chunk *c = LoadChunk(0, 0);
... check(solid, "loaded terrain has blocks");
```

Generation actually produces terrain.

```cpp
SetBlockWorld(2, 60, 2, STONE);
RayHit h2 = RaycastVoxel(Vector3{2.5f, 65.5f, 2.5f}, Vector3{0, -1, 0}, 20.0f);
check(h2.hit && h2.y == 60 && h2.py == 61, "raycast hits placed block");
```

Raycast hits the right block **and reports the correct empty cell above it** (`py == 61`),
which is what placement depends on. A ray shot 65.5 → down hits the block at y=60; checking
`py == 61` verifies the placement target too.

```cpp
check(AABBOverlapsBlock(Vector3{2.5f, 60.2f, 2.5f}, 2, 60, 2), "AABB overlap");
check(!AABBOverlapsBlock(Vector3{2.5f, 62.5f, 2.5f}, 2, 60, 2), "AABB miss");
```

Both the positive and negative case of the place-inside-self guard — the negative case
catches an "always returns true" bug.

```cpp
Vector3 fwd0 = ForwardVec(0, 0), right0 = RightVec(0);
check(right0.x < -0.99f && fabsf(right0.z) < 1e-4f, "right at yaw0 is -X");
Vector3 fwd1 = ForwardVec(1.5707963f, 0), right1 = RightVec(1.5707963f);
check(fabsf(Vector3DotProduct(fwd1, right1)) < 1e-4f, "right perpendicular to forward");
check(fabsf(right1.z - 1) < 1e-4f && fabsf(right1.x) < 1e-4f, "right at yaw90 is +Z");
```

**The strafe-direction regression test.** It checks `RightVec` at two yaws, including
perpendicularity to forward — a sign flip fails all of these. This was added *after* the A/D
bug, and it was verified to fail on the old (buggy) formula, so it is a real test rather than
a decorative one.

Note the non-obvious correct value: at yaw 90° the right vector is **+Z**. That was
mis-asserted on the first attempt (`-Z`); the test now encodes the actual derivation.

```cpp
Chunk *c2 = LoadChunk(1, 0);
SetBlockWorld(16, 60, 2, STONE);
check(GetBlockWorld(16, 60, 2) == STONE, "border block readable across chunks");
check(GetBlockWorld(15, 60, 2) == AIR, "chunk 0 border not aliased");
```

Two chunks at a shared border: a block in chunk 1 at `x=16` is readable, and chunk 0 at
`x=15` is *not* accidentally the same cell. Catches local↔world coordinate conversion bugs
that would otherwise cause ghost blocks at seams.

```cpp
dirty.clear();
UnloadChunk(c2);
UnloadChunk(c);
chunks.clear();
```

Cleanup: clear the dirty queue **before** unloading, so nothing holds a freed pointer, then
free the chunks.

**Is having a self-test standard?** For a project this size, a framework would be overkill,
but *some* runnable check on the coordinate math is not optional — Sections 5, 15 and 16 are
all places where a silent, subtle bug is easy and expensive. This is the minimum viable
version of that.

### 19.1 M2 checks: items, tools, crafting

The item loop added checks that don't need a world at all, because the logic was written as
pure functions on purpose:

```cpp
check(BlockDrop(GRASS) == DIRT && BlockDrop(LEAVES) == ITEM_NONE, "block drop table");
check(Hardness(STONE) > Hardness(DIRT), "stone is harder than dirt");
```

```cpp
check(BreakTime(STONE, ITEM_NONE) < 0.0f, "bare hands cannot harvest stone");
check(BreakTime(STONE, ITEM_AXE) < 0.0f, "axe is not a pickaxe");
check(BreakTime(STONE, ITEM_PICKAXE) > 0.0f, "pickaxe harvests stone");
check(BreakTime(STONE, ITEM_PICKAXE) < Hardness(STONE), "pickaxe is faster than bare hands");
check(BreakTime(DIRT, ITEM_NONE) > 0.0f, "dirt is breakable bare-handed");
check(BreakTime(WOOD, ITEM_AXE) < BreakTime(WOOD, ITEM_NONE), "axe speeds up wood");
check(BreakTime(WOOD, ITEM_PICKAXE) == BreakTime(WOOD, ITEM_NONE), "wrong tool is no bonus");
```

The stone checks pin all three branches of `BreakTime` — the `-1` gate, the matching bonus,
and the "wrong tool is no bonus" fallthrough. The `ITEM_AXE` line is the one that catches a
class-comparison bug where any tool would satisfy any block.

```cpp
ItemStack t{ITEM_PICKAXE, 1, 0};
check(!WearTool(t) && t.damage == 1, "tool wears without breaking");
t.damage = (unsigned short)(ToolMaxDamage(t.item) - 1);
check(WearTool(t) && t.count == 0 && t.item == ITEM_NONE, "tool breaks at max damage");
ItemStack d{STONE, 5, 0};
check(!WearTool(d) && d.damage == 0 && d.count == 5, "non-tools do not wear");
```

Durability is checked at both edges: one point short of breaking, and exactly at the limit.
The last case guards against applying wear to a non-tool stack — which would silently eat a
stack of stone.

```cpp
inv[0] = ItemStack{ITEM_PICKAXE, 1, 7};
check(AddItem(ITEM_PICKAXE, 1) == 0 && inv[1].item == ITEM_PICKAXE && inv[1].damage == 0,
      "tools stack separately with their own wear");
```

The merging rule from §18.3, tested against the actual failure it prevents: a fresh tool must
not inherit the wear of the worn one.

```cpp
unsigned char g[9] = {0};
check(MatchRecipe(g) < 0, "empty grid crafts nothing");
g[0] = WOOD;
int r = MatchRecipe(g);
check(r >= 0 && RECIPES[r].out == PLANK && RECIPES[r].outCount == 4, "log -> 4 planks");
unsigned char g2[9] = {0}; g2[4] = WOOD;
check(MatchRecipe(g2) == r, "recipe position does not matter");
unsigned char g3[9] = {0}; g3[0] = WOOD; g3[8] = WOOD;
check(MatchRecipe(g3) < 0, "extra ingredient breaks the match");
```

This is the bounding-box normalization from §18.9, tested at the four points where it can
break: empty grid, correct match, shifted match, and one-too-many ingredients. The shifted
case is the whole reason the normalization exists; the extra-ingredient case catches a
matcher that only checks "does the pattern fit somewhere" rather than "is this exactly the
pattern".

```cpp
unsigned char pick2[9] = {0};
pick2[0] = PLANK; pick2[1] = PLANK;
pick2[3] = ITEM_STICK; pick2[6] = ITEM_STICK;
check(MatchRecipe(pick2) < 0, "a 2-wide shape is not the 3-wide pickaxe");
```

A near-miss with the right items in roughly the right shape, which a bounding-box comparison
rejects on width alone. Without the width check, a lenient matcher would happily craft a
pickaxe from a broken pattern.

```cpp
grid[4] = ItemStack{WOOD, 3, 0};
ConsumeGrid(grid);
check(grid[4].count == 2, "crafting consumes one");
grid[4].count = 1;
ConsumeGrid(grid);
check(grid[4].count == 0, "last ingredient clears the cell");
```

Both consumption paths — decrement, and clearing the cell on the last item. The second case
is what stops an empty stack from lingering with `count == 0` and `item != ITEM_NONE`, which
would make the grid look occupied.

### 19.2 M3 checks: survival metabolism and damage

`TickSurvival` takes its two stats by reference and its cooldown by value, and touches no\nglobals, so these checks
cover the whole system with plain floats:

```cpp
float hp = 20.0f, hg = 20.0f;
TickSurvival(1.0f, 0.0f, 0.0f, hp, hg);
check(hg < 20.0f && hp == 20.0f, "idle hunger drains, health untouched");
float hg2 = 20.0f;
TickSurvival(1.0f, 5.0f, 0.0f, hp, hg2);
check(hg2 < hg, "walking drains hunger faster than standing");
```

The second check is the one that catches a drain formula that ignores `moved` — a plausible
bug, since the term is only visible when you compare two different distances.

```cpp
hp = 10.0f; hg = 19.0f;
for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // 1 s fed
check(hp > hpBefore && hg < hgBefore, "fed regen heals and costs hunger");
```

Sixty sub-steps rather than one `dt = 1.0`, because it pins that the logic is **step-size
independent** — the property that matters when the frame rate varies. A formula that only
worked for a single big frame would pass a one-shot test and fail in play.

```cpp
hp = 10.0f; hg = 17.9f;
// 1 s just below the gate
check(hp == 10.0f, "no regen below the hunger gate");
```

The gate boundary, tested from the wrong side. Without this, raising `REGEN_MIN_HUNGER` to 0
would still pass every other check.

```cpp
hp = 10.0f; hg = 0.0f;
// 1 s starving
check(hp < 10.0f, "starvation damages health");
```

```cpp
hp = 20.0f; hg = 20.0f;
for (int i = 0; i < 600; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);  // 10 s idle
check(hp == 20.0f && hg > 19.9f, "stats stay in [0..20] (no drift past full)");
```

Ten seconds of ticks. This is the check that would catch a missing `Clamp` — the values drift
by fractions of a unit per frame and the damage only shows up after a few hundred ticks, which
is exactly the class of bug that survives manual testing.

```cpp
hp = 0.05f; hg = 0.0f;
for (int i = 0; i < 600; i++) TickSurvival(1.0f / 60.0f, 1.0f, 0.0f, hp, hg);  // long starvation
check(hp == 0.0f, "starvation cannot go below zero");
```

The lower bound of the same clamp. A negative health would render as a full bar again through
the `ceilf` in `DrawStatusBars`, which would look like the game healing you as you starve.

```cpp
hp = 10.0f; hg = 20.0f;
for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.5f, hp, hg);   // 1 s in i-frames
check(hp == 10.0f, "no regen during the post-hit hold");
for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // hold expired
check(hp > 10.0f, "regen resumes once the hold ends");
```

Both halves are needed. The first alone passes if `hurtCooldown` is ignored entirely and regen
is broken; the second alone passes if regen is broken. Together they pin that the parameter is
actually consulted *and* that it clears.

```cpp
check(FallDamage(0.0f) == 0.0f && FallDamage(3.0f) == 0.0f, "falls up to 3 blocks are free");
check(FallDamage(3.01f) > 0.0f, "the 4th block hurts");
check(FallDamage(8.0f) == 5.0f, "8-block fall costs 5 HP");
check(FallDamage(30.0f) >= MAX_STAT, "a long fall is lethal in one hit");
```

The boundary from both sides. `FallDamage(3.0f) == 0` at exactly the free distance and
`FallDamage(3.01f) > 0` one step past it is what catches `>=` vs `>` — the off-by-one that is
invisible in play and would let a 4-block drop be free forever.

```cpp
check(FallDistance(10.0f, 0.0f) == 10.0f, "drop is apex minus landing");
check(FallDistance(0.0f, 10.0f) == 0.0f, "rising is not a fall");
check(FallDistance(34.0f, 22.0f) == 12.0f, "landing on a ledge charges only to the ledge");
```

`FallDistance(0, 10)` — landing *above* the peak — must be 0, not -10. That is the sign error
that would turn every jump into healing.

```cpp
cfg.creative = false;                    // survival rules; creative is immune
player.health = MAX_STAT;
invulnTimer = 0.0f; regenHold = 0.0f; hurtFlash = 0.0f;
check(Hurt(4.0f) && player.health == 16.0f, "Hurt applies damage");
check(!Hurt(4.0f) && player.health == 16.0f, "second hit inside i-frames is ignored");
check(regenHold > 0.0f && hurtFlash > 0.0f, "a hit locks regen and flashes the screen");

cfg.creative = true;
check(!Hurt(4.0f) && player.health == 16.0f, "creative takes no damage");
cfg.creative = false;
```

`Hurt` is the one function that touches globals here, so the check saves and restores them. The
second assertion is the entire point of the i-frame window: same call, no effect, returns `false`
— so a caller can tell whether its hit landed. Without it, a fall that straddles two frames
would damage twice.

The last pair pins the creative guard. It is in `Hurt` rather than at the call site on purpose:
`UpdatePlayer` is where the fall lands, but it is not where the *rule* belongs, and a mob coming
in M3 would have to repeat the check.

The wiring itself (`fallPeakY` tracking and the landing call inside `UpdatePlayer`) is not
unit-tested — it reads input and needs a world. It was verified once with a temporary probe that
ran `UpdatePlayer` 400 times from `y = 70` with no input and logged health: **20 → 0** for a
48-block drop, and **20 → 11.1** from `y = 34` (12 blocks, 9 damage). The second number is why
`FallDistance` replaced the per-frame sum, which reported 10.7 for the same drop.

---

## 20. main()

**Argument handling:**

```cpp
if (argc > 1 && (strcmp(argv[1], "-selftest") == 0 || strcmp(argv[1], "--selftest") == 0)) {
  InitWindow(16, 16, "CrafClone selftest");
  SetTraceLogLevel(LOG_ALL);
  int rc = SelfTest();
  CloseWindow();
  return rc;
}
```

A tiny 16×16 window is created even for the test because **`BuildMesh` calls `UploadMesh`,
which requires a live OpenGL context.** You cannot unit-test meshing headless without one.
The window is invisible-ish and closed immediately.

`SetTraceLogLevel(LOG_ALL)` makes raylib print everything, so failures are visible.

**Startup:**

```cpp
InitWindow(1280, 720, "CrafClone");
bool bench = (argc > 1 && strcmp(argv[1], "-bench") == 0);
SetTargetFPS(bench ? 0 : 60);
DisableCursor();
```

- `SetTargetFPS(60)` caps the frame rate — reduces power/heat and gives even pacing. In
  bench mode it is **uncapped**, because with a cap you would measure the sleep time, not the
  work.
- `DisableCursor()` locks and hides the OS pointer for mouse-look (paired with
  `EnableCursor()` at shutdown). Without it, the pointer escapes the window.

```cpp
atlas = MakeAtlas();
mat = LoadMaterialDefault();
mat.maps[MATERIAL_MAP_DIFFUSE].texture = atlas;
mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
```

`LoadMaterialDefault()` gives raylib's built-in material (its default shader, plus the white
1×1 fallback texture). Swapping the DIFFUSE map to our atlas is what makes the meshes textured
while staying on the default shader. `color = WHITE` means no extra tint (`colDiffuse` in the
shader).

```cpp
LoadEdits();
LoadChunk(0, 0);
for (int y = CY - 1; y >= 0; y--)
  if (SolidAt(8, y, 8)) { player.pos = Vector3{8.5f, (float)y + 1.001f, 8.5f}; break; }
```

Edits first, then the spawn chunk — **ordering matters**, because the spawn scan reads blocks
and would find nothing if the chunk were not loaded yet. The scan goes top-down for the first
solid block and places the player just above it (`+ 1.001f` leaves a hair of clearance so the
player is not born embedded in the ground). The `y` here is a **feet** height (camera is
`y + EYE`).

**Frame loop:**

```cpp
float dt = fminf(GetFrameTime(), 0.05f);
```

`GetFrameTime()` is seconds since the last frame. **The `0.05f` clamp is important:** if the
window is dragged, or the machine hitches, `dt` can spike to seconds, and unclamped that
would teleport the player through walls (large step → collision test passes on both sides) and
fling them out of the world. Clamping to 50 ms (a 20 fps floor) keeps physics stable. This is
the standard "variable timestep with a maximum" approach.

```cpp
int pcx = FloorDiv((int)floorf(player.pos.x), CX);
int pcz = FloorDiv((int)floorf(player.pos.z), CZ);
if (pcx != lastCX || pcz != lastCZ) { RefreshChunks(pcx, pcz); lastCX = pcx; lastCZ = pcz; }
ProcessQueues();
```

Streaming only reacts to **chunk boundary crossings** (`pcx != lastCX`), not every frame —
rebuilding the load queue every frame would be wasteful. `ProcessQueues` is called every
frame to spend the budget.

```cpp
Vector3 eye = {player.pos.x, player.pos.y + EYE, player.pos.z};
Vector3 dir = ForwardVec(player.yaw, player.pitch);
RayHit hit = RaycastVoxel(eye, dir, 6.0f);
```

The camera eye is the player's feet plus eye height. `dir` is the look vector. Note the
camera used for rendering and the ray used for targeting are built from **the same** `eye`
and `dir`, so the crosshair always points where the ray goes. Keeping these consistent is
what makes aiming feel right.

**Edit actions:**

```cpp
if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
  SetBlockWorld(hit.x, hit.y, hit.z, AIR);
if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) &&
    GetBlockWorld(hit.px, hit.py, hit.pz) == AIR &&
    !AABBOverlapsBlock(player.pos, hit.px, hit.py, hit.pz))
  SetBlockWorld(hit.px, hit.py, hit.pz, HOTBAR[selected]);
```

- Left click **breaks** the hit block (sets it to air).
- Right click **places** in the cell *before* the hit, guarded by two conditions: the target
  must actually be empty (do not overwrite a block), and it must not overlap the player (do
  not trap yourself).

`IsMouseButtonPressed` is edge-triggered (fires once per click); `IsMouseButtonDown` would
place continuously while held. For building, once-per-click is the sane default.

```cpp
for (int i = 0; i < HOTBAR_N; i++)
  if (IsKeyPressed(KEY_ONE + i)) selected = i;
float wheel = GetMouseWheelMove();
if (wheel != 0) selected = (selected + (wheel > 0 ? HOTBAR_N - 1 : 1)) % HOTBAR_N;
```

`KEY_ONE + i` works because raylib defines the number keys contiguously (`KEY_ONE` is 49,
`KEY_SEVEN` is 55), so adding `i` steps through them. Relying on that is a small assumption
about the enum, but a documented and stable one.

The wheel wraps: `+ HOTBAR_N - 1` then `% HOTBAR_N` is a safe way to subtract 1 with wrap for
unsigned-ish values.

```cpp
if (IsKeyPressed(KEY_F11)) {
  if (!borderless) {
    savedW = GetScreenWidth(); savedH = GetScreenHeight();
    int mon = GetCurrentMonitor();
    SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
    ToggleBorderlessWindowed();
    borderless = true;
  } else {
    ToggleBorderlessWindowed();
    SetWindowSize(savedW, savedH);
    borderless = false;
  }
}
```

**Fullscreen via borderless windowed, not `ToggleFullscreen()`.** `ToggleFullscreen()` uses
*exclusive* fullscreen, which switches the display's video mode — slow, can re-arrange other
windows, and can leave the desktop in a wrong resolution if the program crashes. Borderless
windowed sizes the window to the desktop and removes the chrome, which is what most modern
games default to: instant, no mode switch, alt-tab friendly.

`GetCurrentMonitor()` is used so it works on a multi-monitor setup (it goes fullscreen on
whichever monitor the window is on). The old size is saved and restored on toggle back.

```cpp
FlushDirty();
```

Once per frame, after all edits, rebuild whatever changed — see Section 13.

**Camera and draw:**

```cpp
Camera3D cam = {0};
cam.position = eye;
cam.target = Vector3Add(eye, dir);
cam.up = Vector3{0, 1, 0};
cam.fovy = 70;
cam.projection = CAMERA_PERSPECTIVE;
```

A raylib `Camera3D`. Position is the eye; target is eye + look direction (a camera looks at a
point, so the target is "one unit ahead"). `fovy = 70` is a common FPS field of view.

```cpp
BeginDrawing();
ClearBackground(SKYBLUE);
BeginMode3D(cam);
for (auto &kv : chunks)
  if (kv.second->hasMesh)
    DrawMesh(kv.second->mesh, mat,
             MatrixTranslate((float)(kv.second->cx * CX), 0, (float)(kv.second->cz * CZ)));
```

`ClearBackground` before `BeginMode3D` (it works in 2D mode). Inside 3D mode, every loaded
chunk with a mesh is drawn. `MatrixTranslate` positions the chunk's mesh at its **world**
origin — the mesh vertices are built in *local* chunk coordinates, and the translate matrix
puts them in place. This is why meshing never needs world coordinates: the transform does it
on the GPU.

This loop is where the remaining frame cost lives: ~260 `DrawMesh` calls, one per chunk. Each
draws only visible faces (culled in `BuildMesh`), but there is no *frustum* culling — chunks
behind the camera are still submitted. That is the next optimization if perf ever matters
(Section 22).

```cpp
if (hit.hit)
  DrawCubeWires(Vector3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f}, 1.002f, 1.002f, 1.002f, BLACK);
```

The block-highlight outline. `+0.5f` centers it on the block (block coordinates are at
corners). `1.002f` makes it very slightly larger than 1.0 so the wireframe sits *outside* the
block surface — at exactly 1.0 it would z-fight with the block faces and flicker.

```cpp
DrawLine(GetScreenWidth() / 2 - 10, GetScreenHeight() / 2, ...);
```

The crosshair, centered with `GetScreenWidth/Height` so it stays centered in fullscreen.
(It was previously hardcoded to 640/360 — correct at 1280×720 and wrong everywhere else.)

```cpp
DrawFPS(10, 10);
DrawText(TextFormat("LMB break  RMB place  1-7/wheel pick  F fly(%s)  F5 save  F11 fullscreen  chunks:%d queued:%d",
                    player.fly ? "on" : "off", (int)chunks.size(), (int)loadQueue.size()),
         10, 40, 18, DARKGRAY);
```

`DrawFPS` is a built-in raylib overlay. `TextFormat` is raylib's `printf` that returns a
`const char*` for `DrawText` (it uses an internal static buffer, so the result is only valid
until the next call — fine for immediate draw).

**Bench mode:**

```cpp
if (benchLeft > 0) {
  float now = (float)GetTime();
  float frameMs = (now - benchT1) * 1000.0f;
  benchT1 = now;
  if (benchLeft < 260 - benchWarm) benchMaxMs = fmaxf(benchMaxMs, frameMs);
  benchLeft--;
  if (benchLeft == 0) break;
}
```

A self-contained performance test: fly the player through fresh terrain (in the loop above)
and track the **worst** frame time, after skipping a warm-up period. It runs a fixed number
of frames and exits, printing the result. `benchMaxMs` is the number that proves streaming is
bounded.

The warm-up skip matters because the very first frames include one-off costs (shader compile,
first upload) that would otherwise dominate the max and hide the real number.

**Shutdown:**

```cpp
SaveEdits();
SaveSettings();
ReturnCraftGrid();   // ingredients left in the crafting grid go back to the player
for (auto &kv : chunks) UnloadChunk(kv.second);
chunks.clear();
UnloadTexture(atlas);
EnableCursor();
CloseWindow();
```

Order: save, free all chunk meshes and chunks, free the atlas, restore the cursor, close the
window. Every raylib resource that was created is released — meshes via `UnloadMesh` (inside
`UnloadChunk`), the window via `CloseWindow`, the texture via `UnloadTexture`. Not freeing
them would leak VRAM; on a desktop OS the process exit would reclaim it, but doing it
correctly is the habit, and it is required if this ever becomes a library or a restarting
loop.

`ReturnCraftGrid()` runs at shutdown for the same reason `CloseInventory()` returns the
cursor stack: **anything the player owns must be accounted for before the process dies.**
Ingredients sitting in the crafting grid are the player's items, so they go back into the
inventory, or onto the ground if there is no room. A save/load of the inventory itself does
not exist yet, so this is only about not destroying things mid-session.

---

## 21. Is this the standard implementation?

Short answer: **the individual algorithms are the standard ones, chosen correctly; the
architecture is deliberately simplified in specific, documented places.**

### Genuinely standard / correct

| Thing | Status |
|---|---|
| Chunked world with a hash map of loaded chunks | Standard |
| Face culling (only emit faces adjacent to air) | Standard, and mandatory |
| One mesh per chunk, `UploadMesh` | Standard |
| Vertex AO baked into vertex colour | Standard technique |
| Per-face texture lookup via an atlas | Standard |
| Amanatides & Woo DDA raycast | **The** canonical algorithm, correctly implemented incl. the degenerate-axis guard |
| Per-axis AABB collision resolution | Standard for voxel games |
| Floor division for negative coordinates | Standard and correctly handled |
| fBm noise, 4 octaves | Canonical |
| Fixed timestep clamp + `dt`-scaled movement | Standard |
| Budgeted chunk streaming with nearest-first ordering | Standard approach |
| Border blocks mark neighbour chunks dirty | Standard, and correctly done |
| Magic-numbered save file | Standard |
| Edge-triggered vs held input | Correct distinction |
| Deterministic integer hash for procedural placement | Standard |

### Deliberately simplified (with the ceiling named)

| Simplification | Ceiling | The "real" version |
|---|---|---|
| Naive face culling, no greedy meshing | More triangles than needed | **Greedy meshing** — merge coplanar faces into large quads. Big win, notably more code. |
| No frustum culling | Chunks behind camera still submitted | Test each chunk's AABB against the camera frustum before drawing. |
| Single-octave 3D cave noise | Blobby caves | Multi-octave 3D fBm, or worm/cave-carver algorithms. |
| Value noise, not Perlin/Simplex | Blockier terrain | Gradient noise; or a library like FastNoiseLite. |
| Trees confined to one chunk | Trees cannot cross chunk borders | Global structure pass, or structure-straddles-chunks system. |
| Save stores edits only | Changing the generator changes old saves | Store generated chunks on disk too (real Minecraft stores per-chunk region files). |
| `unsigned short` mesh indices | A pathological chunk could exceed 65k vertices | `unsigned int` indices, or split large chunks. |
| No threading | Generation/meshing competes with the frame | Worker threads generating & meshing while the main thread draws. |
| Single file, globals | Hard to grow beyond a certain size | Split into modules (world, mesh, player, render). |
| Block = 1 byte, no compression | ~24 KB/chunk | Bit-packing or palette compression. |
| Flat AO (no flipped-quad triangulation) | Slight AO seam artifact at some corners | Flip the quad diagonal based on AO values. |
| No lighting propagation | Baked face shade only | Flood-fill light values through blocks (Minecraft's light engine). |
| No water | `SEA_LEVEL` only places beaches | A water block with a non-solid, translucent render pass. |
| `ApplyEdits` linear scan | O(edits) per chunk | Bucket edits by chunk coordinate. |
| Block ids reused as item ids, no `Item` enum | Items and blocks share one id space; `IsBlockItem` needed at each boundary | A separate item table with block→item mapping. |
| Tool bonus is 4× or nothing | No per-tool/per-block speed matrix | A `[tool][block]` multiplier table like Minecraft's. |
| One durability value for all tools | No material tiers | Per-material max damage. |
| Recipes are exact 3×3 patterns, bounding-box normalized | No mirrored variants, no shapeless recipes, no tags/ingredient alternatives | Mirror matching, ingredient tags (`any_plank`), recipe JSON. |
| `MatchRecipe` is a linear scan over 5 recipes | O(recipes) whenever the crafting grid changes | Hash the normalized grid into a map. |

### The honest summary

This is a **correct, conventional voxel engine with the well-known optimizations in place
(face culling, chunked meshing, vertex AO, budgeted streaming, DDA raycast) and the well-known
advanced optimizations deliberately omitted (greedy meshing, frustum culling, threading,
lighting).** Nothing here is an unusual or wrong way to build a voxel game. The areas where it
departs from a shipped engine are all *performance and scale* concerns, not correctness
concerns — and each of those departures is marked in the code with a `ponytail:` comment or
documented here.

On the gameplay side the same principle applies: the item loop (ids, stacks, mining time,
tools, durability, crafting) is the standard Minecraft model, with the *content* — how many
tools, how many recipes, whether there are tiers — deliberately kept at the minimum that
proves the system works. Adding a block or a recipe touches one table entry, not the logic.

The places where *correctness* required real care, and got it, are:

1. **Negative-coordinate floor division** (Section 5) — a silent memory-corruption bug if wrong.
2. **Queue cleanup on chunk unload** (Section 13) — a use-after-free if wrong.
3. **Mining time and the progress bar must use the same function** (Section 18.4) — they
   disagreed once, and the symptom (bar stalling at 25%) looked like a UI bug rather than a
   logic bug.
4. **Item conservation** (Sections 18.3, 18.8, 18.9) — every path that can strand an item
   (full inventory, closing the crafting screen, quitting with ingredients in the grid) returns
   it rather than deleting it.

Both have runnable checks.

---

## 22. Known ceilings and known bugs

### Known ceiling — perf

Worst measured streaming frame is **~8–10 ms** at render distance 8 (`-bench`, Release,
uncapped, 261 chunks / 211 meshed). That figure replaced an earlier ~25 ms one and the two are
**not comparable**: the old harness advanced the camera `40*dt` per frame, so a faster frame
travelled less and streamed fewer chunks — the metric partly measured its own feedback loop.
The harness now moves a fixed 0.5 units/frame and reports `chunks=` / `meshed=`, so a
half-loaded world cannot masquerade as a fast one.

Numbers from this machine are noisy — occasional 50–200 ms outliers appear with **no code
change**, which is OS/AV scheduling, not the game. Compare medians of several runs.

The levers, in order: **frustum culling**, then **greedy meshing**.

### Known ceiling — fog is linear

`FOG_FS` mixes linearly between `fogStart` and `fogEnd`. On long sightlines the band can look
flat compared with an exponential curve. Add an octave or switch to `1 - exp(-d*k)` in the
fragment shader if it becomes noticeable.

### Known ceiling — vertex index cap

Chunk meshes use `unsigned short` indices, so a chunk cannot exceed 65,536 vertices. With
`CX×CZ = 16×16` and caves, the current worst case is far below that, but greedy meshing
(reduces vertices) or a taller chunk (increases them) would both push on this limit.

### Known ceiling — audio is non-positional

Sounds are synthesised noise bursts with random pitch jitter, not recorded samples, and there
is no `SetSoundPan` by direction — a break 8 blocks behind you sounds the same as one at your
feet (only distance-attenuated footsteps exist).

### Known ceiling — chunk-border tree column

Trees never appear within 2 blocks of a chunk border (Section 12), because they must stay
inside their own chunk. Visually this means a thin cross-shaped region where trees are rarer.

### Known ceiling — AO seams

Flat AO without flipped-quad triangulation can show a faint diagonal discontinuity at some
corners.

### Known ceiling — one durability value

Every tool has the same 64-point durability and every block costs one point. `ponytail:` this
is deliberate — a per-material/per-block table is a 3-line change to `ToolMaxDamage` and
`WearTool` once tiers (stone/iron) exist. Writing it before there is a second tier would be
guessing at numbers.

### Known ceiling — no inventory persistence

`world.dat` stores block edits only. The inventory, hotbar selection and crafting grid are
rebuilt in memory each launch; the starter stack is re-granted at startup. A full player save
should land with M3 (health/hunger) so it is written once, not twice.

### Known ceiling — death has no consequence yet

`health` and `hunger` tick and draw, and fall damage now reduces health, but reaching 0 HP still
does nothing: you stand up at 0 health and regen as soon as you eat. That is the next plan item
(`Death + respawn`) — the world-spawn teleport and the inventory drop already exist as pieces
(the void reset and the drop entities), so it is assembly rather than new systems.

The regen delay promised alongside this step **did ship**: `Hurt` sets `regenHold` for 1.5 s and
`TickSurvival` honours it as a parameter. It became observable the moment a fall could hurt you.

### Known ceiling — `bench.png` is not a 1:1 capture

`-bench` reports `GetScreenWidth/Height` as **1280×720** but writes a **1920×1080** `bench.png`,
and the drawn geometry does not map onto those pixels linearly (a 120-logical-pixel edge band
measured 120 px across the X axis and ~480 px down the Y axis in the same image). Time-of-day
also advances between runs, so a pixel diff against a second run is meaningless unless the clock
is pinned.

What this means in practice: `bench.png` is trustworthy for **"does it render and look right"**
— that is how the crafting-panel overflow and the vignette's flat-band problem were both caught —
but not for measuring coordinates. Verify geometry from the code and from `GetScreenWidth`, not
from pixel scans of this file. If exact extents ever matter, fix the capture resolution first
rather than trusting arithmetic on the image.

### Cosmetic leftovers and dead state

- `PendingMesh::attempts` is declared and unused (it was for a retry counter that the
  neighbour-gate made unnecessary).
- `BLOCK_COUNT` is only used as a range sentinel, never as a real block id.
- `ItemTile()` returns `-1` for `ITEM_STICK`, so the stick falls back to the shrinking-text
  path in `DrawSlot`. It is legible but inconsistent with the other items; a small drawn stick
  tile would fix it.

None of these affect behaviour.

### Things that were bugs and are now fixed and covered

- **Strafe direction** — `RightVec` returned the negated (left) vector; A and D were swapped.
  Fixed, and pinned by `-selftest`.
- **Jump never fired** — `player.onGround = false` was assigned immediately before the
  `if (... && player.onGround)` jump test, so the condition could never be true. Now the
  previous frame's grounded state is captured first (`wasGrounded`).
- **Chunk-gen stutter** — ~790 ms frame stalls; fixed by the two-phase budgeted queues.
- **Crosshair/HUD hardcoded to 1280×720** — replaced with `GetScreenWidth/Height`, needed for
  fullscreen.
- **Heap corruption on shutdown** — raylib sound aliases were unloaded with `UnloadSound`,
  which freed the shared sample data out from under the remaining aliases. Base sounds now use
  `UnloadSound`, aliases `UnloadSoundAlias`.
- **Progress bar disagreed with mining** — the bar divided by `Hardness(b)` while the break
  test used the tool-adjusted time, so holding a pickaxe filled the bar to 25% and stalled.
  Both now call `BreakTime`.
- **Creative grew the drop list forever** — nothing is consumed in creative, so drops were
  never picked up and never despawned within reach. Creative now spawns no drops.
- **Bench harness measured itself** — described under *Known ceiling — perf* above.
- **Crafting UI escaped its panel** — the result slot was right-aligned off the panel edge
  and the inventory title overlapped the crafting grid. Both found by looking at `bench.png`,
  not by reasoning about the numbers.

---

## 23. Build and verify

```powershell
# configure (Release is required for meaningful perf)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# build
cmake --build build

# run
.\build\bin\CrafClone.exe

# run the correctness self-test
.\build\bin\CrafClone.exe -selftest      # exit code 0 = pass

# run the streaming performance benchmark
.\build\bin\CrafClone.exe -bench         # prints worst streaming frame time
```

### Controls

| Input | Action |
|---|---|
| `W A S D` | Move |
| Mouse | Look |
| `Space` | Jump (walk) / up (fly) |
| `Ctrl` | Down (fly only) |
| `F` | Toggle fly / walk |
| Left click | Break block (hold — mining takes time) |
| Right click | Place selected block |
| `1`–`9`, wheel | Select hotbar slot |
| `E` | Inventory + 3×3 crafting |
| `C` | Toggle creative mode |
| `F5` | Save now (also auto-saves on exit) |
| `F11` | Toggle fullscreen |
| `Esc` | Pause menu (not quit) |

#### Interaction rules worth knowing

- **Survival bars.** Hearts left, hunger right, above the hotbar. Hidden in creative.
  Hunger drains slowly when idle and faster the further you actually walk; at 18+ hunger it
  regenerates health (and burns hunger doing it); at 0 hunger you start losing health.
- **Falls hurt.** The first 3 blocks of any drop are free, then 1 HP per block — a 12-block drop
  costs 9. The screen edges flash red when a hit lands, and regeneration pauses for a moment
  after it. Falls are the only damage source so far; 0 HP does not yet kill you.

- **Mining takes time.** Hold LMB on a block; a bar under the crosshair fills. Releasing or
  looking away resets it. Layers: leaves 0.25 s, dirt/sand/grass 0.55 s, wood/plank 1.6 s,
  stone 3.0 s — all bare-handed.
- **Stone needs a pickaxe.** Without one there is no progress bar and the block never breaks.
  Wood → planks → sticks → pickaxe, all craftable from a tree.
- **A matching tool is 4× faster.** Pickaxe on stone, axe on wood/planks, shovel on
  dirt/sand/grass. A *wrong* tool gives no bonus at all.
- **Tools wear out.** One point per block broken, 64 points total. The slot shows a
  green→red bar once damaged; at zero the tool disappears.
- **Inventory.** `E` opens 9×4 with a 3×3 crafting grid above. LMB = whole stack, RMB = one.
  The carried stack returns to the inventory when you close the screen.
- **Crafting takes effect when you take the result**, not when you arrange the ingredients —
  so mistakes cost nothing. Ingredient position inside the 3×3 does not matter.
- **Recipes:** 1 wood → 4 planks · 2 planks vertical → 4 sticks · 3 planks over 2 sticks
  (pickaxe) · `PP`/`PS`/`_S` (axe) · `P`/`S`/`S` (shovel).
- **Creative (`C`)** breaks instantly, consumes nothing, and drops nothing. It also refills
  the starter block stack when toggled on.

### Dependencies

- **raylib 5.5**, vendored in `libs/raylib` (headers + static lib). Linked with `winmm`,
  `gdi32`, `opengl32` — the standard Windows raylib link set.
- Nothing else. The standard library covers the rest, and the noise is hand-written.

### File map

| File | Purpose |
|---|---|
| `src/main.cpp` | The entire game |
| `CMakeLists.txt` | Build config (C++17, links raylib + Windows libs) |
| `libs/raylib/` | Vendored raylib |
| `plan.md` | The step-by-step build plan |
| `checklist.md` | Completed steps, ceilings, backlog |
| `PlanM.md` | Status ledger of every plan item (done/pending/edited/deleted/deferred) |
| `DOCUMENTATION.md` | This file |
| `world.dat` | Saved edits (created at runtime) |