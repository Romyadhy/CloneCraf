// CrafClone — Minecraft clone (single file)
// Step 9 (M1 "feel"): day/night cycle, fog shader, procedural audio, break particles,
// held-item view, pause menu + settings. See ROADMAP.md.
#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

// ---------- world layout ----------
const int CX = 16, CZ = 16, CY = 96;   // chunk dims
const int RENDER_DIST = 8;             // chunks around player
const int GEN_BUDGET = 1;              // chunks generated per frame (stutter knob)
const int MESH_BUDGET = 2;             // chunk meshes built per frame (stutter knob)
const int SEA_LEVEL = 24;
// ponytail: caves use a single-octave 3D noise pass in the gen function, find the
// `CaveNoise` call and add an octave there if the pockets look too blobby.
constexpr int CHUNK_VOL = CX * CZ * CY;

enum Block : unsigned char {
  AIR = 0, GRASS, DIRT, STONE, SAND, WOOD, LEAVES, PLANK, BLOCK_COUNT
};

static inline int Idx(int x, int y, int z) { return x + CX * (z + CZ * y); }
static inline int FloorDiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }
static inline uint64_t ChunkKey(int cx, int cz) {
  return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}
static inline bool IsSolid(unsigned char b) { return b != AIR; }

// ---------- noise (hash value noise, no dependency) ----------
// ponytail: cheap hash noise; swap for FastNoiseLite only if terrain looks too regular
static inline float Hash2(int x, int z) {
  uint32_t n = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u;
  n = (n ^ (n >> 13)) * 1274126177u;
  n ^= n >> 16;
  return (float)(n & 0xFFFFFF) / (float)0xFFFFFF;
}
static inline float Hash3(int x, int y, int z) {
  uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 1103515245u +
              (uint32_t)z * 668265263u;
  n = (n ^ (n >> 13)) * 1274126177u;
  n ^= n >> 16;
  return (float)(n & 0xFFFFFF) / (float)0xFFFFFF;
}
static inline float SmoothT(float t) { return t * t * (3.0f - 2.0f * t); }

static float ValueNoise(float x, float z) {
  int xi = (int)floorf(x), zi = (int)floorf(z);
  float tx = SmoothT(x - xi), tz = SmoothT(z - zi);
  float a = Hash2(xi, zi), b = Hash2(xi + 1, zi);
  float c = Hash2(xi, zi + 1), d = Hash2(xi + 1, zi + 1);
  return Lerp(Lerp(a, b, tx), Lerp(c, d, tx), tz);
}

static float CaveNoise(float x, float y, float z) {
  int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
  float tx = SmoothT(x - xi), ty = SmoothT(y - yi), tz = SmoothT(z - zi);
  float c000 = Hash3(xi, yi, zi),         c100 = Hash3(xi + 1, yi, zi);
  float c010 = Hash3(xi, yi + 1, zi),     c110 = Hash3(xi + 1, yi + 1, zi);
  float c001 = Hash3(xi, yi, zi + 1),     c101 = Hash3(xi + 1, yi, zi + 1);
  float c011 = Hash3(xi, yi + 1, zi + 1), c111 = Hash3(xi + 1, yi + 1, zi + 1);
  float x00 = Lerp(c000, c100, tx), x10 = Lerp(c010, c110, tx);
  float x01 = Lerp(c001, c101, tx), x11 = Lerp(c011, c111, tx);
  return Lerp(Lerp(x00, x10, ty), Lerp(x01, x11, ty), tz);
}

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

// ---------- atlas ----------
// tiles: 0 grass_top 1 grass_side 2 dirt 3 stone 4 sand 5 wood_side 6 wood_top 7 leaves 8 plank
//        9 pickaxe 10 axe 11 shovel (item icons only, never sampled by world geometry)
const int TILE = 16, BLOCK_TILES = 9, TILES = 12;
const int TILE_PICKAXE = 9, TILE_AXE = 10, TILE_SHOVEL = 11;

// tile base colours, shared by the atlas and the break particles
static const Color ATLAS_BASE[TILES] = {
    {95, 159, 53, 255},   {134, 96, 67, 255},   {134, 96, 67, 255},
    {128, 128, 128, 255}, {219, 207, 163, 255}, {104, 78, 47, 255},
    {160, 130, 90, 255},  {60, 120, 45, 255},   {170, 133, 85, 255},
    {150, 150, 155, 255}, {150, 150, 155, 255}, {150, 150, 155, 255}};

static const Color HANDLE_COL = {120, 84, 50, 255};
static const Color OUTLINE_COL = {45, 45, 50, 255};

// One 16x16 tool icon into tile `t`: a diagonal handle plus a head that differs per tool.
// Bold shapes + a dark outline, because at 48 px in a slot a subtle icon reads as "a stick".
static void DrawToolTile(Image &img, int t, int tool) {
  int ox = t * TILE;
  Color metal = ATLAS_BASE[t];
  Color edge = {200, 200, 205, 255};
  // handle, outlined so it reads against the slot background
  for (int i = -1; i < 3; i++)
    ImageDrawLine(&img, ox + 2 + i, 15, ox + 8 + i, 6, (i < 0 || i > 2) ? OUTLINE_COL : HANDLE_COL);
  if (tool == TILE_PICKAXE) {          // wide T bar with drooping ends
    ImageDrawRectangle(&img, ox + 3, 0, 11, 4, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 4, 1, 9, 2, metal);
    ImageDrawRectangle(&img, ox + 2, 3, 3, 3, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 12, 3, 3, 3, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 3, 4, 2, 2, metal);
    ImageDrawRectangle(&img, ox + 12, 4, 2, 2, metal);
    ImageDrawRectangle(&img, ox + 5, 1, 7, 1, edge);
  } else if (tool == TILE_AXE) {       // chunky wedge on the right of the handle
    ImageDrawRectangle(&img, ox + 8, 0, 8, 8, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 9, 1, 6, 6, metal);
    ImageDrawRectangle(&img, ox + 9, 1, 6, 1, edge);
    ImageDrawRectangle(&img, ox + 5, 2, 4, 3, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 6, 3, 3, 2, metal);
  } else {                             // shovel: square scoop
    ImageDrawRectangle(&img, ox + 7, 0, 8, 9, OUTLINE_COL);
    ImageDrawRectangle(&img, ox + 8, 1, 6, 7, metal);
    ImageDrawRectangle(&img, ox + 8, 1, 6, 1, edge);
  }
}

static Texture2D MakeAtlas() {
  Image img = GenImageColor(TILE * TILES, TILE, WHITE);
  for (int i = 0; i < TILES; i++)
    ImageDrawRectangle(&img, i * TILE, 0, TILE, TILE, ATLAS_BASE[i]);
  for (int i = 0; i < BLOCK_TILES; i++)   // speckle so tiles aren't flat; tools stay clean
    for (int k = 0; k < 60; k++) {
      int px = i * TILE + (int)(Hash2(i * 77 + k, k * 13) * TILE);
      int py = (int)(Hash2(k * 29, i * 31 + k) * TILE);
      Color c = ATLAS_BASE[i];
      c.r = (unsigned char)(c.r * 0.85f);
      c.g = (unsigned char)(c.g * 0.85f);
      c.b = (unsigned char)(c.b * 0.85f);
      ImageDrawPixel(&img, px, py, c);
    }
  ImageDrawRectangle(&img, 1 * TILE, 0, TILE, 5, ATLAS_BASE[0]);   // grass_side green strip
  ImageDrawRectangle(&img, 5 * TILE + 5, 0, 6, TILE, ATLAS_BASE[6]);   // wood_side bark highlight
  DrawToolTile(img, TILE_PICKAXE, TILE_PICKAXE);
  DrawToolTile(img, TILE_AXE, TILE_AXE);
  DrawToolTile(img, TILE_SHOVEL, TILE_SHOVEL);
  Texture2D t = LoadTextureFromImage(img);
  UnloadImage(img);
  SetTextureFilter(t, TEXTURE_FILTER_POINT);
  return t;
}

static int TileFor(unsigned char b, int face) {
  switch (b) {
    case GRASS: return face == 2 ? 0 : (face == 3 ? 2 : 1);
    case DIRT: return 2;
    case STONE: return 3;
    case SAND: return 4;
    case WOOD: return (face == 2 || face == 3) ? 6 : 5;
    case LEAVES: return 7;
    case PLANK: return 8;
  }
  return 3;
}

// face order: 0:+Z 1:-Z 2:+Y 3:-Y 4:-X 5:+X
static const int FACE_N[6][3] = {{0, 0, 1},  {0, 0, -1}, {0, 1, 0},
                                 {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}};
static const float FACE_V[6][4][3] = {
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},   // +Z
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},   // -Z
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},   // +Y
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},   // -Y
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},   // -X
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},   // +X
};
static const float FACE_UV[6][4][2] = {
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}}, {{0, 1}, {1, 1}, {1, 0}, {0, 0}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}}, {{0, 1}, {1, 1}, {1, 0}, {0, 0}},
};
static const float FACE_SHADE[6] = {0.75f, 0.75f, 1.0f, 0.5f, 0.85f, 0.85f};
// tangent axes per face, used for the AO side samples
static const int FACE_UA[6] = {0, 0, 0, 0, 2, 2};
static const int FACE_WA[6] = {1, 1, 2, 2, 1, 1};

// ---------- chunk ----------
struct Chunk {
  int cx, cz;
  std::vector<unsigned char> blocks;
  Mesh mesh{};
  bool hasMesh = false;
};

std::unordered_map<uint64_t, Chunk *> chunks;
Texture2D atlas;
Material mat;

// ---------- fog shader ----------
// Linear fog blends distant terrain into the sky colour, hiding chunk pop-in at the
// render-distance edge (raylib 5.5's C API has no fog call, so this is a small shader).
// ponytail: linear fog, swap to exponential if the band looks too flat.
static const char *FOG_VS = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragPos;
void main() {
  fragTexCoord = vertexTexCoord;
  fragColor = vertexColor;
  fragPos = vec3(matModel*vec4(vertexPosition, 1.0));
  gl_Position = mvp*vec4(vertexPosition, 1.0);
}
)";

static const char *FOG_FS = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragPos;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec4 fogColor;
uniform float fogStart;
uniform float fogEnd;
out vec4 finalColor;
void main() {
  vec4 texelColor = texture(texture0, fragTexCoord);
  finalColor = texelColor*colDiffuse*fragColor;
  float dist = length(viewPos - fragPos);
  float f = clamp((fogEnd - dist)/(fogEnd - fogStart), 0.0, 1.0);
  finalColor = mix(fogColor, finalColor, f);
}
)";

Shader fogShader;
int locViewPos = -1, locFogColor = -1, locFogStart = -1, locFogEnd = -1;

// ---------- settings (persisted to settings.txt) ----------
struct Settings {
  int renderDist = RENDER_DIST;
  float fov = 70.0f;
  float sensitivity = 0.003f;   // same default as SENS, which is declared further down
  float volume = 0.7f;
  bool creative = true;         // creative skips item consumption and mines instantly
} cfg;

static void SaveSettings() {
  char buf[128];
  snprintf(buf, sizeof(buf), "%d %.2f %.5f %.2f %d\n", cfg.renderDist, cfg.fov,
           cfg.sensitivity, cfg.volume, cfg.creative ? 1 : 0);
  SaveFileText("settings.txt", buf);
}

static void LoadSettings() {
  char *txt = LoadFileText("settings.txt");
  if (!txt) return;
  int rd = cfg.renderDist, cr = cfg.creative ? 1 : 0;
  float fv = cfg.fov, sn = cfg.sensitivity, vo = cfg.volume;
  // 4 fields = file written before creative mode existed; keep it readable
  int n = sscanf(txt, "%d %f %f %f %d", &rd, &fv, &sn, &vo, &cr);
  if (n >= 4) {
    cfg.renderDist = (int)Clamp((float)rd, 4.0f, 16.0f);
    cfg.fov = Clamp(fv, 50.0f, 110.0f);
    cfg.sensitivity = Clamp(sn, 0.0005f, 0.02f);
    cfg.volume = Clamp(vo, 0.0f, 1.0f);
  }
  if (n >= 5) cfg.creative = (cr != 0);
  UnloadFileText(txt);
}

// ---------- day / night ----------
const float DAY_LENGTH = 600.0f;   // seconds per full cycle
float dayTime = 0.25f;             // 0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight

static float SunHeight() { return cosf((dayTime - 0.25f) * 2.0f * PI); }
static float DayBrightness() { return Clamp(0.30f + 0.70f * (SunHeight() * 0.5f + 0.5f), 0.30f, 1.0f); }
static unsigned char Byte(float f) { return (unsigned char)(Clamp(f, 0.0f, 1.0f) * 255.0f); }
static Color SkyColor() {
  Color night{10, 14, 32, 255}, dusk{70, 70, 110, 255}, day{120, 170, 235, 255};
  float f = DayBrightness();                 // 0.30 night .. 1.0 noon
  float t = Clamp((f - 0.30f) / 0.70f, 0.0f, 1.0f);
  Color a = (t < 0.5f) ? night : dusk;
  Color b = (t < 0.5f) ? dusk : day;
  float u = (t < 0.5f) ? t * 2.0f : (t - 0.5f) * 2.0f;
  return Color{(unsigned char)Lerp((float)a.r, (float)b.r, u),
               (unsigned char)Lerp((float)a.g, (float)b.g, u),
               (unsigned char)Lerp((float)a.b, (float)b.b, u), 255};
}

