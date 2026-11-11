# Shell architecture: Gate 2 (asset reuse)

Status: **DONE** (2026-11-10 01:40). Question: can the 64-bit server reuse the GPU assets that the 32-bit EXE has
already loaded (textures, shaders, meshes) instead of parsing the asset files itself?

Install restored and verified after the last run: `MODS\(1) Community Patch\CvGameCore_Expansion2.dll` SHA-256
`262AEE88C5312C4BD4E899DA1D8B413C000BC996FA57ABFE8D04B87FD37DF16E`, config.ini `7b53b17c3935702d...`
(`disable_systems.py restore` OK; B4 was not used), GraphicsSettingsDX11.ini `F1D9CF01...` (unchanged),
`crashlogs\shell.helper` deleted, the loader's `crashlogs\shell.log` moved to `gate2/results/shell_loader.log`,
front-end Lua restored (`vp_modgame.py restore-lua` after each run). Three launches (`civ_bg.py launch --park bottom`,
huge t350 save, all systems on), no turn ended, no crash, no new dump. Saves untouched.

## Verdict

| Asset | Tappable? | Measured | Better route |
|---|---|---|---|
| **Textures** | **Yes**, two ways, both verified live | (a) copy into a shared texture: works for every format the game uses (BC1/BC3 with full mip chains, A8, RGBA16F, cube arrays). It costs exactly one extra copy of the texture in VRAM (+1,408 KB for a 1,365 KB texture, +5,504 KB for 5,461 KB) and 0.07-0.26 ms of render-thread CPU per texture. (b) add the shared flags when the game creates the texture: zero copy, zero extra VRAM, the game renders unchanged, and the server's image is byte-identical to (a) | **Parse** for static textures (FPK = an uncompressed pack of plain DDS files: 10,004 of them, 1.0 GB; parsing gives file names for free). **Tap** only what the GPU generates (the terrain bake caches). |
| **Shaders** | No need | All 1,417 DXBC blobs sit in plain form in the EXE's own `.shader` section (6.99 MB). The game creates them at app start, before any helper DLL loads (0 shaders were created during the save load). The running game uses the shader-model-5 set: 128 VS, 362 PS, 2 HS, 1 DS. Read from game memory, its bytes equal the file's bytes for 128 of 128 VS. **All 1,417 create in a 64-bit D3D11 device**; 379 of 380 VS take an input layout built from their own signature | **Parse the EXE file** (done: `extract_shaders.py`). The real work is the constant-buffer contents (no RDEF chunk, so no names), and that work is the same for tap and parse. |
| **Buffers / meshes** | **No** (as a product) | D3D11 documents sharing only for 2D textures. On this machine (RTX 4080, Win 11 26100), legacy `MISC_SHARED` DEFAULT vertex/index buffers do cross x86→x64 with intact contents; NT-handle, DYNAMIC and IMMUTABLE buffers do not. Most of the game's mesh buffers are IMMUTABLE or DYNAMIC. A buffer is useless without its draw metadata (layout, ranges, bones, materials), and that lives in EXE structures (Granny model instances) | **Terrain:** the DX11 terrain has *no* vertex buffer. It is a grid built from `SV_VertexID` plus a height texture plus bicubic tessellation. Take the CPU height field from EXE memory, or re-bake. **Models:** Granny `.gr2` (12,412 files, 566 MB) is the open problem. Readback through a staging buffer is portable but needs the metadata reverse-engineered. Parsing needs a 64-bit Granny reader. |

**Does tapping beat parsing? Not as a strategy.** It wins only for things the EXE *computes* (GPU terrain caches, the
height field, possibly skinned poses). For things the EXE merely *loads* (DDS, DXBC, Granny), parsing is cheaper, keeps
names, and above all does not need the EXE to keep loading and holding the world assets. Dropping those assets is what
the shell exists for: Gate 1's next step stubs Terrain and Unit, and from then on the tap would have nothing to read.

## How it was measured