// edits are kept as coords so infinite terrain survives reload
struct Edit {
  int x, y, z;
  unsigned char b;
  int _pad;
};
std::vector<Edit> edits;

// chunk streaming queues, declared here because LoadChunk() feeds them
std::vector<std::pair<int, int>> loadQueue;   // chunk coords, nearest last
struct PendingMesh { Chunk *c; int attempts; };
std::vector<PendingMesh> meshQueue;

// ---------- block access ----------
static unsigned char GetBlockWorld(int x, int y, int z) {
  if (y < 0 || y >= CY) return AIR;
  int cx = FloorDiv(x, CX), cz = FloorDiv(z, CZ);
  auto it = chunks.find(ChunkKey(cx, cz));
  if (it == chunks.end()) return AIR;
  return it->second->blocks[Idx(x - cx * CX, y, z - cz * CZ)];
}

static void MarkDirty(int cx, int cz);

static void SetBlockWorld(int x, int y, int z, unsigned char b, bool record = true) {
  if (y < 0 || y >= CY) return;
  int cx = FloorDiv(x, CX), cz = FloorDiv(z, CZ);
  auto it = chunks.find(ChunkKey(cx, cz));
  if (it == chunks.end()) return;
  int lx = x - cx * CX, lz = z - cz * CZ;
  it->second->blocks[Idx(lx, y, lz)] = b;
  if (record) {
    edits.push_back({x, y, z, b, 0});
    MarkDirty(cx, cz);
    if (lx == 0) MarkDirty(cx - 1, cz);
    if (lx == CX - 1) MarkDirty(cx + 1, cz);
    if (lz == 0) MarkDirty(cx, cz - 1);
    if (lz == CZ - 1) MarkDirty(cx, cz + 1);
  }
}

// ---------- meshing (face culling + vertex ambient occlusion) ----------
static Mesh BuildMesh(Chunk *c) {
  // neighbor chunk cache so border faces cull correctly and AO bleeds across borders
  Chunk *nb[3][3];
  for (int dz = -1; dz <= 1; dz++)
    for (int dx = -1; dx <= 1; dx++) {
      auto it = chunks.find(ChunkKey(c->cx + dx, c->cz + dz));
      nb[dz + 1][dx + 1] = it == chunks.end() ? nullptr : it->second;
    }
  auto at = [&](int lx, int ly, int lz) -> unsigned char {
    if (ly < 0 || ly >= CY) return AIR;
    int dx = lx < 0 ? -1 : (lx >= CX ? 1 : 0);
    int dz = lz < 0 ? -1 : (lz >= CZ ? 1 : 0);
    Chunk *n = nb[dz + 1][dx + 1];
    if (!n) return AIR;
    return n->blocks[Idx(lx - dx * CX, ly, lz - dz * CZ)];
  };

  std::vector<float> verts, uvs, norms;
  std::vector<unsigned char> cols;
  std::vector<unsigned short> idx;

  for (int y = 0; y < CY; y++)
    for (int z = 0; z < CZ; z++)
      for (int x = 0; x < CX; x++) {
        unsigned char b = c->blocks[Idx(x, y, z)];
        if (b == AIR) continue;
        for (int f = 0; f < 6; f++) {
          int nx = x + FACE_N[f][0], ny = y + FACE_N[f][1], nz = z + FACE_N[f][2];
          if (IsSolid(at(nx, ny, nz))) continue;      // face culling
          float u0 = ((TileFor(b, f) * TILE) + 0.02f) / (TILE * TILES);
          float u1 = (((TileFor(b, f) + 1) * TILE) - 0.02f) / (TILE * TILES);
          float v0 = 0.02f / TILE, v1 = (TILE - 0.02f) / TILE;
          unsigned short base = (unsigned short)(verts.size() / 3);
          for (int k = 0; k < 4; k++) {
            // ambient occlusion: sample the two edge neighbors + corner in the layer above the face
            int su = FACE_V[f][k][FACE_UA[f]] > 0.5f ? 1 : -1;
            int sw = FACE_V[f][k][FACE_WA[f]] > 0.5f ? 1 : -1;
            int s[3] = {0, 0, 0};
            s[FACE_UA[f]] = su;
            int t[3] = {0, 0, 0};
            t[FACE_WA[f]] = sw;
            int vx = x + FACE_N[f][0], vy = y + FACE_N[f][1], vz = z + FACE_N[f][2];
            bool o1 = IsSolid(at(vx + s[0], vy + s[1], vz + s[2]));
            bool o2 = IsSolid(at(vx + t[0], vy + t[1], vz + t[2]));
            bool oc = IsSolid(at(vx + s[0] + t[0], vy + s[1] + t[1], vz + s[2] + t[2]));
            int occl = (o1 && o2) ? 3 : ((int)o1 + (int)o2 + (int)oc);
            float shade = FACE_SHADE[f] * (1.0f - 0.22f * occl);

            verts.push_back(x + FACE_V[f][k][0]);
            verts.push_back(y + FACE_V[f][k][1]);
            verts.push_back(z + FACE_V[f][k][2]);
            uvs.push_back(u0 + FACE_UV[f][k][0] * (u1 - u0));
            uvs.push_back(v0 + FACE_UV[f][k][1] * (v1 - v0));
            norms.push_back((float)FACE_N[f][0]);
            norms.push_back((float)FACE_N[f][1]);
            norms.push_back((float)FACE_N[f][2]);
            unsigned char cc = (unsigned char)(255 * shade);
            cols.push_back(cc);
            cols.push_back(cc);
            cols.push_back(cc);
            cols.push_back(255);
          }
          idx.push_back(base); idx.push_back(base + 1); idx.push_back(base + 2);
          idx.push_back(base); idx.push_back(base + 2); idx.push_back(base + 3);
        }
      }

  Mesh m = {0};
  m.vertexCount = (int)(verts.size() / 3);
  m.triangleCount = (int)(idx.size() / 3);
  if (m.vertexCount == 0) return m;
  // raylib owns these buffers; UnloadMesh() frees them later
  m.vertices = (float *)MemAlloc(verts.size() * sizeof(float));
  memcpy(m.vertices, verts.data(), verts.size() * sizeof(float));
  m.texcoords = (float *)MemAlloc(uvs.size() * sizeof(float));
  memcpy(m.texcoords, uvs.data(), uvs.size() * sizeof(float));
  m.normals = (float *)MemAlloc(norms.size() * sizeof(float));
  memcpy(m.normals, norms.data(), norms.size() * sizeof(float));
  m.colors = (unsigned char *)MemAlloc(cols.size());
  memcpy(m.colors, cols.data(), cols.size());
  m.indices = (unsigned short *)MemAlloc(idx.size() * sizeof(unsigned short));
  memcpy(m.indices, idx.data(), idx.size() * sizeof(unsigned short));
  UploadMesh(&m, false);
  return m;
}

// ---------- world generation ----------
static void ApplyEdits(Chunk *c) {
  int x0 = c->cx * CX, z0 = c->cz * CZ;
  for (const Edit &e : edits) {
    if (e.x < x0 || e.x >= x0 + CX || e.z < z0 || e.z >= z0 + CZ) continue;
    if (e.y < 0 || e.y >= CY) continue;
    c->blocks[Idx(e.x - x0, e.y, e.z - z0)] = e.b;
  }
}

// fills blocks only; meshing happens separately
static void GenChunkBlocks(Chunk *c) {
  c->blocks.assign(CHUNK_VOL, AIR);
  int x0 = c->cx * CX, z0 = c->cz * CZ;
  int height[CX][CZ];

  for (int z = 0; z < CZ; z++)
    for (int x = 0; x < CX; x++) {
      int wx = x0 + x, wz = z0 + z;
      int h = (int)floorf(TerrainHeight(wx, wz));
      if (h < 1) h = 1;
      if (h > CY - 2) h = CY - 2;
      height[x][z] = h;
      bool beach = h <= SEA_LEVEL + 1;
      for (int y = 0; y <= h; y++) {
        unsigned char b = (y == h) ? (beach ? SAND : GRASS)
                                   : (y >= h - 4 ? DIRT : STONE);
        c->blocks[Idx(x, y, z)] = b;
      }
      // caves: carve air pockets underground (world coords -> continuous across chunks)
      for (int y = 1; y < h; y++)
        if (CaveNoise(wx * 0.09f, y * 0.13f, wz * 0.09f) > 0.74f)
          c->blocks[Idx(x, y, z)] = AIR;
      c->blocks[Idx(x, 0, z)] = STONE;   // bedrock floor
    }

  // trees: deterministic per chunk, kept 2 blocks inside the border so canopies stay in-chunk
  for (int t = 0; t < 4; t++) {
    float r = Hash2(c->cx * 7919 + t, c->cz * 104729 + t * 31);
    if (r > 0.35f) continue;
    int x = 2 + (int)(Hash2(c->cx * 31 + t * 7, c->cz * 17 + t) * (CX - 4));
    int z = 2 + (int)(Hash2(c->cx * 53 + t, c->cz * 71 + t * 13) * (CZ - 4));
    int h = height[x][z];
    if (h <= SEA_LEVEL + 1 || c->blocks[Idx(x, h, z)] != GRASS) continue;
    int trunk = 4 + (int)(Hash2(c->cx + t, c->cz - t) * 3);
    for (int y = h + 1; y <= h + trunk && y < CY; y++) c->blocks[Idx(x, y, z)] = WOOD;
    int top = h + trunk;
    for (int dy = -2; dy <= 1; dy++)
      for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++) {
          if (abs(dx) == 2 && abs(dz) == 2) continue;   // trim corners
          int yy = top + dy, xx = x + dx, zz = z + dz;
          if (yy < 0 || yy >= CY || xx < 0 || xx >= CX || zz < 0 || zz >= CZ) continue;
          if (c->blocks[Idx(xx, yy, zz)] == AIR) c->blocks[Idx(xx, yy, zz)] = LEAVES;
        }
  }

  ApplyEdits(c);
}

static Chunk *LoadChunk(int cx, int cz) {
  Chunk *c = new Chunk();
  c->cx = cx;
  c->cz = cz;
  GenChunkBlocks(c);          // blocks only; meshing is a separate budgeted phase
  chunks[ChunkKey(cx, cz)] = c;
  meshQueue.push_back({c, 0});
  return c;
}

// chunks whose mesh needs rebuilding; declared early so unload can scrub it
std::vector<Chunk *> dirty;

static void UnloadChunk(Chunk *c) {
  if (c->hasMesh) UnloadMesh(c->mesh);
  // removing a chunk from the queues keeps flush/mesh phases from touching freed memory
  dirty.erase(std::remove(dirty.begin(), dirty.end(), c), dirty.end());
  meshQueue.erase(std::remove_if(meshQueue.begin(), meshQueue.end(),
                                 [&](const PendingMesh &p) { return p.c == c; }),
                  meshQueue.end());
  delete c;
}

static void RebuildChunk(Chunk *c) {
  if (c->hasMesh) {
    UnloadMesh(c->mesh);
    c->mesh = Mesh{0};
    c->hasMesh = false;
  }
  c->mesh = BuildMesh(c);
  c->hasMesh = c->mesh.vertexCount > 0;
}

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

// ---------- budgeted chunk streaming ----------
// Two tiny queues, because a chunk used to be gen+mesh+4-neighbor-remesh in one call,
// which stalled the frame. Now blocks are generated first and meshed only once all four
// neighbors exist, so border faces have real data and nothing needs remeshing.
static void RefreshChunks(int pcx, int pcz) {
  for (auto it = chunks.begin(); it != chunks.end();) {
    Chunk *c = it->second;
    if (abs(c->cx - pcx) > cfg.renderDist + 1 || abs(c->cz - pcz) > cfg.renderDist + 1) {
      UnloadChunk(c);
      it = chunks.erase(it);
    } else {
      ++it;
    }
  }
  loadQueue.clear();
  for (int dz = -cfg.renderDist; dz <= cfg.renderDist; dz++)
    for (int dx = -cfg.renderDist; dx <= cfg.renderDist; dx++) {
      int cx = pcx + dx, cz = pcz + dz;
      if (chunks.find(ChunkKey(cx, cz)) == chunks.end())
        loadQueue.push_back({cx, cz});
    }
  std::sort(loadQueue.begin(), loadQueue.end(), [&](const std::pair<int, int> &a,
                                                    const std::pair<int, int> &b) {
    int da = (a.first - pcx) * (a.first - pcx) + (a.second - pcz) * (a.second - pcz);
    int db = (b.first - pcx) * (b.first - pcx) + (b.second - pcz) * (b.second - pcz);
    return da > db;   // descending: pop_back() gives the nearest chunk
  });
}

static bool HasAllNeighbors(Chunk *c) {
  return chunks.count(ChunkKey(c->cx - 1, c->cz)) &&
         chunks.count(ChunkKey(c->cx + 1, c->cz)) &&
         chunks.count(ChunkKey(c->cx, c->cz - 1)) &&
         chunks.count(ChunkKey(c->cx, c->cz + 1));
}

static int meshedThisFrame = 0;   // for -bench: proves chunks are actually being built

static void ProcessQueues() {
  for (int i = 0; i < GEN_BUDGET && !loadQueue.empty(); i++) {
    auto [cx, cz] = loadQueue.back();
    loadQueue.pop_back();
    if (chunks.find(ChunkKey(cx, cz)) == chunks.end()) LoadChunk(cx, cz);
  }
  int meshed = 0;
  for (auto it = meshQueue.begin(); it != meshQueue.end() && meshed < MESH_BUDGET;) {
    if (!HasAllNeighbors(it->c)) { ++it; continue; }   // wait for neighbors
    RebuildChunk(it->c);   // also picks up any edits recorded while it was pending
    it = meshQueue.erase(it);
    meshed++;
  }
  meshedThisFrame = meshed;
}

// ---------- audio (procedural, no asset files) ----------
// ponytail: noise-burst SFX synthesised at load; swap in LoadSound() of real .wav assets
// if the blips get tiring.
static Sound MakeNoiseSound(float seconds, float toneHz, float decay) {
  int rate = 22050;
  int frames = (int)(rate * seconds);
  if (frames < 1) frames = 1;
  short *data = (short *)MemAlloc(frames * sizeof(short));
  for (int i = 0; i < frames; i++) {
    float t = (float)i / rate;
    float env = expf(-decay * t);                                 // percussive decay
    float n = (float)GetRandomValue(-32767, 32767) / 32767.0f;    // noise body
    float s = n * env;
    if (toneHz > 0) s = s * 0.55f + sinf(2.0f * PI * toneHz * t) * env * 0.45f;
    data[i] = (short)(Clamp(s, -1.0f, 1.0f) * 32767.0f);
  }
  Wave w = {0};
  w.frameCount = (unsigned)frames;
  w.sampleRate = (unsigned)rate;
  w.sampleSize = 16;
  w.channels = 1;
  w.data = data;
  Sound snd = LoadSoundFromWave(w);   // copies the samples
  UnloadWave(w);
  return snd;
}

// a bank of aliases so repeated sounds overlap instead of cutting each other off.
// raylib aliases share the sample data, so the base is freed with UnloadSound (frees the
// data) and the aliases with UnloadSoundAlias (frees only their own buffer struct).
struct Sfx {
  Sound base{};
  Sound v[4];
  int next = 0;
  void Play(float vol, float pitch) {
    Sound &s = v[next];
    next = (next + 1) % 4;
    SetSoundVolume(s, vol);
    SetSoundPitch(s, pitch);
    ::PlaySound(s);
  }
  void Unload() {
    for (Sound &s : v) UnloadSoundAlias(s);
    UnloadSound(base);
  }
};

static Sfx MakeSfx(float seconds, float toneHz, float decay) {
  Sfx s;
  s.base = MakeNoiseSound(seconds, toneHz, decay);
  if (s.base.stream.buffer == NULL) return s;   // no device / conversion failed
  for (int i = 0; i < 4; i++) s.v[i] = LoadSoundAlias(s.base);
  return s;
}

static bool audioReady = false;
static Sfx sfxStep, sfxBreak, sfxPlace, sfxJump;
static float stepTimer = 0.0f;

static void PlayVaried(Sfx &s, float vol, float pitchJitter) {
  if (!audioReady) return;
  float j = 1.0f + (float)GetRandomValue(-100, 100) / 100.0f * pitchJitter;
  s.Play(vol, j);
}

// ---------- gamepad helpers ----------
// ponytail: gamepad 0 only, no rebinding. Axis deadzones baked in.
static const int GP = 0;             // gamepad index
static const float GP_DEAD = 0.15f;  // stick deadzone (0..1)
static const float GP_LOOK = 40.0f;  // right-stick look speed multiplier — raise to look faster, lower to slow down

static inline float GPAxis(int axis) {
  float v = GetGamepadAxisMovement(GP, axis);
  return fabsf(v) > GP_DEAD ? v : 0.0f;
}
static inline bool GPBtn(int btn)        { return IsGamepadButtonDown(GP, btn); }
static inline bool GPBtnPressed(int btn) { return IsGamepadButtonPressed(GP, btn); }

// ---------- player ----------
struct Player {
  Vector3 pos{8, 40, 8};
  Vector3 vel{0, 0, 0};
  float yaw = 0, pitch = 0;
  bool fly = false, onGround = false;
  float health = 20.0f, hunger = 20.0f;   // Minecraft units: 10 hearts, 10 haunches
};
Player player;

const float PW = 0.3f, PH = 1.8f, EYE = 1.62f, GRAVITY = 25.0f, JUMP = 8.0f;
const float WALK = 5.0f, FLY_SPEED = 14.0f, SENS = 0.003f;

// ---------- survival stats (M3 step 1) ----------
const float MAX_STAT = 20.0f;              // both bars use MC's 0..20 scale
const float HUNGER_IDLE_RATE = 0.008f;     // drain per second standing still
const float HUNGER_WALK_RATE = 0.02f;      // extra drain per world-unit walked
const float REGEN_HUNGER_COST = 0.8f;      // hunger spent per 1 HP (MC: ~0.8 per shank)
const float REGEN_MIN_HUNGER = 18.0f;      // below this, no regen (MC needs food >= 18)
const float REGEN_RATE = 0.5f;             // HP per second while fed and not starving

// One second of survival metabolism. Pure function (no globals) so -selftest can pin it.
// `hurtCooldown` locks regen for a moment after a hit, or the player would out-heal chip damage.
static void TickSurvival(float dt, float moved, float hurtCooldown, float &health, float &hunger) {
  hunger = Clamp(hunger - dt * (HUNGER_IDLE_RATE + moved * HUNGER_WALK_RATE), 0.0f, MAX_STAT);
  if (hunger <= 0.0f)                      // starvation: health is the last reserve
    health = Clamp(health - dt * 0.2f, 0.0f, MAX_STAT);
  else if (hunger >= REGEN_MIN_HUNGER && hurtCooldown <= 0.0f && health < MAX_STAT) {
    float gain = fminf(dt * REGEN_RATE, MAX_STAT - health);
    health += gain;
    hunger = Clamp(hunger - gain * REGEN_HUNGER_COST, 0.0f, MAX_STAT);
  }
}

// ---------- damage sources (M3 step 2) ----------
const float FALL_SAFE = 3.0f;        // blocks of free fall before damage starts (MC's 3)
const float INVULN_TIME = 0.5f;      // i-frames after any hit, so one fall can't hit twice
const float REGEN_HOLD_TIME = 1.5f;  // no regen for this long after a hit

float fallPeakY = 0.0f;     // highest Y reached since leaving the ground
float invulnTimer = 0.0f;   // > 0 = invulnerable
float regenHold = 0.0f;     // > 0 = regen paused
float hurtFlash = 0.0f;     // 1 -> 0, drives the red screen vignette

// Pure: how much damage a fall of `dist` blocks does. The 3-block grace is the whole rule.
static float FallDamage(float dist) {
  return dist > FALL_SAFE ? dist - FALL_SAFE : 0.0f;
}

// Pure: distance from the highest point reached to where the player ended up. Measuring the
// drop instead of summing per-frame deltas keeps it exact — summing over-counts by a frame of
// travel and invents distance while sliding down a wall, where Y doesn't move but velocity does.
static float FallDistance(float peakY, float landY) {
  return peakY > landY ? peakY - landY : 0.0f;
}

// The one place health is reduced by a hit. Every damage source goes through here, so the
// invulnerability window and the creative exemption can never be forgotten by a new caller.
// Returns true if it landed.
static bool Hurt(float amount) {
  if (amount <= 0.0f || invulnTimer > 0.0f || cfg.creative) return false;
  player.health = Clamp(player.health - amount, 0.0f, MAX_STAT);
  invulnTimer = INVULN_TIME;
  regenHold = REGEN_HOLD_TIME;
  hurtFlash = 1.0f;
  PlayVaried(sfxJump, 0.6f, 0.25f);   // a low thud; no damage sample to load
  return true;
}

static bool SolidAt(int x, int y, int z) { return GetBlockWorld(x, y, z) != AIR; }

static bool CollidesAt(Vector3 p) {
  const float e = 0.001f;
  int x0 = (int)floorf(p.x - PW), x1 = (int)floorf(p.x + PW - e);
  int y0 = (int)floorf(p.y), y1 = (int)floorf(p.y + PH - e);
  int z0 = (int)floorf(p.z - PW), z1 = (int)floorf(p.z + PW - e);
  for (int x = x0; x <= x1; x++)
    for (int y = y0; y <= y1; y++)
      for (int z = z0; z <= z1; z++)
        if (SolidAt(x, y, z)) return true;
  return false;
}

static bool AABBOverlapsBlock(Vector3 p, int bx, int by, int bz) {
  return (p.x - PW < bx + 1 && p.x + PW > bx && p.y < by + 1 &&
          p.y + PH > by && p.z - PW < bz + 1 && p.z + PW > bz);
}

static Vector3 ForwardVec(float yaw, float pitch) {
  return Vector3{cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
}
// screen-right = cross(forward, up); for forward=(sin,0,cos) that is (-cos,0,sin)
static Vector3 RightVec(float yaw) {
  return Vector3{-cosf(yaw), 0, sinf(yaw)};
}

// returns horizontal world-units actually moved (used for hunger drain)
static float UpdatePlayer(float dt) {
  Vector2 md = GetMouseDelta();
  // ponytail: right stick look; GP_LOOK multiplier defined near the gamepad helpers block (~L651)
  float gpLookScale = cfg.sensitivity * GP_LOOK;
  player.yaw   -= md.x * cfg.sensitivity + GPAxis(GAMEPAD_AXIS_RIGHT_X) * gpLookScale;
  player.pitch -= md.y * cfg.sensitivity + GPAxis(GAMEPAD_AXIS_RIGHT_Y) * gpLookScale;
  player.pitch = fmaxf(-1.5f, fminf(1.5f, player.pitch));

  Vector3 fwd = ForwardVec(player.yaw, player.pitch);
  Vector3 flat = Vector3Normalize(Vector3{fwd.x, 0, fwd.z});
  Vector3 right = RightVec(player.yaw);

  Vector3 wish{0, 0, 0};
  if (IsKeyDown(KEY_W) || GPAxis(GAMEPAD_AXIS_LEFT_Y) < -GP_DEAD) wish = Vector3Add(wish, Vector3Scale(flat, IsKeyDown(KEY_W) ? 1.0f : -GPAxis(GAMEPAD_AXIS_LEFT_Y)));
  if (IsKeyDown(KEY_S) || GPAxis(GAMEPAD_AXIS_LEFT_Y) >  GP_DEAD) wish = Vector3Subtract(wish, Vector3Scale(flat, IsKeyDown(KEY_S) ? 1.0f :  GPAxis(GAMEPAD_AXIS_LEFT_Y)));
  if (IsKeyDown(KEY_D) || GPAxis(GAMEPAD_AXIS_LEFT_X) >  GP_DEAD) wish = Vector3Add(wish, Vector3Scale(right, IsKeyDown(KEY_D) ? 1.0f :  GPAxis(GAMEPAD_AXIS_LEFT_X)));
  if (IsKeyDown(KEY_A) || GPAxis(GAMEPAD_AXIS_LEFT_X) < -GP_DEAD) wish = Vector3Subtract(wish, Vector3Scale(right, IsKeyDown(KEY_A) ? 1.0f : -GPAxis(GAMEPAD_AXIS_LEFT_X)));

  if (IsKeyPressed(KEY_F) || GPBtnPressed(GAMEPAD_BUTTON_LEFT_THUMB)) { player.fly = !player.fly; fallPeakY = player.pos.y; }

  if (player.fly) {
    if (IsKeyDown(KEY_SPACE)        || GPBtn(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))  wish.y += 1;
    if (IsKeyDown(KEY_LEFT_CONTROL) || GPBtn(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) wish.y -= 1;
    if (Vector3Length(wish) > 0) wish = Vector3Normalize(wish);
    player.vel = Vector3Scale(wish, FLY_SPEED);
  } else {
    if (Vector3Length(wish) > 0) wish = Vector3Normalize(wish);
    player.vel.x = wish.x * WALK;
    player.vel.z = wish.z * WALK;
    player.vel.y -= GRAVITY * dt;
    bool wasGrounded = player.onGround;
    player.onGround = false;
    if ((IsKeyDown(KEY_SPACE) || GPBtn(GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) && wasGrounded) { player.vel.y = JUMP; PlayVaried(sfxJump, 0.5f, 0.12f); }

    // footsteps: a short tick every N world-units of grounded horizontal travel
    float speed = sqrtf(player.vel.x * player.vel.x + player.vel.z * player.vel.z);
    if (wasGrounded && speed > 0.3f) {
      stepTimer += speed * dt;
      if (stepTimer > 2.4f) { stepTimer = 0.0f; PlayVaried(sfxStep, 0.35f, 0.15f); }
    } else stepTimer = 0.0f;
  }

  // per-axis move + resolve (X, Z, then Y so grounded is detected last)
  Vector3 startPos = player.pos;
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
  float moved = sqrtf((player.pos.x - startPos.x) * (player.pos.x - startPos.x) +
                      (player.pos.z - startPos.z) * (player.pos.z - startPos.z));
  // fall tracking, after the move so the peak is measured on the resolved position:
  //   airborne -> remember the highest point reached
  //   grounded -> charge for the drop from that apex, then re-pin the peak to the new floor
  // Standing still re-pins to the same Y each frame and charges nothing (FallDistance is 0,
  // FallDamage is 0, Hurt(0) returns before touching a timer), so no transition flag is needed.
  if (!player.fly) {
    if (player.onGround) {
      Hurt(FallDamage(FallDistance(fallPeakY, player.pos.y)));
      fallPeakY = player.pos.y;
    } else {
      fallPeakY = fmaxf(fallPeakY, player.pos.y);
    }
  }
  if (player.pos.y < -20) {
    player.pos = Vector3{8, 60, 8};
    player.vel = Vector3{0, 0, 0};
    fallPeakY = player.pos.y;
  }
  return moved;
}

// ---------- voxel raycast (Amanatides & Woo) ----------
struct RayHit {
  bool hit = false;
  int x = 0, y = 0, z = 0;      // hit voxel
  int px = 0, py = 0, pz = 0;   // voxel before it (placement spot)
};

static RayHit RaycastVoxel(Vector3 o, Vector3 dir, float maxDist) {
  RayHit r;
  int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z);
  int sx = dir.x > 0 ? 1 : -1, sy = dir.y > 0 ? 1 : -1, sz = dir.z > 0 ? 1 : -1;
  float tdx = fabsf(dir.x) < 1e-8f ? 1e30f : fabsf(1 / dir.x);
  float tdy = fabsf(dir.y) < 1e-8f ? 1e30f : fabsf(1 / dir.y);
  float tdz = fabsf(dir.z) < 1e-8f ? 1e30f : fabsf(1 / dir.z);
  float tmx = (sx > 0 ? (x + 1 - o.x) : (o.x - x)) * tdx;
  float tmy = (sy > 0 ? (y + 1 - o.y) : (o.y - y)) * tdy;
  float tmz = (sz > 0 ? (z + 1 - o.z) : (o.z - z)) * tdz;
  int px = x, py = y, pz = z;
  float t = 0;
  while (t <= maxDist) {
    if (SolidAt(x, y, z)) {
      r.hit = true;
      r.x = x; r.y = y; r.z = z;
      r.px = px; r.py = py; r.pz = pz;
      return r;
    }
    px = x; py = y; pz = z;
    if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; }
    else if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; }
    else { z += sz; t = tmz; tmz += tdz; }
  }
  return r;
}