Tools are in `gate2/`; build with `build.cmd`, VS 2022, no downloads:

- `shell_tap.dll` (x86) is loaded by the game DLL's `crashlogs\shell.helper` loader. It finds the device at the
  first Present and step-asides the swap-chain slots during CreateSwapChain, as `shell_composite.dll` does. It then
  patches the device vtable: CreateTexture2D, CreateBuffer, CreateInputLayout and the VS/PS/GS/HS/DS creators. On
  Win 11 that vtable is a shared read-only table in d3d11.dll, patched with VirtualProtect.
  - Every call is logged with its description and a stack-scanned call chain, printed as Ghidra VAs.
  - It keeps a reference to every shader-resource texture of 16 KB or more.
  - `tap.copy` triggers option (a) and the `inject = W H FMT N` setting triggers option (b).
  - Exports are NT handles named `Local\VPTap_<seq>` or `Local\VPTapB_<seq>` (legacy handles for cubes), listed
    in `tap_manifest.txt`.
- `tap_server.exe` (x64) has three commands:
  - `dump` opens by name or legacy handle and acquires the keyed mutex. It renders every array slice through a pixel
    shader into an RGBA32F target, so the sampler decodes BC formats and DirectXTex is not needed. It writes PNGs
    (WIC) and per-channel ranges.
  - `shaders` creates every blob on the 64-bit device, runs D3DReflect and builds an input layout from each VS
    signature.
  - `matrix` is the sharing matrix below.
- `tap_matrix32.exe` (x86) is the sharing-matrix producer. `tap_host32.exe` (x86) is an offline stand-in for the
  game: a device on a window that is never shown. It ran the whole pipeline before the live runs: inject plus four
  copies (BC1 immutable, BC3 array, RGBA16, cube). The x64 dump decoded the BC gradients correctly.