// ---------- save / load ----------
const char *SAVE_FILE = "world.dat";
const uint32_t MAGIC = 0x31434357;   // "WCC1"

static void SaveEdits() {
  std::vector<unsigned char> buf(sizeof(uint32_t) + sizeof(uint32_t) +
                                 edits.size() * sizeof(Edit));
  uint32_t count = (uint32_t)edits.size();
  memcpy(buf.data(), &MAGIC, sizeof(MAGIC));
  memcpy(buf.data() + 4, &count, sizeof(count));
  if (count) memcpy(buf.data() + 8, edits.data(), count * sizeof(Edit));
  SaveFileData(SAVE_FILE, buf.data(), (int)buf.size());
}

static void LoadEdits() {
  int size = 0;
  unsigned char *data = LoadFileData(SAVE_FILE, &size);
  if (!data || size < 8) { if (data) UnloadFileData(data); return; }
  uint32_t magic = 0, count = 0;
  memcpy(&magic, data, 4);
  memcpy(&count, data + 4, 4);
  if (magic == MAGIC && (int)(8 + count * sizeof(Edit)) <= size) {
    edits.resize(count);
    if (count) memcpy(edits.data(), data + 8, count * sizeof(Edit));
  }
  UnloadFileData(data);
}

// ---------- break particles ----------
struct Particle {
  Vector3 pos, vel;
  float life;
  Color col;
};
std::vector<Particle> particles;

static void SpawnBreakParticles(unsigned char b, int x, int y, int z) {
  if (b == AIR || b >= BLOCK_COUNT) return;   // guard the atlas index (items section is later)
  Color c = ATLAS_BASE[TileFor(b, 2)];   // top-face tile is a good sample colour
  for (int i = 0; i < 8; i++) {
    Particle p;
    p.pos = Vector3{x + 0.5f, y + 0.5f, z + 0.5f};
    p.vel = Vector3{(float)GetRandomValue(-120, 120) / 100.0f,
                    (float)GetRandomValue(40, 140) / 100.0f,
                    (float)GetRandomValue(-120, 120) / 100.0f};
    p.life = 0.45f + (float)GetRandomValue(0, 35) / 100.0f;
    unsigned char j = (unsigned char)GetRandomValue(0, 45);
    p.col = Color{(unsigned char)(c.r > j ? c.r - j : 0),
                  (unsigned char)(c.g > j ? c.g - j : 0),
                  (unsigned char)(c.b > j ? c.b - j : 0), 255};
    particles.push_back(p);
  }
}

static void UpdateParticles(float dt) {
  for (Particle &p : particles) {
    p.vel.y -= GRAVITY * 0.5f * dt;   // lighter than the player so it hangs a moment
    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += p.vel.z * dt;
    p.life -= dt;
  }
  // ponytail: particles die on a timer instead of colliding; collide only if it looks wrong
  particles.erase(std::remove_if(particles.begin(), particles.end(),
                                 [](const Particle &p) { return p.life <= 0.0f; }),
                  particles.end());
}

static void DrawParticles() {
  for (const Particle &p : particles) DrawCube(p.pos, 0.09f, 0.09f, 0.09f, p.col);
}

// ---------- items & inventory ----------
// Block items reuse the Block ids (1..7) so placing a stack is a plain cast; non-block items
// get ids from 16 up. YAGNI: no separate Item enum layer until a block is NOT an item.
enum Item : unsigned char {
  ITEM_NONE = AIR,
  ITEM_STICK = 16,
  ITEM_PICKAXE,
  ITEM_AXE,
  ITEM_SHOVEL,
};

struct ItemStack {
  unsigned char item = ITEM_NONE;
  unsigned char count = 0;
  unsigned short damage = 0;   // reserved for tool durability (M2 pass 2)
};

const int INV_COLS = 9;
const int INV_MAIN_ROWS = 3;
const int HOTBAR_SLOTS = 9;
const int INV_SLOTS = HOTBAR_SLOTS + INV_COLS * INV_MAIN_ROWS;   // 36
const int MAX_STACK = 64;

static ItemStack inv[INV_SLOTS];
static ItemStack cursorStack;   // stack held by the mouse cursor in the inventory UI
static int hotbarSel = 0;
static bool invOpen = false;

// ---------- tool classes ----------
// which tool is "right" for a block; TOOL_NONE = bare hands are fine
enum ToolClass { TOOL_NONE, TOOL_PICK, TOOL_AXE, TOOL_SHOVEL };

static int ToolClassOf(unsigned char item) {
  switch (item) {
    case ITEM_PICKAXE: return TOOL_PICK;
    case ITEM_AXE: return TOOL_AXE;
    case ITEM_SHOVEL: return TOOL_SHOVEL;
  }
  return TOOL_NONE;
}

const float TOOL_SPEED = 4.0f;   // matching tool is 4x faster

// ponytail: one durability value for every tool (no per-tier table). Split into a
// per-material table when tiers exist.
const int TOOL_MAX_DAMAGE = 64, TOOL_WEAR_PER_BLOCK = 1;

static unsigned short ToolMaxDamage(unsigned char it) {
  return ToolClassOf(it) != TOOL_NONE ? (unsigned short)TOOL_MAX_DAMAGE : 0;
}

// Returns true when the tool just broke. Mutates the stack in place so it is testable
// without touching the inventory.
static bool WearTool(ItemStack &s) {
  if (ToolClassOf(s.item) == TOOL_NONE) return false;
  s.damage = (unsigned short)(s.damage + TOOL_WEAR_PER_BLOCK);
  if (s.damage < ToolMaxDamage(s.item)) return false;
  s = ItemStack{};   // broke
  return true;
}

// wear whatever is in the selected slot; returns true if it shattered
static bool WearHeldTool() {
  ItemStack &held = inv[hotbarSel];
  return ToolClassOf(held.item) != TOOL_NONE && WearTool(held);
}

static bool IsBlockItem(unsigned char it) { return it != ITEM_NONE && it < BLOCK_COUNT; }
static bool IsToolItem(unsigned char it) { return ToolClassOf(it) != TOOL_NONE; }

// atlas tile used as the 2D icon for an item (-1 = no icon)
static int ItemTile(unsigned char it) {
  switch (it) {
    case GRASS: return 0;    // grass_top reads better as a thumb than grass_side
    case DIRT: return 2;
    case STONE: return 3;
    case SAND: return 4;
    case WOOD: return 5;     // bark side
    case LEAVES: return 7;
    case PLANK: return 8;
    case ITEM_PICKAXE: return TILE_PICKAXE;
    case ITEM_AXE: return TILE_AXE;
    case ITEM_SHOVEL: return TILE_SHOVEL;
  }
  return -1;
}

static const char *ItemName(unsigned char it) {
  switch (it) {
    case GRASS: return "Grass";
    case DIRT: return "Dirt";
    case STONE: return "Stone";
    case SAND: return "Sand";
    case WOOD: return "Wood";
    case LEAVES: return "Leaves";
    case PLANK: return "Planks";
    case ITEM_STICK: return "Stick";
    case ITEM_PICKAXE: return "Pickaxe";
    case ITEM_AXE: return "Axe";
    case ITEM_SHOVEL: return "Shovel";
  }
  return "?";
}

// fill the inventory with a full stack of every block, the old creative hotbar behaviour
static void GiveStarterItems() {
  for (int i = 0; i < INV_SLOTS; i++) inv[i] = ItemStack{};
  const unsigned char start[] = {GRASS, DIRT, STONE, SAND, WOOD, LEAVES, PLANK};
  for (int i = 0; i < (int)(sizeof(start) / sizeof(start[0])); i++) {
    inv[i].item = start[i];
    inv[i].count = MAX_STACK;
  }
}

// returns how many of `count` did NOT fit
static int AddItem(unsigned char item, int count) {
  if (item == ITEM_NONE || count <= 0) return count;
  // Tools carry wear, so they never merge: one fresh stack per item.
  if (IsToolItem(item)) {
    for (int i = 0; i < INV_SLOTS && count > 0; i++) {
      ItemStack &s = inv[i];
      if (s.count != 0) continue;
      s = ItemStack{};
      s.item = item;
      s.count = 1;
      count--;
    }
    return count;
  }
  for (int i = 0; i < INV_SLOTS && count > 0; i++) {   // top up matching stacks first
    ItemStack &s = inv[i];
    if (s.item == item && s.count < MAX_STACK) {
      int add = std::min(count, (int)(MAX_STACK - s.count));
      s.count = (unsigned char)(s.count + add);
      count -= add;
    }
  }
  for (int i = 0; i < INV_SLOTS && count > 0; i++) {   // then empty slots
    ItemStack &s = inv[i];
    if (s.count == 0) {
      s = ItemStack{};
      s.item = item;
      int add = std::min(count, (int)MAX_STACK);
      s.count = (unsigned char)add;
      count -= add;
    }
  }
  return count;
}

static int CountItem(unsigned char item) {
  int n = 0;
  for (const ItemStack &s : inv)
    if (s.item == item) n += s.count;
  return n;
}

// remove up to `count` of `item`, returns how many were removed
static int TakeItem(unsigned char item, int count) {
  int taken = 0;
  for (int i = 0; i < INV_SLOTS && taken < count; i++) {
    ItemStack &s = inv[i];
    if (s.item != item) continue;
    int take = std::min(count - taken, (int)s.count);
    s.count = (unsigned char)(s.count - take);
    taken += take;
    if (s.count == 0) s = ItemStack{};
  }
  return taken;
}

// ---------- crafting ----------
// Recipes are stored as a 3x3 grid (0 = must be empty). Matching normalizes the grid's
// occupied bounding box, so a 2x2 recipe works in any corner of the 3x3 (Minecraft's rule).
const int CRAFT_W = 3, CRAFT_H = 3, CRAFT_SLOTS = CRAFT_W * CRAFT_H;

struct Recipe {
  const char *name;
  unsigned char in[CRAFT_SLOTS];   // 0 = nothing
  unsigned char out;               // item id produced
  unsigned char outCount;
};

static const Recipe RECIPES[] = {
    {"planks", {WOOD, 0, 0, 0, 0, 0, 0, 0, 0}, PLANK, 4},
    {"sticks", {PLANK, 0, 0, PLANK, 0, 0, 0, 0, 0}, ITEM_STICK, 4},
    {"pickaxe", {PLANK, PLANK, PLANK, 0, ITEM_STICK, 0, 0, ITEM_STICK, 0}, ITEM_PICKAXE, 1},
    {"axe", {PLANK, PLANK, 0, PLANK, ITEM_STICK, 0, 0, ITEM_STICK, 0}, ITEM_AXE, 1},
    {"shovel", {PLANK, 0, 0, ITEM_STICK, 0, 0, ITEM_STICK, 0, 0}, ITEM_SHOVEL, 1},
};
const int RECIPE_COUNT = (int)(sizeof(RECIPES) / sizeof(RECIPES[0]));

// Copy the pattern's occupied bounding box out, top-left aligned, into a w x h array.
// Returns false if empty. `st` matches CRAFT_SLOTS.
static bool BoundingCells(const unsigned char st[CRAFT_SLOTS], unsigned char out[CRAFT_SLOTS],
                        int &w, int &h) {
  int minx = CRAFT_W, maxx = -1, miny = CRAFT_H, maxy = -1;
  for (int y = 0; y < CRAFT_H; y++)
    for (int x = 0; x < CRAFT_W; x++)
      if (st[y * CRAFT_W + x]) {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
  if (maxx < 0) return false;
  for (int i = 0; i < CRAFT_SLOTS; i++) out[i] = 0;
  w = maxx - minx + 1;
  h = maxy - miny + 1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) out[y * CRAFT_W + x] = st[(y + miny) * CRAFT_W + (x + minx)];
  return true;
}

// index of the recipe the 3x3 grid matches, or -1
static int MatchRecipe(const unsigned char grid[CRAFT_SLOTS]) {
  unsigned char g[CRAFT_SLOTS];
  int gw = 0, gh = 0;
  if (!BoundingCells(grid, g, gw, gh)) return -1;
  for (int r = 0; r < RECIPE_COUNT; r++) {
    unsigned char p[CRAFT_SLOTS];
    int pw = 0, ph = 0;
    if (!BoundingCells(RECIPES[r].in, p, pw, ph)) continue;
    if (pw != gw || ph != gh) continue;
    bool same = true;
    for (int y = 0; y < gh && same; y++)
      for (int x = 0; x < gw && same; x++)
        if (p[y * CRAFT_W + x] != g[y * CRAFT_W + x]) same = false;
    if (same) return r;
  }
  return -1;
}

// Consume one of every non-empty grid cell. Caller has already checked the result fits.
static void ConsumeGrid(ItemStack grid[CRAFT_SLOTS]) {
  for (int i = 0; i < CRAFT_SLOTS; i++) {
    if (grid[i].count == 0) continue;
    if (--grid[i].count == 0) grid[i] = ItemStack{};
  }
}

// ---------- block hardness ----------
// seconds to break bare-handed (a matching tool divides this, see BreakTime)
static float Hardness(unsigned char b) {
  switch (b) {
    case GRASS: case DIRT: case SAND: return 0.55f;
    case LEAVES: return 0.25f;
    case WOOD: case PLANK: return 1.6f;
    case STONE: return 3.0f;
  }
  return 1.0f;
}

// which tool is "right" for a block; TOOL_NONE = bare hands are fine
static int ClassOfBlock(unsigned char b) {
  switch (b) {
    case STONE: return TOOL_PICK;
    case WOOD: case PLANK: return TOOL_AXE;
    case GRASS: case DIRT: case SAND: return TOOL_SHOVEL;
  }
  return TOOL_NONE;
}

// Seconds needed to break `b` while holding `held`, or -1 if it cannot be harvested at all.
// The only hard gate is stone: M2's "stone needs the pickaxe". Everything else is breakable
// bare-handed, just slowly.
static float BreakTime(unsigned char b, unsigned char held) {
  bool matching = ClassOfBlock(b) != TOOL_NONE && ClassOfBlock(b) == ToolClassOf(held);
  if (b == STONE && !matching) return -1.0f;
  return matching ? Hardness(b) / TOOL_SPEED : Hardness(b);
}

// what a block yields when broken (same block, simplifed from MC's cobble/grass rules)
static unsigned char BlockDrop(unsigned char b) {
  switch (b) {
    case GRASS: return DIRT;   // MC drops dirt for grass
    case LEAVES: return ITEM_NONE;
  }
  return b;
}

// ---------- item drops ----------
struct Drop {
  Vector3 pos, vel;
  unsigned char item;
  int count;
  float age;
};
static std::vector<Drop> drops;

static void SpawnDrop(unsigned char item, int count, int x, int y, int z) {
  if (item == ITEM_NONE || count <= 0) return;
  Drop d;
  d.pos = Vector3{x + 0.5f, y + 0.35f, z + 0.5f};
  d.vel = Vector3{(float)GetRandomValue(-60, 60) / 100.0f,
                  (float)GetRandomValue(60, 140) / 100.0f,
                  (float)GetRandomValue(-60, 60) / 100.0f};
  d.item = item;
  d.count = count;
  d.age = 0.0f;
  drops.push_back(d);
}

static void UpdateDrops(float dt, Vector3 playerFeet) {
  Vector3 mid = {playerFeet.x, playerFeet.y + 0.9f, playerFeet.z};
  for (size_t i = 0; i < drops.size();) {
    Drop &d = drops[i];
    d.age += dt;
    d.vel.y -= GRAVITY * dt;
    d.pos = Vector3Add(d.pos, Vector3Scale(d.vel, dt));
    // settle on the first solid block under the drop
    int bx = (int)floorf(d.pos.x), by = (int)floorf(d.pos.y), bz = (int)floorf(d.pos.z);
    if (SolidAt(bx, by, bz)) {
      d.pos.y = (float)by + 0.35f;
      d.vel.y = 0.0f;
      d.vel.x *= 0.6f;
      d.vel.z *= 0.6f;
    }
    // pickup: only after a short delay so drops don't teleport into you instantly
    // pickup only in survival; creative drops would pile up forever since nothing is consumed
    bool take = !cfg.creative && d.age > 0.25f && Vector3Distance(d.pos, mid) < 1.6f;
    if (take) {
      if (AddItem(d.item, d.count) == 0) {
        PlayVaried(sfxStep, 0.25f, 0.2f);
        drops.erase(drops.begin() + (long)i);
        continue;
      }
    }
    // despawn after a couple of minutes so the vector can't grow without bound
    if (d.age > 120.0f) {
      drops.erase(drops.begin() + (long)i);
      continue;
    }
    i++;
  }
}

static void DrawDrops() {
  for (const Drop &d : drops) {
    float bob = sinf(d.age * 3.0f) * 0.06f;
    // tools can be dropped too; they have no block colour, so fall back to their icon colour
    Color c = IsBlockItem(d.item) ? ATLAS_BASE[TileFor(d.item, 2)]
                                  : ATLAS_BASE[ItemTile(d.item) >= 0 ? ItemTile(d.item) : 3];
    DrawCube(Vector3{d.pos.x, d.pos.y + bob, d.pos.z}, 0.28f, 0.28f, 0.28f, c);
    DrawCubeWires(Vector3{d.pos.x, d.pos.y + bob, d.pos.z}, 0.30f, 0.30f, 0.30f,
                  Fade(BLACK, 0.35f));
  }
}

// ---------- hotbar ----------
// draws one inventory slot; shared by the hotbar and the inventory screen
static void DrawSlot(int x, int y, int size, const ItemStack &s, bool highlight, float alpha) {
  DrawRectangle(x, y, size, size, Fade(Color{0, 0, 0, 255}, 0.5f * alpha));
  DrawRectangleLines(x, y, size, size,
                     highlight ? Fade(WHITE, alpha) : Fade(Color{40, 40, 40, 200}, alpha));
  if (s.count == 0) return;
  int tile = ItemTile(s.item);
  int pad = size / 7;
  Rectangle dst = {(float)(x + pad), (float)(y + pad), (float)(size - 2 * pad),
                   (float)(size - 2 * pad)};
  if (tile >= 0) {
    Rectangle src = {(float)(tile * TILE), 0, (float)TILE, (float)TILE};
    DrawTexturePro(atlas, src, dst, Vector2{0, 0}, 0, Fade(WHITE, alpha));
  } else {
    // no icon yet (tools): shrink the name until it fits the slot instead of overflowing
    const char *n = ItemName(s.item);
    int fs = size / 4;
    while (fs > 7 && MeasureText(n, fs) > size - 6) fs--;
    DrawText(n, x + (size - MeasureText(n, fs)) / 2, y + size / 2 - fs / 2, fs,
             Fade(RAYWHITE, alpha));
  }
  if (s.count > 1)
    DrawText(TextFormat("%d", s.count), x + size - 20, y + size - 18, 14, Fade(WHITE, alpha));

  // durability bar for worn tools: green -> red as it approaches breaking
  if (IsToolItem(s.item) && ToolMaxDamage(s.item)) {
    float left = 1.0f - (float)s.damage / (float)ToolMaxDamage(s.item);
    Color c = {Byte(255.0f * (1.0f - left)), Byte(255.0f * left), 40, 255};
    int w = size - 8, h = 3;
    DrawRectangle(x + 4, y + size - 6, w, h, Fade(BLACK, 0.7f * alpha));
    DrawRectangle(x + 4, y + size - 6, (int)(w * left), h, Fade(c, alpha));
  }
}

static void DrawHotbar(int selected, float alpha) {
  const int slot = 52;
  int totalW = HOTBAR_SLOTS * slot;
  int x0 = (GetScreenWidth() - totalW) / 2;
  int y0 = GetScreenHeight() - slot - 10;
  for (int i = 0; i < HOTBAR_SLOTS; i++)
    DrawSlot(x0 + i * slot, y0, slot - 4, inv[i], i == selected, alpha);
}

// Health left, hunger right, just above the hotbar. Each cell is 2 units, drawn in halves,
// so a 20-point stat reads as 10 icons like Minecraft. Rectangles instead of heart sprites:
// same information, no asset. Hidden in creative, where the stats don't run.
static void DrawStatusBars(float alpha) {
  const int seg = 12, gap = 2, CELLS = 10;
  const int totalW = HOTBAR_SLOTS * 52;
  int x0 = (GetScreenWidth() - totalW) / 2;
  int y = GetScreenHeight() - 52 - 10 - 26;   // clears the hotbar top (H-62) by 2 px
  const struct { float v; int x; Color bg, fg; } bars[2] = {
      {player.health, x0, {40, 0, 26, 255}, {215, 30, 30, 255}},
      {player.hunger, x0 + totalW - CELLS * (seg + gap), {40, 26, 0, 255}, {200, 140, 40, 255}},
  };
  for (const auto &b : bars)
    for (int i = 0; i < CELLS; i++) {
      int x = b.x + i * (seg + gap);
      DrawRectangle(x, y, seg * 2, seg * 2, Fade(b.bg, 0.6f * alpha));
      int half = (int)Clamp(ceilf(b.v - i * 2.0f), 0.0f, 2.0f);   // 0 empty, 1 half, 2 full
      if (half) DrawRectangle(x, y, half * seg, seg * 2, Fade(b.fg, alpha));
    }
}

// ---------- inventory screen ----------
// display order: main rows on top, hotbar row at the bottom (MC layout)
static int DisplayToInv(int d) { return d < INV_COLS * INV_MAIN_ROWS ? d + HOTBAR_SLOTS : d - INV_COLS * INV_MAIN_ROWS; }

static void ClickSlot(ItemStack &s, bool rightClick) {
  if (cursorStack.count == 0) {                 // pick up
    if (s.count == 0) return;
    if (rightClick) {                           // split: take half, rounded up
      int half = (s.count + 1) / 2;
      cursorStack = s;
      cursorStack.count = (unsigned char)half;
      s.count = (unsigned char)(s.count - half);
      if (s.count == 0) s = ItemStack{};
    } else {
      cursorStack = s;
      s = ItemStack{};
    }
    return;
  }
  if (s.count == 0) {                           // drop onto empty slot
    if (rightClick) {
      s = cursorStack;
      s.count = 1;
      cursorStack.count--;
      if (cursorStack.count == 0) cursorStack = ItemStack{};
    } else {
      s = cursorStack;
      cursorStack = ItemStack{};
    }
    return;
  }
  if (s.item == cursorStack.item && !IsToolItem(s.item)) {   // merge (tools would lose wear)
    int space = MAX_STACK - (int)s.count;
    int move = std::min(space, rightClick ? 1 : (int)cursorStack.count);
    s.count = (unsigned char)(s.count + move);
    cursorStack.count = (unsigned char)(cursorStack.count - move);
    if (cursorStack.count == 0) cursorStack = ItemStack{};
    return;
  }
  std::swap(s, cursorStack);                    // different item: swap
}

// the 3x3 crafting grid; persists across opening/closing the inventory so ingredients are
// never silently destroyed (they are returned to the player on shutdown instead)
static ItemStack craftGrid[CRAFT_SLOTS];

// grid contents as plain ids, for the matcher
static void GridIds(unsigned char out[CRAFT_SLOTS]) {
  for (int i = 0; i < CRAFT_SLOTS; i++) out[i] = craftGrid[i].count ? craftGrid[i].item : 0;
}

static int CurrentRecipe() {
  unsigned char g[CRAFT_SLOTS];
  GridIds(g);
  return MatchRecipe(g);
}

// Returns true when the carried stack just became the crafted output (or was merged into it).
// Crafting is "take the result" like Minecraft: nothing is consumed until the result is taken.
static bool TakeCraftResult() {
  int r = CurrentRecipe();
  if (r < 0) return false;
  unsigned char outItem = RECIPES[r].out;
  unsigned char outCount = RECIPES[r].outCount;

  if (cursorStack.count == 0) {
    cursorStack = ItemStack{outItem, outCount, 0};
  } else if (cursorStack.item == outItem && !IsToolItem(outItem) &&
             cursorStack.count + outCount <= MAX_STACK) {
    cursorStack.count = (unsigned char)(cursorStack.count + outCount);
  } else {
    return false;   // hands full of something else, or no room: do nothing
  }
  ConsumeGrid(craftGrid);
  return true;
}