- `extract_shaders.py` (blobs, disassembly via the system d3dcompiler_47, index CSV), `read_vs_table.py` (read-only
  ReadProcessMemory of the renderer's VS table), `classify_tap_log.py` (owner per texture/buffer), `fpk_survey.py`
  (FPK index parser and asset census), `ghidra/` (shtable.py: shader create sites from the objdump listing;
  HandleRefs.java, FuncOf.java: headless Ghidra queries).

Live runs:
1. A plain tap. It logged the load and copied 10 textures, which were dumped. Logs: `results/shell_tap_run1.log`,
   `results/dump1.txt`.
2. Inject with a wrong format code (77 instead of 76 for BC3_TYPELESS), so nothing was injected. Discarded.
3. `inject = 1024 1024 76 2`. Logs: `results/shell_tap_run3_inject_events.log`, `results/dump_inject.txt`.

PNGs and screenshots are game art, so they stay outside the repo in `C:\Users\Art\Documents\shell-gates-live\gate2\`:
`png/`, `png_inject/`, `run1_t350.png` and `run3_inject_t350.png`.

## 1. Textures

### What the game creates (helper loaded at the main menu, then the huge t350 save loaded)

- **Created after the helper loaded:** 1,508 textures (422 MB) and 1,410 buffers (106 MB), but **0 shaders and
  0 input layouts**.
- **Not seen:** whatever the EXE created before the game DLL loaded. That includes the renderer's shaders and the
  terrain texture pools that `TerrainSystem::Startup` allocates (the "Terrain 64/128/256/512 size texture" caches).
- **Owners** (`results/run1_owners.txt`), from the call chains mapped to functions:

| Owner (call chain) | Textures | What |
|---|---|---|
| Terrain loader `0x74a9e0` (`Terrain.cpp`) | 70 × 1024² A8, 11 mips, IMMUTABLE+init, 93.3 MB | terrain masks: mostly empty alpha with shapes (seq 238: a strip, 270: blobs) |
| `CreateTerrain 0x709b50` → TerrainRenderer `0x5bcef0` → `0x678290` ("Terrain Render/Height/Spec/Shadow Temp") | 30 render targets (RGBA16F/RGBA8/R32G32 512²…16², GENERATE_MIPS), 19.8 MB | the GPU bake's temporaries; seq 308 holds a freshly baked terrain tile (forest/grass) |
| Generic asset loader `0x8bbd60` (FFileIO + trigger system) | 1,401 textures, 305 MB: BC3/BC1 with full chains (IMMUTABLE+init), RGBA8 render targets, UI images | includes the terrain material textures (seq 154 = grass diffuse with height or specular in alpha, 1024² BC3) and the 2048² BC3 detail maps (155, 181) |
| Leader head (`CvLeaderHeadTextureManager.cpp`) | 7 × 128² BC1 cube **arrays** (54 slices) | environment maps |

The engine attaches **no debug names** (`WKPDID_D3DDebugObjectName` is empty on all 785 kept textures). A tap
therefore sees anonymous resources, and their identity has to come from call stacks or from hooking the asset loader
for file names. Parsing the files gets names for free.

### Option (a): shareable copy (live, run 1)

The copy is a texture with the same desc, DEFAULT usage, SRV binding and `SHARED_NTHANDLE|SHARED_KEYEDMUTEX`
(legacy `SHARED` for cubes), filled by `CopyResource` on the render thread inside Present. Source formats are kept as
they are (typeless BC3, A8 and so on).

| seq | Texture | Copy VRAM (process usage after copy) | Server |
|---|---|---|---|
| 238, 239, 270 | terrain mask 1024² A8 ×11 mips (1,365 KB) | +1,408 KB each | opened, sampled, PNG |
| 154 | terrain grass 1024² BC3 ×11 (1,365 KB) | +1,408 KB | PNG = grass texture; RGB 0..1, alpha 0.11..0.95 |
| 155, 181 | 2048² BC3 ×12 (5,461 KB) | +5,504 KB each | PNG = grey detail maps |
| 308 | bake temp 512² RGBA16F ×10 (2,730 KB) | +2,752 KB | PNG = a baked terrain tile |
| 311, 335 | bake temp 512² RGBA8 | +1,408 / +2,816 KB | all zero at that moment |
| 1194 | leader-head 128² BC1 cube array ×54 (576 KB) | +768 KB | legacy handle, 3 slices dumped |

- **VRAM:** the copy costs its own size, rounded up to 64 KB (9 of 10 copies). The outlier is seq 335, at
  +2,816 KB, probably because another allocation fell into the same measuring window. The allocation is committed
  lazily (+0 right after CreateTexture2D, counted after the copy).
- **Opening it in the server costs +0 KB** of server VRAM: it is the same allocation.
- **CPU per copy:** 0.07-0.26 ms on the game's render thread (Present hook). The GPU copy is asynchronous.
- A keyed-mutex copy is handed over once (key 0→1). Later readers acquire key 0.

### Option (b): shared flags at creation (live, run 3)

`inject = 1024 1024 76 2` matched the first two 1024² BC3 textures of the load: seq 154 (the same grass texture as in
run 1, because the load order is deterministic) and seq 158. The hook changes IMMUTABLE to DEFAULT and adds
`SHARED|SHARED_NTHANDLE` with no keyed mutex, so the game needs no synchronisation. It also drops GENERATE_MIPS and
keeps the game's own initial data.

- The game loaded and rendered unchanged (`run3_inject_t350.png` vs `run1_t350.png`).
- The server opened `Local\VPTapB_154` and `_158` directly. Its PNG of 154 is **byte-identical** to option (a)'s PNG
  of the same texture (SHA-256 `E01B4D9A…` for both).
- Extra VRAM 0, copy 0.
- Limits:
  - It works only for textures created after the helper loads. Main-menu and startup textures (the terrain pools)
    are missed, because the loader runs at DllGetGameContext.
  - IMMUTABLE has to become DEFAULT.
  - Cubes need legacy handles.
  - The server's reference keeps a texture alive after the game frees it, so the protocol needs release
    notifications.

### Sharing matrix (offline, x86 producer → x64 consumer; `results/matrix_*.txt`)

Adapter: RTX 4080, FL 11_0, `ExtendedResourceSharing = 1`, `SharedResourceTier = 3` (both sides).

| Resource | NT handle + keyed mutex | NT handle, no mutex | legacy `MISC_SHARED` |
|---|---|---|---|
| BGRA8, 1 mip | ✅ | | |
| BC1 / BC3 256², full 9-mip chain | ✅ contents match | ✅ (BC1) | ✅ (BC1) |
| RGBA8 / BC3 array ×4, full chain | ✅ | | |
| RGBA16F, RGBA16 UNORM, R16F, A8, R32F | ✅ | | |
| BC1 **cube** (6 faces, 7 mips) | ❌ create E_INVALIDARG | | ✅ |
| **IMMUTABLE** BC1 | ❌ create E_INVALIDARG | ❌ create E_INVALIDARG | creates, but **open fails** |
| VB 1 MB DEFAULT | ❌ create E_INVALIDARG | ❌ | ✅ **contents match** |
| IB 256 KB DEFAULT | | | ✅ contents match |
| VB IMMUTABLE | | | creates, open fails |
| VB DYNAMIC | | | ❌ create E_INVALIDARG |
| raw SRV buffer | | ❌ | |

## 2. Shaders

- **Where they are:** not in `.fxsib` files. **1,417 DXBC blobs (6,993,892 bytes) are embedded in the EXE's `.shader`
  section**, raw at file offset 0x8B6A00, VA 0x619B000.
  - The renderer's init `0x4e5c20` (1,277 creates) and `0x80cd10` (136) create them by address, through
    `D3D11Renderer::CreateVertexShader 0x8bbc40` (blob, size, vertex-format mask) and
    `CreatePixelShader 0x8bbbf0`; `0x8bbcc0` creates HS and `0x8bbd10` DS.
  - The 16-bit handles are stored in 493 globals at 0x2de056c...
  - They come in three sets, selected by the shader level `[0x2e49edc]`: 4_0 (126 VS, 320 PS), 4_1 (126/352) and
    5_0 (128/362, plus 2 HS and 1 DS).
- **Live check:** the running game has level 10, and its renderer counters are VS 128, PS 362, HS 2, DS 1 (the
  SM5 set).
  - All 128 VS bytecode pointers in the renderer's table (`0x3006f68[i]`, sizes `0x3005f68[i]`, count
    renderer+0x458) point into the EXE image.
  - **Live bytes == file bytes** for 128 of 128 (`read_vs_table.py`, read-only).
  - The device hooks saw **0** shader or input-layout creations during the load: everything is created at app start.
- **64-bit load:** `tap_server.exe shaders` gives **1,417 of 1,417 created** (VS/PS/HS/DS) on the x64 device and
  `D3DReflect` S_OK on all.
  - Reflection reports **0 constant buffers and 0 bound resources everywhere**. The blobs are stripped to
    ISGN+OSGN+SHDR/SHEX, with no RDEF and no STAT chunk.
  - Resource slots and cbuffer sizes come from the disassembly instead (`dcl_*`, in `results/shader_index.csv`).
  - Input layouts built from each VS's own input signature create for 379 of 380. The exception is the terrain VS,
    which has only `SV_VertexID` and needs no layout.
- **The terrain shaders** (SM5; VAs at ImageBase 0x400000; signatures chained VS→HS→DS→PS):

| Stage | Blob VA | Signature / resources |
|---|---|---|
| VS | 0x067d6490 (772 B) | input `SV_VertexID` only: `x = id & 255, y = id >> 8`, samples **t0 = height texture** (SampleLevel), cb0[7], cb1[23]; outputs one height per control point |
| HS | 0x067f98f0 / 0x067f9cb8 | patch-constant tessellation factors, cb1[15] / cb1[16] |
| DS | 0x067fa080 | `domain_quad`, 16 control points (bicubic patch), cb0[7], cb1[17]; outputs `SV_POSITION, NORMAL, TEXCOORD0-3` |
| PS | 0x067e6e58 (2,704 B) / 0x067e8d68 | same input signature as the DS output; **t0-t5 2D, t6-t7 3D, t8-t9 2D, 10 samplers**, cb1[35], cb2[2] |
| VS (no tessellation) | 0x067d6798 | `POSITION.xy, NORMAL, TEXCOORD` + t0 height, same outputs |
| water PS | 0x067ebe78 / 0x067ed018 | same inputs, 2 **cube** textures + 3D noise |

  - The four stages are tied together in the renderer's material 73 (struct `0x5d1e5c0 + 73×0x520`, passes
    `{VS c8c, PS cb8, HS cf4, DS d00}`, `{…, PS cbc, …}`, …), which matches the EXE strings
    `BicubicTerrainTessellation` and `TerrainTessellation`.
  - Identification is static: the signatures, the only HS/DS in the EXE, and the material table. **RenderDoc was not
    run.** It would have to inject at process start, which `civ_bg.py` does not do, and the rules forbid running it
    alongside the helper.

## 3. Meshes

- **Terrain "pieces" are not meshes.** `Assets\terrain\pieces` holds 300 pieces as loose files (975 MB):
  - for each piece, `_H.dds` (128² 16-bit RGBA height), `_T1.dds` (128² RGBA8), `_H_BLEND`/`_T_BLEND.dds` and a
    `.csv` 4×4 hex-pattern mask;
  - plus `Resource\Common\TerrainPiecesLibrary.fxsb` (26.3 MB binary, format not decoded), which the EXE loads
    first ("Terrain Pieces Library binary file ... Loading flat files by default").
  - `HexPatternMatcher` matches the pieces on the CPU, and `BlendTerrainHeightmap` blends them into the static
    height field **0xD91270** (16×16-tile ushort heights, the 9.1 MB `Terrain.h:79` block, filled by
    `CreateTerrain`'s first call 0x47e350; see `stubs/terrain.md`).
  - On DX11 the GPU draws that height field as a **height texture + SV_VertexID grid + bicubic tessellation**: there is
    no terrain vertex buffer to share.
  - The terrain's buffers at load are the TerrainRenderer's DYNAMIC streaming VBs (3.76 + 1.88 MB) and IB (0.94 MB),
    one IMMUTABLE quad, and a static IMMUTABLE VB of 881,600 B plus IB of 329,760 B. That static pair is built by
    `CreateTerrain → 0x6d51d0 → 0x560b40 → 0x6b1580`; its owner is not named (coast or ice?).
- **Buffers cannot be shared as such.** The D3D11 docs (`D3D11_RESOURCE_MISC_FLAG`, `D3D11_RESOURCE_MISC_SHARED`) state
  that the only resources that can be shared are 2D non-mipmapped textures. That sentence predates Win 8 sharing:
  measured here, mipmapped, array and BC textures share fine.
  - Buffers share *only* through legacy `MISC_SHARED` with DEFAULT usage (matrix above). That is undocumented and
    driver-dependent (tested on NVIDIA only), and IMMUTABLE or DYNAMIC buffers cannot use it at all.
  - The game's model buffers at load are IMMUTABLE VB/IB 16.1 + 6.8 MB, DEFAULT VB/IB 29.7 + 3.4 MB and
    DEFAULT raw SRV|UAV buffers 41.9 MB (`ALLOW_RAW_VIEWS`, 141 of them).
- **Alternatives, sized:**
  - **CPU readback at load.** The helper copies each IMMUTABLE/DEFAULT buffer to a staging buffer, maps it and copies
    it into a named section in chunks of a few MB, so the 32-bit address-space cost is transient.
    - Volume: ~100 MB once per load on this save.
    - Time: roughly 10-30 ms of PCIe plus map stalls, spread over frames.
    - The bytes are useless without the draw metadata (layouts, strides, ranges, bone palettes, material binding).
      That metadata sits in the EXE's Granny model instances, so this route is a reverse-engineering task.
  - **Parse the files.**
    - FPK is trivial: `"FPK_"` magic, a name table with names padded to 4 bytes, uncompressed payloads.
      `fpk_survey.py` parses all 147 archives.
    - Models are **Granny 2 `.gr2`**: 12,412 files, 566 MB, magic `29de6cc0`. The format is proprietary, Granny is
      statically linked into the 32-bit EXE (`granny_file.cpp`), and no 64-bit Granny runtime is on the machine.
    - Terrain needs re-implementing the pattern matcher and blend (or decoding the fxsb), or **reading the finished
      height field 0xD91270 from EXE memory** (a 9 MB memcpy into shared memory per terrain change).

## 4. Conclusion and effort (Opus-session-hours)

Tappable:
- **textures:** verified live both ways, and every format the game uses shares;
- **CPU-side computed state:** the height field and, by extension, any EXE structure the DLL can read.

Not tappable:
- **buffers:** only an undocumented, driver-specific path, with no metadata;
- **Granny model instances:** CPU-side, 32-bit and proprietary;
- **shaders:** there is nothing to tap, because they never pass through a hook point after start-up, and the file
  already has them.

| Work item | Tap layer | Parse |
|---|---|---|
| Shaders: blobs | 0 (useless) | **done** (1 h) |
| Shaders: cbuffer contents per material family (terrain alone: cb0[7], cb1[23], cb2[2], 10 textures) | same either way: 6-12 h per family, 40-80 h for the world set | |
| Static textures (DDS) | 12-20 h: production hook layer (MinHook or an EXE-side hook on the loader `0x8bbd60` for names), creation-flag injection, release notifications, manifest in shared memory, server cache | **3-5 h** (FPK index + DDS→D3D11 loader; formats map 1:1) |
| GPU terrain caches (bake) | 8-15 h: copy on update; the pools are created at app start, so (b) needs earlier injection | regenerate in the server: part of the terrain renderer port |
| Terrain geometry | **3-6 h** (height field 0xD91270 + plot data via the DLL into shared memory) | 25-50 h (pattern matcher + blend, or fxsb decoding) |
| Model meshes | 30-60 h (staging readback + Granny instance metadata RE) | 25-60 h, **high risk** (64-bit GR2 reader for Civ 5's Granny version) |

**Recommendation.**
- Shaders: parse from the EXE (done).
- Static textures: parse FPK/DDS.
- Terrain: a *state* tap of the height field (cheap, exact), with the server running its own bake using the extracted
  bake shaders.
- Models: run a separate 4-6 h spike before choosing between readback and parsing: try an open GR2 reader (for
  example LSLib's) on five Civ 5 unit `.gr2` files. If that fails, the readback route is the fallback, and its
  metadata RE is the long pole.

A general asset-tap layer is **not worth building**. It would duplicate what the files already hold, cost VRAM or
creation-time hooks, and depend on the EXE keeping alive the very world systems the shell is meant to drop.

## Files

`experiments/shell-gates/gate2/`:
- `src/` holds `shell_tap.cpp`, `tap_vtable.c`, `tap_server.cpp`, `tap_matrix32.cpp`, `tap_host32.cpp` and
  `tap_common.h`.
- `build.cmd`.
- The scripts `extract_shaders.py`, `read_vs_table.py`, `classify_tap_log.py` and `fpk_survey.py`.
- `ghidra/`.
- `results/` holds the matrix, the tap logs and manifests, the dump outputs, `shader_index.csv` (one row per blob:
  VA, model, signatures, textures, cbuffers, create site, handle), `load64.csv`, `shader_create_sites.json`,
  `chain_functions.txt`, `run1_owners.txt`, `fpk_archives.csv` and `live_shader_tables.txt`.

Outside the repo (game data), in `C:\Users\Art\Documents\shell-gates-live\gate2\`:
- `shaders/blobs` (1,417 `.dxbc`) and `shaders/asm`;
- the PNG dumps;
- the full logs of all three runs.