static void DrawInventory() {
  const int cell = 52;
  int w = INV_COLS * cell;
  int gridH = (INV_MAIN_ROWS + 1) * cell;

  // crafting strip sits ABOVE the inventory, so the panel has to be taller than the grid
  const int cs = 44, cgap = 4;
  int gw = CRAFT_W * cs + (CRAFT_W - 1) * cgap;
  int craftStripH = CRAFT_H * (cs + cgap) + 64;      // crafting title + grid + inventory title area
  int panelW = w + 28;

  int x0 = (GetScreenWidth() - w) / 2;
  int y0 = (GetScreenHeight() - gridH - craftStripH) / 2 + craftStripH;   // grid shifted down
  int panelX = x0 - 14;
  int panelY = y0 - 58 - craftStripH;
  int panelH = gridH + 80 + craftStripH;

  DrawRectangle(panelX, panelY, panelW, panelH, Fade(BLACK, 0.82f));
  DrawRectangleLines(panelX, panelY, panelW, panelH, LIGHTGRAY);
  DrawText("Inventory", x0, y0 - 50, 24, RAYWHITE);
  DrawText("click: take/put   right-click: half   E/Esc: close", x0, y0 - 21, 13, LIGHTGRAY);

  Vector2 m = GetMousePosition();
  bool lclick = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
  bool rclick = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

  // ---- crafting area: 3x3 grid + arrow + result ----
  int cxp = panelX + 16;                    // left-aligned: arrow + result must stay inside the panel
  int cyp = panelY + 32;

  DrawText("Crafting", cxp, panelY + 8, 18, RAYWHITE);
  for (int i = 0; i < CRAFT_SLOTS; i++) {
    int cx = cxp + (i % CRAFT_W) * (cs + cgap);
    int cy = cyp + (i / CRAFT_W) * (cs + cgap);
    Rectangle r = {(float)cx, (float)cy, (float)cs, (float)cs};
    bool hover = CheckCollisionPointRec(m, r);
    DrawSlot(cx, cy, cs, craftGrid[i], hover, 1.0f);
    if (hover && (lclick || rclick)) ClickSlot(craftGrid[i], rclick);
  }

  // arrow + result slot, vertically centred on the grid
  int ax = cxp + gw + 16;
  int ay = cyp + (CRAFT_H * (cs + cgap) - cgap) / 2;
  DrawLine(ax, ay, ax + 22, ay, LIGHTGRAY);
  DrawLine(ax + 16, ay - 5, ax + 22, ay, LIGHTGRAY);
  DrawLine(ax + 16, ay + 5, ax + 22, ay, LIGHTGRAY);

  int r = CurrentRecipe();
  ItemStack result;
  if (r >= 0) result = ItemStack{RECIPES[r].out, RECIPES[r].outCount, 0};
  int rx = ax + 32, ry = ay - cs / 2;
  Rectangle rr = {(float)rx, (float)ry, (float)cs, (float)cs};
  bool rHover = CheckCollisionPointRec(m, rr);
  DrawSlot(rx, ry, cs, result, rHover && r >= 0, 1.0f);
  if (rHover && lclick && r >= 0) TakeCraftResult();

  if (r >= 0) DrawText(RECIPES[r].name, rx + cs + 10, ry + cs / 2 - 8, 16, LIGHTGRAY);

  // ---- inventory grid ----
  for (int d = 0; d < INV_SLOTS; d++) {
    int row = d / INV_COLS, col = d % INV_COLS;
    int x = x0 + col * cell;
    int y = y0 + row * cell + (row >= INV_MAIN_ROWS ? 14 : 0);   // gap above the hotbar row
    Rectangle r = {(float)x, (float)y, (float)(cell - 4), (float)(cell - 4)};
    int idx = DisplayToInv(d);
    bool hover = CheckCollisionPointRec(m, r);
    DrawSlot(x, y, cell - 4, inv[idx], hover, 1.0f);
    if (hover) {
      int fs = 16;
      const char *name = inv[idx].count ? ItemName(inv[idx].item) : "";
      if (name[0]) {
        int tw = MeasureText(name, fs);
        int tx = x + (cell - 4 - tw) / 2;
        int ty = y0 - 70;
        if (ty < panelY + 4) ty = panelY + 4;
        DrawRectangle(tx - 4, ty - 2, tw + 8, fs + 4, Fade(BLACK, 0.8f));
        DrawText(name, tx, ty, fs, RAYWHITE);
      }
    }
    if (hover && (lclick || rclick)) ClickSlot(inv[idx], rclick);
  }
  // close by clicking outside the panel
  Rectangle panel = {(float)panelX, (float)panelY, (float)panelW, (float)panelH};
  if ((lclick || rclick) && !CheckCollisionPointRec(m, panel) && cursorStack.count) {
    // return the carried stack instead of dropping it on the floor
    cursorStack.count = (unsigned char)AddItem(cursorStack.item, cursorStack.count);
    if (cursorStack.count == 0) cursorStack = ItemStack{};
  }
  if (cursorStack.count) {
    int tile = ItemTile(cursorStack.item);
    if (tile >= 0) {
      Rectangle src = {(float)(tile * TILE), 0, (float)TILE, (float)TILE};
      Rectangle dst = {m.x - 20, m.y - 20, 40, 40};
      DrawTexturePro(atlas, src, dst, Vector2{0, 0}, 0, WHITE);
    }
    if (cursorStack.count > 1)
      DrawText(TextFormat("%d", cursorStack.count), (int)m.x + 12, (int)m.y + 12, 14, WHITE);
  }
}

// put the carried stack back when the screen closes, so items can't be lost
static void CloseInventory() {
  if (cursorStack.count) {
    cursorStack.count = (unsigned char)AddItem(cursorStack.item, cursorStack.count);
    if (cursorStack.count) SpawnDrop(cursorStack.item, cursorStack.count,
                                     (int)floorf(player.pos.x), (int)floorf(player.pos.y),
                                     (int)floorf(player.pos.z));
    cursorStack = ItemStack{};
  }
  invOpen = false;
}

// drain the crafting grid back into the inventory (and onto the floor if it genuinely full),
// so closing the game with ingredients in the grid does not delete them
static void ReturnCraftGrid() {
  for (int i = 0; i < CRAFT_SLOTS; i++) {
    ItemStack &cell = craftGrid[i];
    if (cell.count == 0) continue;
    int left = AddItem(cell.item, cell.count);
    if (left > 0)
      SpawnDrop(cell.item, left, (int)floorf(player.pos.x), (int)floorf(player.pos.y),
                (int)floorf(player.pos.z));
    cell = ItemStack{};
  }
}

// ---------- held item view ----------
// ponytail: a 2D textured quad of the selected block, not a 3D model. Swap in a mesh if
// the flat look stops reading. Drawn after EndMode3D so there is no depth fighting.
static void DrawHeldItem(float swing) {
  const ItemStack &s = inv[hotbarSel];
  if (s.count == 0) return;
  int tile = ItemTile(s.item);
  if (tile < 0) return;
  Rectangle src = {(float)(tile * TILE), 0, (float)TILE, (float)TILE};
  float size = 110.0f;
  float x = GetScreenWidth() - size - 24.0f;
  float y = GetScreenHeight() - size - 70.0f;
  float lift = sinf(swing * PI) * 26.0f;          // kicks up then settles
  float rot = sinf(swing * PI) * -22.0f;
  Rectangle dst = {x, y + lift, size, size};
  DrawTexturePro(atlas, src, dst, Vector2{size * 0.5f, size * 0.5f}, rot, WHITE);
}

// ---------- pause menu ----------
static bool Button(Rectangle r, const char *label) {
  bool hover = CheckCollisionPointRec(GetMousePosition(), r);
  DrawRectangleRec(r, hover ? Color{80, 80, 80, 255} : Color{48, 48, 48, 255});
  DrawRectangleLinesEx(r, 1, LIGHTGRAY);
  int tw = MeasureText(label, 20);
  DrawText(label, (int)(r.x + (r.width - tw) / 2), (int)(r.y + (r.height - 20) / 2), 20,
           RAYWHITE);
  return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// returns true when the game should quit
static bool DrawPauseMenu(bool &borderless, bool &resume) {
  int sw = GetScreenWidth(), sh = GetScreenHeight();
  DrawRectangle(0, 0, sw, sh, Fade(BLACK, 0.6f));
  const char *title = "CrafClone  -  Paused";
  DrawText(title, (sw - MeasureText(title, 40)) / 2, sh / 2 - 200, 40, RAYWHITE);

  int cx = sw / 2 - 180;
  int y = sh / 2 - 130;
  const int h = 38, gap = 10;

  auto row = [&](const char *name, const char *value, bool minus, bool plus) -> int {
    DrawText(name, cx, y + 10, 20, LIGHTGRAY);
    int bx = cx + 240;
    Rectangle rm = {(float)bx, (float)y, 40, (float)h};
    Rectangle rp = {(float)(bx + 50), (float)y, 40, (float)h};
    bool clickMinus = Button(rm, "-");
    bool clickPlus = Button(rp, "+");
    int vw = MeasureText(value, 20);
    DrawText(value, bx + 130 - vw / 2, y + 10, 20, RAYWHITE);
    if (clickMinus && minus) return -1;
    if (clickPlus && plus) return 1;
    return 0;
  };

  char buf[32];
  snprintf(buf, sizeof(buf), "%d", cfg.renderDist);
  if (int d = row("Render distance", buf, cfg.renderDist > 4, cfg.renderDist < 16))
    cfg.renderDist += d;

  snprintf(buf, sizeof(buf), "%.0f", cfg.fov);
  y += h + gap;
  if (int d = row("Field of view", buf, cfg.fov > 50, cfg.fov < 110))
    cfg.fov = Clamp(cfg.fov + d * 5.0f, 50.0f, 110.0f);

  snprintf(buf, sizeof(buf), "%.3f", cfg.sensitivity);
  y += h + gap;
  if (int d = row("Sensitivity", buf, cfg.sensitivity > 0.0005f, cfg.sensitivity < 0.02f))
    cfg.sensitivity = Clamp(cfg.sensitivity + d * 0.0005f, 0.0005f, 0.02f);

  snprintf(buf, sizeof(buf), "%.2f", cfg.volume);
  y += h + gap;
  if (int d = row("Volume", buf, cfg.volume > 0.01f, cfg.volume < 1.0f)) {
    cfg.volume = Clamp(cfg.volume + d * 0.1f, 0.0f, 1.0f);
    SetMasterVolume(cfg.volume);
  }

  y += h + gap + 14;
  Rectangle rRes = {(float)cx, (float)y, 180, (float)h};
  Rectangle rFs = {(float)(cx + 200), (float)y, 180, (float)h};
  bool quit = false;
  if (Button(rRes, "Resume")) resume = true;
  if (Button(rFs, borderless ? "Windowed" : "Fullscreen")) {
    if (!borderless) {
      int mon = GetCurrentMonitor();
      SetWindowSize(GetMonitorWidth(mon), GetMonitorHeight(mon));
      ToggleBorderlessWindowed();
      borderless = true;
    } else {
      ToggleBorderlessWindowed();
      SetWindowSize(1280, 720);
      borderless = false;
    }
  }
  y += h + gap;
  Rectangle rQ = {(float)cx, (float)y, 380, (float)h};
  if (Button(rQ, "Save and quit")) quit = true;
  DrawText("Esc resumes  -  settings are saved on exit", cx, y + h + 20, 16, DARKGRAY);
  return quit;
}

// ---------- selftest ----------
// ponytail: one tiny self-check for the math that's easy to get wrong
static int SelfTest() {
  int fail = 0;
  auto check = [&](bool ok, const char *what) {
    if (!ok) { TraceLog(LOG_ERROR, "FAIL: %s", what); fail++; }
  };
  check(FloorDiv(-1, 16) == -1 && FloorDiv(-16, 16) == -1 && FloorDiv(15, 16) == 0, "FloorDiv");
  check(Idx(0, 0, 0) == 0 && Idx(1, 0, 0) == 1, "Idx");

  // day/night: brightness must peak at noon and bottom out at midnight
  float savedDay = dayTime;
  dayTime = 0.25f; float noon = DayBrightness();
  dayTime = 0.75f; float mid = DayBrightness();
  check(noon > 0.99f && mid < 0.32f && noon > mid, "day/night peaks at noon");
  dayTime = savedDay;

  chunks.clear();
  Chunk *c = LoadChunk(0, 0);
  bool solid = false;
  for (int y = 0; y < CY; y++) if (SolidAt(0, y, 0)) solid = true;
  check(solid, "loaded terrain has blocks");

  // place a known block in the air, ray down, expect to hit its top face
  SetBlockWorld(2, 60, 2, STONE);
  RayHit h2 = RaycastVoxel(Vector3{2.5f, 65.5f, 2.5f}, Vector3{0, -1, 0}, 20.0f);
  check(h2.hit && h2.y == 60 && h2.py == 61, "raycast hits placed block");
  check(AABBOverlapsBlock(Vector3{2.5f, 60.2f, 2.5f}, 2, 60, 2), "AABB overlap");
  check(!AABBOverlapsBlock(Vector3{2.5f, 62.5f, 2.5f}, 2, 60, 2), "AABB miss");

  // strafe: at yaw 0 forward is +Z, so screen-right must be -X (A left, D right)
  Vector3 fwd0 = ForwardVec(0, 0), right0 = RightVec(0);
  check(fabsf(fwd0.z - 1) < 1e-4f && fabsf(fwd0.x) < 1e-4f, "forward at yaw0 is +Z");
  check(right0.x < -0.99f && fabsf(right0.z) < 1e-4f, "right at yaw0 is -X");
  // right must stay perpendicular to forward and flip sign when facing backwards
  Vector3 fwd1 = ForwardVec(1.5707963f, 0), right1 = RightVec(1.5707963f);
  check(fabsf(Vector3DotProduct(fwd1, right1)) < 1e-4f, "right perpendicular to forward");
  check(fabsf(right1.z - 1) < 1e-4f && fabsf(right1.x) < 1e-4f, "right at yaw90 is +Z");

  // audio: base owns the sample data, aliases must be freed first (wrong order = heap
  // corruption). Validity is checked here; the unload order is proven by the exit code.
  if (IsAudioDeviceReady()) {
    Sound base = MakeNoiseSound(0.02f, 120.0f, 30.0f);
    Sound al = LoadSoundAlias(base);
    check(IsSoundValid(base) && IsSoundValid(al), "sound + alias valid");
    UnloadSoundAlias(al);
    UnloadSound(base);
  }

  // inventory: add/take/merge must keep counts exact
  for (int i = 0; i < INV_SLOTS; i++) inv[i] = ItemStack{};
  check(AddItem(STONE, 60) == 0 && inv[0].count == 60, "AddItem into an empty slot");
  check(AddItem(STONE, 10) == 0, "AddItem splits across stacks");
  check(inv[0].count == MAX_STACK && inv[1].count == 6, "first stack fills, rest spills");
  check(CountItem(STONE) == 70, "AddItem total is exact");
  check(TakeItem(STONE, 20) == 20 && CountItem(STONE) == 50, "TakeItem removes exact");
  check(TakeItem(STONE, 999) == 50 && CountItem(STONE) == 0, "TakeItem stops at what exists");
  int over = AddItem(STONE, MAX_STACK * (INV_SLOTS + 1));
  check(over > 0 && CountItem(STONE) == MAX_STACK * INV_SLOTS, "AddItem reports leftover when full");
  check(BlockDrop(GRASS) == DIRT && BlockDrop(LEAVES) == ITEM_NONE, "block drop table");
  check(Hardness(STONE) > Hardness(DIRT), "stone is harder than dirt");

  // survival metabolism: drain, walk cost, regen gate, regen cost, starvation
  {
    float hp = 20.0f, hg = 20.0f;
    TickSurvival(1.0f, 0.0f, 0.0f, hp, hg);
    check(hg < 20.0f && hp == 20.0f, "idle hunger drains, health untouched");
    float hg2 = 20.0f;
    TickSurvival(1.0f, 5.0f, 0.0f, hp, hg2);
    check(hg2 < hg, "walking drains hunger faster than standing");

    hp = 10.0f; hg = 19.0f;
    float hpBefore = hp, hgBefore = hg;
    for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // 1 s fed
    check(hp > hpBefore && hg < hgBefore, "fed regen heals and costs hunger");

    hp = 10.0f; hg = 17.9f;
    for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // 1 s, below gate
    check(hp == 10.0f, "no regen below the hunger gate");

    hp = 10.0f; hg = 0.0f;
    for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // 1 s starving
    check(hp < 10.0f, "starvation damages health");

    hp = 20.0f; hg = 20.0f;
    for (int i = 0; i < 600; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);  // 10 s idle
    check(hp == 20.0f && hg > 19.9f, "stats stay in [0..20] (no drift past full)");

    hp = 0.05f; hg = 0.0f;
    for (int i = 0; i < 600; i++) TickSurvival(1.0f / 60.0f, 1.0f, 0.0f, hp, hg);  // long starvation
    check(hp == 0.0f, "starvation cannot go below zero");

    // regen must pause after a hit, else chip damage is out-healed and hits stop mattering
    hp = 10.0f; hg = 20.0f;
    for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.5f, hp, hg);   // 1 s in i-frames
    check(hp == 10.0f, "no regen during the post-hit hold");
    for (int i = 0; i < 60; i++) TickSurvival(1.0f / 60.0f, 0.0f, 0.0f, hp, hg);   // hold expired
    check(hp > 10.0f, "regen resumes once the hold ends");
  }

  // fall damage: 3 blocks are free, everything after that is 1 HP per block
  check(FallDamage(0.0f) == 0.0f && FallDamage(3.0f) == 0.0f, "falls up to 3 blocks are free");
  check(FallDamage(3.01f) > 0.0f, "the 4th block hurts");
  check(FallDamage(8.0f) == 5.0f, "8-block fall costs 5 HP");
  check(FallDamage(30.0f) >= MAX_STAT, "a long fall is lethal in one hit");

  // the accumulator: a drop is measured apex-to-landing, so it can't drift or double-count
  {
    check(FallDistance(10.0f, 0.0f) == 10.0f, "drop is apex minus landing");
    check(FallDistance(0.0f, 10.0f) == 0.0f, "rising is not a fall");
    check(FallDistance(34.0f, 22.0f) == 12.0f, "landing on a ledge charges only to the ledge");
    check(fabsf(FallDamage(FallDistance(34.0f, 33.5f))) < 0.001f, "half a block under the grace is free");
    check(FallDistance(48.0f, 22.0f) > 20.0f, "a ravine drop is lethal");
  }

  // Hurt: applies once, then the invulnerability window swallows the rest
  {
    bool wasCreative = cfg.creative;
    cfg.creative = false;                 // creative is damage-immune; test survival behaviour
    player.health = MAX_STAT;
    invulnTimer = 0.0f; regenHold = 0.0f; hurtFlash = 0.0f;
    check(Hurt(4.0f) && player.health == 16.0f, "Hurt applies damage");
    check(!Hurt(4.0f) && player.health == 16.0f, "second hit inside i-frames is ignored");
    check(regenHold > 0.0f && hurtFlash > 0.0f, "a hit locks regen and flashes the screen");

    // creative is immune: the guard lives in Hurt, so every future source gets it for free
    invulnTimer = 0.0f;
    cfg.creative = true;
    check(!Hurt(4.0f) && player.health == 16.0f, "creative takes no damage");
    cfg.creative = wasCreative;

    player.health = 20.0f; invulnTimer = 0.0f; regenHold = 0.0f; hurtFlash = 0.0f;
  }

  // tool rules: stone is gated behind the pickaxe, and a matching tool is faster
  check(BreakTime(STONE, ITEM_NONE) < 0.0f, "bare hands cannot harvest stone");
  check(BreakTime(STONE, ITEM_AXE) < 0.0f, "axe is not a pickaxe");
  check(BreakTime(STONE, ITEM_PICKAXE) > 0.0f, "pickaxe harvests stone");
  check(BreakTime(STONE, ITEM_PICKAXE) < Hardness(STONE), "pickaxe is faster than bare hands");
  check(BreakTime(DIRT, ITEM_NONE) > 0.0f, "dirt is breakable bare-handed");
  check(BreakTime(WOOD, ITEM_AXE) < BreakTime(WOOD, ITEM_NONE), "axe speeds up wood");
  check(BreakTime(WOOD, ITEM_PICKAXE) == BreakTime(WOOD, ITEM_NONE), "wrong tool is no bonus");

  // durability: wears 1 per use, breaks exactly at the cap, non-tools never wear
  {
    ItemStack t{ITEM_PICKAXE, 1, 0};
    check(!WearTool(t) && t.damage == 1, "tool wears without breaking");
    t.damage = (unsigned short)(ToolMaxDamage(t.item) - 1);
    check(WearTool(t) && t.count == 0 && t.item == ITEM_NONE, "tool breaks at max damage");
    ItemStack d{STONE, 5, 0};
    check(!WearTool(d) && d.damage == 0 && d.count == 5, "non-tools do not wear");
  }

  // tools never merge in AddItem (a fresh tool must not inherit someone else's wear)
  for (int i = 0; i < INV_SLOTS; i++) inv[i] = ItemStack{};
  inv[0] = ItemStack{ITEM_PICKAXE, 1, 7};
  check(AddItem(ITEM_PICKAXE, 1) == 0 && inv[1].item == ITEM_PICKAXE && inv[1].damage == 0,
        "tools stack separately with their own wear");
  check(CountItem(ITEM_PICKAXE) == 2, "two separate tools counted");

  // crafting: bounding-box normalization means position in the 3x3 should not matter
  {
    unsigned char g[CRAFT_SLOTS] = {0};
    check(MatchRecipe(g) < 0, "empty grid crafts nothing");

    g[0] = WOOD;                                     // one wood, top-left
    int r = MatchRecipe(g);
    check(r >= 0 && RECIPES[r].out == PLANK && RECIPES[r].outCount == 4, "log -> 4 planks");

    unsigned char g2[CRAFT_SLOTS] = {0};
    g2[4] = WOOD;                                    // same log, centre of the grid
    check(MatchRecipe(g2) == r, "recipe position does not matter");

    unsigned char g3[CRAFT_SLOTS] = {0};
    g3[0] = WOOD; g3[8] = WOOD;                      // two logs must NOT match the one-log recipe
    check(MatchRecipe(g3) < 0, "extra ingredient breaks the match");

    unsigned char pick[CRAFT_SLOTS] = {0};
    pick[0] = PLANK; pick[1] = PLANK; pick[2] = PLANK;
    pick[4] = ITEM_STICK; pick[7] = ITEM_STICK;
    int rp = MatchRecipe(pick);
    check(rp >= 0 && RECIPES[rp].out == ITEM_PICKAXE, "pickaxe recipe matches");

    // the same shape shifted one column left should still match (bbox-normalized)
    unsigned char pick2[CRAFT_SLOTS] = {0};
    pick2[0] = PLANK; pick2[1] = PLANK;
    pick2[3] = ITEM_STICK; pick2[6] = ITEM_STICK;
    check(MatchRecipe(pick2) < 0, "a 2-wide shape is not the 3-wide pickaxe");

    // consume one of each filled cell
    ItemStack grid[CRAFT_SLOTS];
    for (int i = 0; i < CRAFT_SLOTS; i++) grid[i] = ItemStack{};
    grid[4] = ItemStack{WOOD, 3, 0};
    ConsumeGrid(grid);
    check(grid[4].count == 2, "crafting consumes one");
    grid[4].count = 1;
    ConsumeGrid(grid);
    check(grid[4].count == 0, "last ingredient clears the cell");
  }

  // neighbor chunk is a separate block store; a border block is visible at the seam
  Chunk *c2 = LoadChunk(1, 0);
  SetBlockWorld(16, 60, 2, STONE);   // first column of chunk 1
  check(GetBlockWorld(16, 60, 2) == STONE, "border block readable across chunks");
  check(GetBlockWorld(15, 60, 2) == AIR, "chunk 0 border not aliased");

  dirty.clear();
  UnloadChunk(c2);
  UnloadChunk(c);
  chunks.clear();
  TraceLog(LOG_INFO, "SelfTest %s", fail ? "FAILED" : "OK");
  return fail;
}

int main(int argc, char **argv) {
  if (argc > 1 && (strcmp(argv[1], "-selftest") == 0 || strcmp(argv[1], "--selftest") == 0)) {
    InitWindow(16, 16, "CrafClone selftest");   // raylib needs a GL context for meshes
    InitAudioDevice();                          // and a device for the sound-alias check
    SetTraceLogLevel(LOG_ALL);
    int rc = SelfTest();
    CloseAudioDevice();
    CloseWindow();
    return rc;
  }

  InitWindow(1280, 720, "CrafClone");
  bool bench = (argc > 1 && strcmp(argv[1], "-bench") == 0);
  SetTargetFPS(bench ? 0 : 60);   // uncapped while benching so frame time is real work
  SetExitKey(KEY_NULL);           // Esc opens the pause menu instead of quitting
  DisableCursor();

  LoadSettings();

  // fog shader: pass 1 gives each face a distance fade into the sky colour
  fogShader = LoadShaderFromMemory(FOG_VS, FOG_FS);
  locViewPos = GetShaderLocation(fogShader, "viewPos");
  locFogColor = GetShaderLocation(fogShader, "fogColor");
  locFogStart = GetShaderLocation(fogShader, "fogStart");
  locFogEnd = GetShaderLocation(fogShader, "fogEnd");

  atlas = MakeAtlas();
  mat = LoadMaterialDefault();
  mat.shader = fogShader;         // default shader replaced; uniforms we set ourselves
  mat.maps[MATERIAL_MAP_DIFFUSE].texture = atlas;
  mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

  InitAudioDevice();
  audioReady = IsAudioDeviceReady();
  if (audioReady) {
    SetMasterVolume(cfg.volume);
    sfxStep = MakeSfx(0.06f, 0.0f, 55.0f);
    sfxBreak = MakeSfx(0.16f, 140.0f, 24.0f);
    sfxPlace = MakeSfx(0.10f, 220.0f, 34.0f);
    sfxJump = MakeSfx(0.09f, 380.0f, 30.0f);
  }

  LoadEdits();

  LoadChunk(0, 0);   // spawn chunk first so the surface scan can see blocks
  for (int y = CY - 1; y >= 0; y--)
    if (SolidAt(8, y, 8)) { player.pos = Vector3{8.5f, (float)y + 1.001f, 8.5f}; break; }

  int lastCX = 0, lastCZ = 0;
  bool borderless = false;
  bool paused = false;
  int savedW = 1280, savedH = 720;
  float swing = 0.0f;           // held-item click animation, 1 -> 0
  float hotbarAlpha = 1.0f;     // fades to dim when not recently used
  bool mining = false;          // holding LMB on a block
  int mineX = 0, mineY = 0, mineZ = 0;
  float mineT = 0.0f;           // seconds spent on the current block
  GiveStarterItems();
  RefreshChunks(0, 0);

  // ponytail: -bench streams chunks by flying the player and reports the worst
  // per-frame work time after warm-up, the number that proves stutter is bounded.
  // Uncap FPS and use Release for a meaningful number.
  int benchLeft = bench ? 260 : 0;
  int benchWarm = bench ? 40 : 0;
  float benchMaxMs = 0, benchT1 = (float)GetTime();
  int benchMeshed = 0;

  while (!WindowShouldClose()) {
    float dt = fminf(GetFrameTime(), 0.05f);

    if (IsKeyPressed(KEY_ESCAPE) || GPBtnPressed(GAMEPAD_BUTTON_MIDDLE_RIGHT)) {
      if (invOpen) {                         // Esc closes the inventory before the pause menu
        CloseInventory();
        DisableCursor();
      } else {
        paused = !paused;
        if (paused) EnableCursor(); else DisableCursor();
      }
    }

    if (benchLeft > 0) {
      player.fly = true;
      // Fixed distance per frame, NOT speed*dt: a time-scaled sweep means faster frames travel
      // less and stream fewer chunks, so "worst frame" feeds back on itself and runs become
      // incomparable. 0.5/frame x 260 = ~8 chunk columns, slow enough that GEN_BUDGET=1 keeps
      // the world fully loaded (a faster sweep measures a half-empty world instead).
      player.pos.x += 0.5f;
      player.pos.y = 48.0f;
      player.pos.z = 8.0f;
      player.yaw = 1.5707963f;
      player.pitch = -0.35f;   // look down at the ground so terrain fills the frame
    }

    if (!paused) {
      dayTime = fmodf(dayTime + dt / DAY_LENGTH, 1.0f);
      float moved = 0.0f;
      if (!invOpen) moved = UpdatePlayer(dt);   // inventory freezes movement but not the world
      // creative: no metabolism and no damage (it already skips item consumption)
      if (!cfg.creative) {
        if (invulnTimer > 0.0f) invulnTimer = fmaxf(0.0f, invulnTimer - dt);
        if (regenHold > 0.0f) regenHold = fmaxf(0.0f, regenHold - dt);
        if (!invOpen) TickSurvival(dt, moved, regenHold, player.health, player.hunger);
      }
      UpdateParticles(dt);
      UpdateDrops(dt, player.pos);
      if (swing > 0) swing = fmaxf(0.0f, swing - dt * 4.5f);
    }

    int pcx = FloorDiv((int)floorf(player.pos.x), CX);
    int pcz = FloorDiv((int)floorf(player.pos.z), CZ);
    if (pcx != lastCX || pcz != lastCZ) {
      RefreshChunks(pcx, pcz);
      lastCX = pcx; lastCZ = pcz;
    }
    ProcessQueues();

    Vector3 eye = {player.pos.x, player.pos.y + EYE, player.pos.z};
    Vector3 dir = ForwardVec(player.yaw, player.pitch);
    RayHit hit = RaycastVoxel(eye, dir, 6.0f);

    bool uiBlocking = paused || invOpen;
    if (!uiBlocking && hit.hit) {
      if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || GetGamepadAxisMovement(GP, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f) {
        if (!mining || mineX != hit.x || mineY != hit.y || mineZ != hit.z) {
          mining = true;
          mineX = hit.x; mineY = hit.y; mineZ = hit.z;
          mineT = 0.0f;
        }
        unsigned char b = GetBlockWorld(hit.x, hit.y, hit.z);
        float need = BreakTime(b, inv[hotbarSel].item);
        // -1 = this block cannot be harvested with what is held: never finishes, no progress
        if (need < 0.0f) {
          mining = false;
          mineT = 0.0f;
        } else {
          mineT += dt;
          bool done = cfg.creative || mineT >= need;
          if (done) {
            SetBlockWorld(hit.x, hit.y, hit.z, AIR);
            SpawnBreakParticles(b, hit.x, hit.y, hit.z);
            if (!cfg.creative) {
              SpawnDrop(BlockDrop(b), 1, hit.x, hit.y, hit.z);   // creative gives no drops
              if (WearHeldTool()) {
                PlayVaried(sfxBreak, 0.35f, 0.3f);   // the tool shattered
                swing = 1.0f;
              }
            }
            PlayVaried(sfxBreak, 0.5f, 0.18f);
            swing = 1.0f;
            mining = false;
            mineT = 0.0f;
          }
        }
      } else {
        mining = false;
        mineT = 0.0f;
      }
      if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || GPBtnPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_2)) {
        ItemStack &held = inv[hotbarSel];
        bool canPlace = IsBlockItem(held.item) && (cfg.creative || held.count > 0);
        if (canPlace && GetBlockWorld(hit.px, hit.py, hit.pz) == AIR &&
            !AABBOverlapsBlock(player.pos, hit.px, hit.py, hit.pz)) {
          unsigned char placed = held.item;
          SetBlockWorld(hit.px, hit.py, hit.pz, placed);
          // drain the SELECTED slot, not the first matching stack anywhere, so the hotbar
          // visibly ticks down the block the player is actually holding
          if (!cfg.creative && --held.count == 0) held = ItemStack{};
          PlayVaried(sfxPlace, 0.45f, 0.15f);
          swing = 1.0f;
        }
      }
    } else if (uiBlocking || !hit.hit) {
      mining = false;
      mineT = 0.0f;
    }
    if (!paused) {
      if (IsKeyPressed(KEY_E) || GPBtnPressed(GAMEPAD_BUTTON_RIGHT_FACE_UP)) {
        invOpen = !invOpen;
        if (invOpen) EnableCursor(); else { CloseInventory(); DisableCursor(); }
      }
      if (IsKeyPressed(KEY_C) || GPBtnPressed(GAMEPAD_BUTTON_MIDDLE_LEFT)) {
        cfg.creative = !cfg.creative;
        if (cfg.creative) GiveStarterItems();
      }
      if (!invOpen) {
        for (int i = 0; i < HOTBAR_SLOTS; i++)
          if (IsKeyPressed(KEY_ONE + i)) { hotbarSel = i; hotbarAlpha = 1.0f; }
        float wheel = GetMouseWheelMove();
        // ponytail: LB scrolls left, RB scrolls right; wheel sign kept as-is
        if (GPBtnPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1))  wheel =  1.0f;
        if (GPBtnPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) wheel = -1.0f;
        if (wheel != 0) {
          hotbarSel = (hotbarSel + (wheel > 0 ? HOTBAR_SLOTS - 1 : 1)) % HOTBAR_SLOTS;
          hotbarAlpha = 1.0f;
        }
        if (IsKeyPressed(KEY_F5)) SaveEdits();
        // borderless fullscreen at desktop resolution; ToggleFullscreen() only does exclusive mode
        if (IsKeyPressed(KEY_F11)) {
          if (!borderless) {
            savedW = GetScreenWidth();
            savedH = GetScreenHeight();
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
      }
    }
    hotbarAlpha = fmaxf(0.45f, hotbarAlpha - dt * 0.8f);   // dim when idle, bright on change
    hurtFlash = fmaxf(0.0f, hurtFlash - dt * 2.5f);

    FlushDirty();

    Color sky = SkyColor();
    float bright = DayBrightness();
    Color tint = {Byte(bright), Byte(bright), Byte(bright), 255};
    mat.maps[MATERIAL_MAP_DIFFUSE].color = tint;

    float fogStart = (float)cfg.renderDist * CX * 0.55f;
    float fogEnd = (float)cfg.renderDist * CX * 1.0f;
    float vp[3] = {eye.x, eye.y, eye.z};
    float fc[4] = {sky.r / 255.0f, sky.g / 255.0f, sky.b / 255.0f, 1.0f};
    SetShaderValue(fogShader, locViewPos, vp, SHADER_UNIFORM_VEC3);
    SetShaderValue(fogShader, locFogColor, fc, SHADER_UNIFORM_VEC4);
    SetShaderValue(fogShader, locFogStart, &fogStart, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, locFogEnd, &fogEnd, SHADER_UNIFORM_FLOAT);

    Camera3D cam = {0};
    cam.position = eye;
    cam.target = Vector3Add(eye, dir);
    cam.up = Vector3{0, 1, 0};
    cam.fovy = cfg.fov;
    cam.projection = CAMERA_PERSPECTIVE;

    BeginDrawing();
    ClearBackground(sky);
    BeginMode3D(cam);
    for (auto &kv : chunks)
      if (kv.second->hasMesh)
        DrawMesh(kv.second->mesh, mat,
                 MatrixTranslate((float)(kv.second->cx * CX), 0,
                                 (float)(kv.second->cz * CZ)));
    DrawParticles();
    DrawDrops();
    if (hit.hit)
      DrawCubeWires(Vector3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f}, 1.002f,
                    1.002f, 1.002f, (bright > 0.5f) ? BLACK : RAYWHITE);
    EndMode3D();

    // damage vignette: four edge bands fading to transparent inward. A *solid* band was the
    // first version and looked wrong: over uniform sky it recoloured the whole top of the screen,
    // reading as weather, not as damage. The gradient makes the edge the darkest point, which is
    // what the eye reads as "hurt". Four gradient rects, no shader and no texture.
    if (hurtFlash > 0.0f) {
      int sw = GetScreenWidth(), sh = GetScreenHeight();
      int band = sh / 6;
      Color hurt = Fade(Color{160, 0, 0, 255}, 0.55f * hurtFlash), none = Fade(hurt, 0.0f);
      DrawRectangleGradientV(0, 0, sw, band, hurt, none);                // top
      DrawRectangleGradientV(0, sh - band, sw, band, none, hurt);        // bottom
      DrawRectangleGradientH(0, 0, band, sh, hurt, none);                // left
      DrawRectangleGradientH(sw - band, 0, band, sh, none, hurt);        // right

    }

    // crosshair: white core over a dark outline so it reads on any background
    int hx = GetScreenWidth() / 2, hy = GetScreenHeight() / 2;
    DrawLine(hx - 11, hy, hx + 11, hy, Fade(BLACK, 0.5f));
    DrawLine(hx, hy - 11, hx, hy + 11, Fade(BLACK, 0.5f));
    DrawLine(hx - 10, hy, hx + 10, hy, RAYWHITE);
    DrawLine(hx, hy - 10, hx, hy + 10, RAYWHITE);

    // mining progress bar under the crosshair
    if (mining && hit.hit) {
      unsigned char b = GetBlockWorld(mineX, mineY, mineZ);
      float total = cfg.creative ? 0.001f : BreakTime(b, inv[hotbarSel].item);
      if (total <= 0.0f) total = 0.001f;
      float p = Clamp(mineT / total, 0.0f, 1.0f);
      DrawRectangle(hx - 26, hy + 18, 52, 6, Fade(BLACK, 0.6f));
      DrawRectangle(hx - 25, hy + 19, (int)(50 * p), 4, RAYWHITE);
    }

    if (invOpen) {
      DrawInventory();
    } else if (!paused) {
      DrawHeldItem(swing);
      DrawHotbar(hotbarSel, hotbarAlpha);
      if (!cfg.creative) DrawStatusBars(hotbarAlpha);
      DrawFPS(10, 10);
      DrawText(TextFormat("%s  LMB mine  RMB place  E inv  C creative(%s)  1-9/wheel  F fly(%s)  Esc menu",
                          cfg.creative ? "CREATIVE" : "SURVIVAL", cfg.creative ? "on" : "off",
                          player.fly ? "on" : "off"),
               10, 40, 18, Fade(DARKGRAY, hotbarAlpha));
    } else {
      bool resume = false;
      if (DrawPauseMenu(borderless, resume)) break;   // Save and quit
      if (resume) { paused = false; DisableCursor(); }
    }
    EndDrawing();

    if (benchLeft > 0) {
      float now = (float)GetTime();
      float frameMs = (now - benchT1) * 1000.0f;
      benchT1 = now;
      benchMeshed += meshedThisFrame;
      if (benchLeft < 260 - benchWarm) benchMaxMs = fmaxf(benchMaxMs, frameMs);
      benchLeft--;
      if (benchLeft == 0) {
        // capture the last frame so -bench can be eyeballed, not just timed
        TakeScreenshot("bench.png");
        break;
      }
    }
  }

  if (bench)
    TraceLog(LOG_INFO, "BENCH worst streaming frame: %.2f ms (chunks=%d meshed=%d)",
             benchMaxMs, (int)chunks.size(), (int)benchMeshed);

  SaveEdits();
  SaveSettings();
  ReturnCraftGrid();   // ingredients left in the crafting grid go back to the player
  for (auto &kv : chunks) UnloadChunk(kv.second);
  chunks.clear();
  if (audioReady) {
    sfxStep.Unload();
    sfxBreak.Unload();
    sfxPlace.Unload();
    sfxJump.Unload();
    CloseAudioDevice();
  }
  UnloadTexture(atlas);
  UnloadShader(fogShader);
  EnableCursor();
  CloseWindow();
  return 0;
}
