# World camera, view/projection and picking in CivilizationV_DX11.exe

Static analysis only (file reads, llvm-objdump, a read-only Ghidra 12.1.4 headless decompile of a
*copy* of the analysed project). The running game was not touched. All VAs are for
`CivilizationV_DX11.exe` at the preferred base 0x400000 (PE timestamp 0x546CD0A8); a table for the DX9 and
Tablet builds is at the end.

Confidence tags:
- **[C]** confirmed by reading the disassembly (and, where noted, the Ghidra decompile).
- **[I]** inferred from code shape and constants; very likely but not proven.
- **[L]** needs a live run to settle.

Names are mine unless they come from a string in the EXE (`GameViewState`, `FCamera.cpp`,
`Civ5App.cpp`, `UIManager`, profiler labels) or from PR #13379 (`InterfaceBuddy`, `g_EventSystemLocalMachine`).

---

## 1. Short answers

| Question | Answer | Confidence |
|---|---|---|
| (a) camera state each frame | One heap object owns it: `GameViewState` = `*(g_Civ5App + 0xC0)`, with `g_Civ5App` = `*(void**)0xCC8E74`. Inside it, at +0x48, is an `FCamera` (FireWorks, `..\FCamera.cpp`): FOV in degrees, aspect, near, far and a 4x4 camera-to-world matrix (rows right / up / back / **position**). Zoom = camera height Z; the zoom *level* is stored as `(20/3)*ln(Z/50)`. Yaw and pitch are not stored as angles: they live in the matrix rows. | [C] layout, [I] zoom-level meaning |
| (b) exact view / projection | Yes. Each 3D frame `GameViewState::UpdateViews` (0x51DF70) writes the **final** matrices to fixed .data globals: projection at `0xDA12D0`, view[i] at `0xDA1310 + 64*i`, view count at `0xD90FA4` (1, or up to 3 when the map wraps and the frustum crosses the seam), inverse views at `0xDA1410`, frustum planes at `0xDA1510 + 0x60*i`. The UI projects world anchors with exactly these (D3DXVec3Project with viewport `0x2DD1FB8`, world `0x2DD1FD0`). D3DX row-major, right-handed (`D3DXMatrixPerspectiveFovRH`, view = inverse of camera matrix). | [C] |
| (c) world / hex / screen conversion | World->hex: `hex = Vec4Transform(((world + off) / scale), M_w2h @ 0xD90FF0)` then cube rounding; hex->world: `Vec4Transform(hex, M_h2w @ 0xD91030)`, then `x*sx - ox`, `y*sy - oy`, z from the height field. Screen: `D3DXVec3Project(world, viewport, proj, view[i], identity)`. Mouse picking (0x6668F0): unproject NDC at z=0 and z=1, intersect with the ground plane, world->hex, round, hex->grid, store the plot at `InterfaceBuddy+0x90` (what `UI.GetMouseOverHex` returns). | [C] pipeline, [L] runtime values of scale and offset |
| Can the DLL read them every frame? | Yes, with no `.text` writes. Everything is in `.data` or the heap, and its root (`g_Civ5App`) is the exact pointer that PR #13379's `LoadCvGameCoreDLL_SetEngineUserInterface` signature already resolves. A per-frame hook on the main thread needs only a heap vptr swap (section 5). | [C] addresses, [L] behaviour |

None of the DLL's interfaces (`ICvDLLUserInterface`, `ICvEngineUtility`, etc.) return camera, matrix or
picking data. They have only setters such as `lookAt(plot, CameraLookAtTypes)`, `centerCamera`,
`releaseLockedCamera` and `lookAtSelectionPlot`, plus the `CITY_ZOOM_*` defines (500/700/800 = camera
heights in world units, offset 20). The FireWorks math headers (FMatrix/FVector) are not in this repository.

---

## 2. Object graph

```
g_Civ5App = *(Civ5App**)0xCC8E74                      [C]  set at 0x602451, cleared at 0x60248E
 +0x0C0  GameViewState*  (0x8D0 bytes, .\App\Civ5App.cpp line 0xF1F, ctor 0x64A860, vtbl 0xA20908)   [C]
 +0x0C4  (0x5C8-byte object, ctor 0x60CA10)
 +0x0C8  (0x724-byte object, ctor 0x523150)
 +0x0D0  InterfaceBuddy*  (PR #13379 name; +4 is the ICvDLLUserInterface the DLL receives)            [C]
 +0x13AC float "Minimum Zoom Level" (config.ini [User Settings], default 11.0)                       [C]
```

`GameViewState` (GVS). Its vtable slot 3 = `GameViewState::Update` 0x7253E0; the string
`GameViewState frame stalled` is in it.

| GVS offset | Contents | Conf. |
|---|---|---|
| +0x044 | `CameraController` (my name; ctor 0x41BE50; first dword = 0x40, no vtable) | [C] |
| +0x048 | `FCamera` object (vptr 0xA9A1B0, ctor 0x8E5170) | [C] |
| +0x050 | FCamera.fovY, **degrees** (`*pi/180` at use) | [C] |
| +0x054 | FCamera.aspect (backbuffer w/h) | [C] |
| +0x058..+0x074 | FCamera: 4 x (velocity, damping) pairs consumed by `FCamera::Update` (vtbl slot 1, 0x8E4D70) | [C] |
| +0x078..+0x0B7 | FCamera camera-to-world 4x4 (D3DX row-major): +0x078 right, +0x088 up, +0x098 back (+Z; view direction is -row2, see getter 0x8E4A10), **+0x0A8 position xyz** | [C] |
| +0x108..+0x114 | FCamera frustum extents (written by the ctor from FOV) | [I] |
| +0x118 / +0x11C | FCamera near / far | [C] |
| +0x120..+0x128 | controller target position xyz (the camera eases toward it; +0x128 = target height) | [C] |
| +0x12C | pan speed (1.0) | [C] |
| +0x134 | zoom base height = 50.0 | [C] |
| +0x138 / +0x13C | min / max zoom level (11.0 / 20.5) | [C] values, [I] meaning |
| +0x140 | current zoom level = `ln(targetZ / 50) * 6.6667` (0x4050C1, 0x55697D, 0x7422DE) | [C] |
| +0x144 | controller clock (accumulated dt) | [C] |
| +0x148 | key-motion bit flags (pan 1/2/4/8, rotate 0x10/0x20, 0x40/0x80) | [C] bits, [I] meaning |
| +0x149 | bit 0 = inertial glide active | [C] |
| +0x150 | wrap sign (+-1) | [C] |
| +0x154..+0x160 | camera clamp box minX, maxX, minY, maxY (copied from the map bounds) | [C] |
| +0x164..+0x1A4 | drag samples / glide velocity / glide timing | [C] |
| +0x1C0 | byte: world view active (Update returns early when 0) | [C] |
| +0x758 | view mode: **1 = 3D map, 2 = Strategic View** | [C] |
| +0x760 / +0x764 | mouse position in NDC | [C] (Ghidra) |
| +0x768 / +0x76C / +0x770 | mouse-over hex (cube x, y as int; z as float) | [C] (Ghidra) |
| +0x790..+0x798 | mouse ground point (world) | [C] (Ghidra) |

`InterfaceBuddy + 0x90`: mouse-over **plot** as `int16 gridX, int16 gridY`. Written by 0x6881D0, called from
the picking function. Read by `UI.GetMouseOverHex` (Lua C function 0x6F5940, which reads
`[[g_Civ5App+0xD0]+0x90]`). Lua scripts pass the result straight to `Map.GetPlot`, so these are grid (plot)
coordinates despite the name. [C]

### FCamera methods (vtable 0xA9A1B0: [0] dtor 0x8E5320, [1] Update(dt) 0x8E4D70, [2] GetViewProj 0x8E4A80)

| VA | What | Conf. |
|---|---|---|
| 0x8E4A80 | `GetViewProj(D3DXMATRIX* view, D3DXMATRIX* proj)`: `view = D3DXMatrixInverse(camWorld)`, `proj = D3DXMatrixPerspectiveFovRH(fov*pi/180, aspect, near, far)`. thiscall, `ret 8`. | [C] |
| 0x8E5030 | `GetFrustumPlanes(D3DXPLANE out[6])`: from proj, inverse, transpose, PlaneTransform | [C] |
| 0x8E4C00 | `LookAt(eye, at, up)`: camWorld = inverse(LookAtRH) | [C] |
| 0x8E50F0 | `LookAt(at, up)` from the current position | [C] |
| 0x8E4940 / 0x8E4960 | `SetFovAspect(fov, aspect)` / `SetNearFar(n, f)` | [C] |
| 0x8E49B0 / 0x8E49D0 | set / get position (+0x60) | [C] |
| 0x8E4A50 | get matrix row i (0 right, 1 up, 2 back, 3 position) | [C] |
| 0x8E4D20 / 0x8E4CC0 | multiply camWorld by RotationX(angle) / Translation | [C] |

### Controller methods (`this` = GVS+0x44)

| VA | What | Conf. |
|---|---|---|
| 0x6FBA80 | `GameViewState::UpdateWorldCamera`: `if (GVS+0x1C0)`, then `controller.Update(dt, t)` (0x741EB0), then **UpdateViews** (0x51DF70). Called from GVS::Update at 0x72591F and once at load (0x4034DC). | [C] |
| 0x741EB0 | controller update: inertial glide, key panning (0x669B60), rotation about the look-at pivot (0x65EE30 = RotationZ about the target, i.e. yaw), map-seam wrap (0x5E6AE0), clamp (0x463510) | [C] |
| 0x556940 | SetPosition(vec3): writes the target and the camera position and recomputes the zoom level | [C] |
| 0x4A3C00 / 0x4728C0 | SetFovAspect / SetNearFar wrappers | [C] |
| 0x6C9770 | RotateX (pitch) wrapper | [C] |
| 0x41E020 | Reset (zero target, speed 1, base 50, zoom limits 11 / 20.5) | [C] |

Projection parameters [C], from the GVS camera init 0x4686C0 and the per-frame 0x6F3ED0 (called at
0x72595C, after the views are built):
- `aspect = backbufferW / backbufferH` (`*(int*)0x301056C` / `*(int*)0x3010568`)
- `fovY_deg = sqrt(K^2 / (1 + aspect^2))` with K = 80 (literal 6400 = K^2 at 0x4B33C2 and 0x6F438A). At init K
  comes from 0x4D3AC0 and the result is floored at 26.5 deg. This is roughly an 80 deg *diagonal* FOV:
  39.2 deg vertical at 16:9, 48 deg at 4:3.
- `near = 50.0` (0xA0BB1C); `far = 0x4855C0()` (not decoded) [L]
- Initial pitch: `RotateX(pi/4)` (0x9E21B4), then position offset (0, -600, 0) (0x4AB8B0). The pitch
  then changes with the PitchChange key bindings. The EXE has the debug string `Camera Pitch: %4.2f`
  but no code reference to it was found. [C] init, [L] how pitch follows zoom

Because yaw, pitch and position are all in the camera matrix, a remote renderer should copy the matrix
(or the final view), not reconstruct it from angles.

---

## 3. The per-frame view globals ("current view" block)

`GameViewState::UpdateViews` 0x51DF70 (mode 1). Disassembly and the Ghidra decompile agree. [C]

```
cam = GVS+0x44
cam.?(0x7758D0)                                 // reads position (unused)
cam.FCamera->GetViewProj(&g_View[0], &g_Proj)   // via 0x73BC00 -> vtbl[2]
cam.FCamera.GetFrustumPlanes(&g_Planes[0])      // 0x6433F0 -> 0x8E5030
g_NumViews = 1
if (g_MapWrapsX == 1)                           // *(int*)0xD90FBC
   for each side whose frustum plane crosses the seam:
      g_View[n] = Translation(+-g_WorldWidth, 0, 0) * g_View[0]; planes; g_NumViews++
for i < g_NumViews: g_InvView[i] = inverse(g_View[i]); g_ViewOffset[i] = g_View[i].row3 - g_View[0].row3
... 0x51B450 (top-down ortho at 0xDA1690 / 0xDA16D0[i], probably the overlay or fog projection [I])
```

| Global | Type | Meaning | Conf. |
|---|---|---|---|
| 0xDA12D0 | D3DXMATRIX | projection (PerspectiveFovRH) | [C] |
| 0xDA1310 + 0x40*i | D3DXMATRIX[4] | view for copy i (i=0 is the real camera; others are the map wrapped by +-W) | [C] |
| 0xDA1410 + 0x40*i | D3DXMATRIX[4] | inverse views | [C] |
| 0xDA1510 + 0x60*i | D3DXPLANE[4][6] | world-space frustum planes | [C] |
| 0xDA1AD0 + 0xC*i | float3[4] | view i translation minus view 0 translation | [C] |
| 0xD90FA4 | uint | number of views this frame (1..3 in 3D; set differently in Strategic View) | [C] |
| 0xD90FBC | int | 1 = map wraps in X | [C] |
| 0xD90FCC/D0/D4 | float | world AABB min x, y, z (z = 0) | [C] |
| 0xD90FD8/DC/E0 | float | world AABB max x, y, z (z = 255) | [C] |
| 0xD90FE4 | float | wrap period = world width (`64*W - 1`, or `-0x41` for one wrap type) | [C] |
| 0xD90FE8 | float | world height (`(64*H - 1) * 0.866`) | [C] |
| 0xD90FF0 | D3DXMATRIX | world(normalized) -> hex (cube) | [C] |
| 0xD91030 | D3DXMATRIX | hex -> world (xy) | [C] |
| 0xD90FC4 | ptr | -> height-field / grid object (static 0xD91270); 0 before a map loads | [C] |
| 0xD91318/1C, 0xD91324/28, 0xD91320 | float | that object's +0xA8/+0xAC scale, +0xB4/+0xB8 offset, +0xB0 ground-height base | [C] offsets, [L] values |
| 0x2DD1FB8 | D3DVIEWPORT9 | {0, 0, W, H, 0.0, 1.0}, W/H from `[[0x2DD1D1C]+0x644/+0x648]` | [C] |
| 0x2DD1FD0 | D3DXMATRIX | identity "world" for projection | [C] |
| 0x301057C / 0x3010584 | ptr | `ID3D11Device*` / immediate `ID3D11DeviceContext*` (out-params of D3D11CreateDevice at 0x8BB6B1) | [C] |

Writers: in mode 1 the block is written by GVS::UpdateViews. Other scenes also write the same
globals with their own cameras (0x4813D0, 0x510140, 0x6E4870, 0x73E520, 0x6A9670: leader screen, other
scene objects). **The block is "whatever is being rendered now", not "the map camera".** Read it right
after UpdateViews (hook in section 5), or check `GVS+0x758 == 1` and compare g_View[0] with
`inverse(FCamera.camWorld)`. [C] that other writers exist, [I] which scenes they are

Main-thread order inside `GameViewState::Update` (0x7253E0) [C]: ... `0x72591F UpdateWorldCamera` (controller
then views), then `0x72595C 0x6F3ED0` (re-applies FOV and aspect for the next frame), then rendering
submission. GVS::Update runs on the EXE's main (UI / Lua) thread. The DLL's game core runs on its own thread.
[C] call order, [I] thread

Strategic View (mode 2) takes another path. Views come from the object at 0x1A0ADA0 (calls 0x46B150 and
0x57C0B0); WorldAnchor uses `0x1A0ADA0->0x4F9E00` instead of D3DXVec3Project; projections are ortho
(D3DXMatrixOrthoOffCenterRH). Not decoded here. [L]

Other cameras that are not the map camera: the cube-map capture at 0x42D790 (six 90 deg faces); the leader
scene with two FCameras at the static 0xDA2780 + 0x6810 / + 0x68E8, set by 0x74EF80; and the FCamera
subclass (vtbl 0xA77748) at +0x15608 of the 0x4495C0 object, whose picking is 0x4A3130.

---

## 4. Conversions (c)

### Grid <-> hex (offset rows <-> axial/cube) [C]
- `ToHexFromGrid` (Lua global, C function 0x5ED240): `hx = gx - (gy >> 1)` (arithmetic shift), `hy = gy`,
  `hz = -(hx + hy)`, then an integer affine transform (0x56D480) built by 0x5A87D0 from the globals at
  0x2DD1CD8. In the normal case this is presumably the identity. [L]
- Back to grid, as done by picking: `gx = hx + (hy >> 1)`, `gy = hy`.

### Hex <-> world [C] structure, [L] runtime constants
- `HexToWorld` (Lua; C function 0x50F370 -> 0x6851C0, `this` = `*(void**)0xD90FC4`):
  `w = D3DXVec4Transform(hex, M_h2w@0xD91030); w.x = w.x*sx - ox; w.y = w.y*sy - oy;`
  `w.z = height(int((w.x+ox)/sx), int((w.y+oy)/sy))` (0x6D8B60), `w.w = 1`.
- World -> hex (picking): `n = ((w.x+ox)/sx, (w.y+oy)/sy, 0, 1); h = Vec4Transform(n, M_w2h@0xD90FF0);`
  then cube rounding (0x5DA590).
- How the map setup (0x535BF0) builds M_w2h: `Scale(1, 0.866, 1) * RotZ(30 deg) * Scale(1/(64*0.866), 1/64, 1)
  * Shear(y -= 0.5x) * Translate(hex origin, which depends on the wrap type) * CubeExpand`
  (0x43F850: z = -x - y). The linear part maps normalized world (64, 0) to hex (1, 0) and (32, 64) to (0, 1).
  So hexes are **64 world units apart in x**, odd rows are shifted +32, and with sy = 0.866 rows are 55.4
  units apart. That matches the world extents (64*W wide, 0.866*64*H tall, centred on 0). [C] matrices,
  [I] sy = 0.866
- **Recommendation: do not hard-code these.** Read M_w2h, M_h2w, sx, sy, ox, oy (0xD91318..0xD91328) once
  per map load (written by 0x535BF0, which ends by setting `*(0xD90FC4) = 0xD91270`), and sample heights from
  the game's own data.

### World <-> screen [C]
- World anchors (0x522910, the `WorldAnchorControl::SetWorldPosition` path):
  `D3DXVec3Project(out, world, &g_Viewport@0x2DD1FB8, &g_Proj@0xDA12D0, &g_View[i], &I@0x2DD1FD0)`, looping
  over all g_NumViews copies and keeping the one that lands on screen (bounds = `[[0x2DD1D1C]+0x644/0x648]`).
  Screen units are backbuffer pixels.
- Screen -> world (mouse picking, `GVS::UpdateMouseOverHex` 0x6668F0, Ghidra-confirmed):
  `cam.GetViewProj(&V, &P)`; `p0 = Unproject((ndcX, ndcY, 0), NULL viewport, P, V)`;
  `p1 = Unproject((ndcX, ndcY, 1))`; `D3DXPlaneIntersectLine(&hit, groundPlane, p0, p1)` with the ground
  plane at height `[0xD91320] * -60.0`; then world -> hex -> round -> grid as above. The results go to
  GVS+0x790 (world point), GVS+0x768 (hex) and `InterfaceBuddy+0x90` (plot). It picks against a flat plane,
  not the terrain mesh. [C]

---

## 5. Proposal: reading the camera from the DLL every frame

### 5.1 Locating everything (no new code bytes)

1. `&g_Civ5App`: reuse PR #13379's verified `LoadCvGameCoreDLL_SetEngineUserInterface`
   (DX11 0x47BA70). The instruction there is `8B 0D <&g_Civ5App>`, so `&g_Civ5App = *(DWORD*)(sig+2)`
   (0xCC8E74). Cross-check: `*(DWORD*)(sig+8) == 0xD0` (the InterfaceBuddy offset the PR already checks).
2. Globals block: signature inside `GameViewState::UpdateViews`, at 0x51DFBD:
   `68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ??` gives &g_Proj (+1) and &g_View (+6). The function
   head 0x51DF70 `55 8B EC 83 E4 F8 81 EC 10 01 00 00 53 55 56 57 8B E9 8B 85 58 07 00 00` fixes the
   GVS view-mode offset (0x758 at +0x14). Viewport and world come from the WorldAnchor site 0x522B70:
   `68 <world> 55 68 <proj> 68 <viewport> 57` (the proj there must equal the first). Both patterns match
   exactly once in all three EXEs (checked, table below).
3. `FCamera::GetViewProj` 0x8E4A80: `8B 54 24 04 56 8B F1 8D 46 30 50 8D 4C 24 0C 51 52 E8` (the rel32 targets
   the D3DXMatrixInverse thunk 0x8A86B6). Check that `*(void**)(GVS+0x48)` is a vtable whose slot 2 is this
   function.

### 5.2 Option A, recommended: heap vptr swap on the map camera (per-frame callback, main thread)

- Copy the 3 entries of `FCamera` vtable 0xA9A1B0 into a DLL-owned array, replace slot 2 with a DLL thunk,
  and write the new table pointer into `*(void**)(GVS+0x48)`. This writes **heap memory only**. No `.text`
  or `.rdata` write, so it respects PR #13379's CEG rule. (The EngineQueueGuard patches EXE code. That is
  a separate, already-taken risk; this needs none of it.)
- Thunk: `__thiscall`, 2 stack arguments, `ret 8`. Call the original 0x8E4A80, then publish view/proj
  (just computed), camWorld (GVS+0x78), fov, aspect, near, far, GVS+0x758, g_Viewport and the
  wrap data (g_WorldWidth 0xD90FE4, g_MapWrapsX). The wrapped copies are `T(+-W) * view`, so the
  server can rebuild them. Bump a sequence counter in a shared-memory block for the 64-bit process.
- It fires on the main thread inside UpdateViews (once per 3D frame) and also from mouse picking
  (0x6668F0) and 0x7117F0, so several times per frame with identical values: deduplicate by content or
  by a frame counter. It does not fire in Strategic View.
- Lifetime: GVS is created per game (Civ5App.cpp line 0xF1F / 0xF3E) and the FCamera dtor resets the vptr
  itself. Re-install on each new `g_Civ5App->+0xC0` value, and never leave a stale table that points into
  an unloaded DLL. That is safe as long as the DLL outlives the GVS (it does in a game session [L]).

### 5.3 Option B: hook-free polling

From any thread, read `*(0xD90FA4)`, `0xDA12D0` and `0xDA1310..`, plus `GVS+0x758` and `GVS+0x48+...`. The matrices
are written once per frame with `rep movsd` and by D3DX calls on the main thread, so a reader on another
thread can see a torn matrix. Read twice and compare, or rebuild view/proj from camWorld + fov/aspect/near/far
(a 4x4 copy plus 4 floats is smaller) and check `inverse(camWorld) == g_View[0]` to tell whether the block
holds the map camera. Good enough for a 64-bit renderer that runs at its own rate and tolerates one frame
of lag.

### 5.4 Option C: in-process D3D11 interception (validate what the GPU actually gets)

- `ID3D11DeviceContext*` = `*(void**)0x3010584` (DX11 build). Swap the **context instance's** vptr to a
  DLL copy of its vtable (heap write again; or patch d3d11.dll's vtable page, which CEG does not cover).
  Wrap `Map` (slot 14) / `Unmap` (15), `UpdateSubresource` (48), `VSSetConstantBuffers` (7) and
  `PSSetConstantBuffers` (16).
- The engine stages shader constants in CPU blocks (0x8CFD50 returns `block+4` and sets a dirty byte; e.g.
  0x643F9B writes `view[i] * proj` into one). They reach the GPU later through Map/WRITE_DISCARD or
  UpdateSubresource.
- Identify the view-projection CB by content. On Unmap or UpdateSubresource of a constant buffer, scan
  16-byte-aligned 64-byte windows for `g_View[i]*g_Proj` **or its transpose** (HLSL column-major packing),
  with relative tolerance 1e-4, using the values from Option A or B for the same frame. Record (buffer
  pointer, offset, transposed?, VS slot).
- Validate: (1) the match holds across camera moves and zooms in one session; (2) project a known world point
  (HexToWorld of the mouse-over plot) with the matched matrix and the viewport, and check it lands on the
  cursor (±1 px) and on that plot's WorldAnchor; (3) take a RenderDoc capture (now installed, 1.47) of the
  same frame and compare the CB contents and the VS input layout; (4) for each wrap copy, expect one
  matching block per g_NumViews.
- Note: the shader bytecode in `.shader` is stripped (no RDEF names; only semantics such as `SV_Position`,
  `InstID`), so names cannot identify the buffer; content matching is the only static-free route.

---

## 6. DX9 and Tablet builds (same code, derived by the same signatures)

| Build | g_Civ5App | GetViewProj | UpdateViews | g_Proj | g_View[0] | g_NumViews | viewport | world | WorldAnchor site |
|---|---|---|---|---|---|---|---|---|---|
| DX11 `CivilizationV_DX11.exe` | 0xCC8E74 | 0x8E4A80 | 0x51DF70 | 0xDA12D0 | 0xDA1310 | 0xD90FA4 | 0x2DD1FB8 | 0x2DD1FD0 | 0x522B70 |
| DX9 `CivilizationV.exe` | 0xCB8CF4 | 0x96BA80 | 0x453BE0 | 0xD91120 | 0xD91160 | 0xD80DF4 | 0x2DC1DB8 | 0x2DC1DD0 | 0x57B630 |
| Tablet `CivilizationV_Tablet.exe` | 0xCCA8F0 | 0x8E6F70 | 0x73EBA0 | 0xDA2D50 | 0xDA2D90 | 0xD92A24 | 0x2DD3FA0 | 0x2DD3FB8 | 0x558A60 |

The g_Civ5App sites match PR #13379's columns (0x47BA70 / 0x72B7F0 / 0x4CB690). Every pattern matched
exactly once per EXE. The struct offsets (GVS+0x44/+0x48/+0x758, FCamera fields) are assumed identical
across builds because the signatures that encode them matched byte for byte. [C] for the signatures,
[I] for the offsets not covered by them.

---

## 7. Open questions (need Ghidra time or a live run)

1. **[L]** The runtime values of the grid object's scale and offset (0xD91318..0xD91328, 0xD91320), the 0x2DD1CD8
   hex-orientation transform (identity?), and `far` (0x4855C0). Read them once after a map loads.
2. **[L]** Is g_View/g_Proj at frame end always the map camera in mode 1, or do UI 3D scenes (leader screen,
   unit preview, city view) overwrite it mid-frame before submission? Watch which writers run per frame.
3. **[L]** Which thread runs GVS::Update (assumed the main/UI thread)? Is the DLL module guaranteed to
   outlive the GVS (vptr swap safety on exit to the main menu)?
4. **Ghidra** Strategic View (mode 2): decode 0x1A0ADA0 (`StrategicView` / `StrategicViewSystem`) views,
   its ortho projection and WorldAnchor path 0x4F9E00 if the server must render it too.
5. **Ghidra** How pitch follows zoom (the PitchChange key bindings, `Camera Pitch: %4.2f`, 0x669B60 /
   0x7117F0). Not needed if the matrix is copied, but needed for camera *control* from the server side.
6. **Ghidra** The renderer's CB upload path (0x8CFD50 blocks -> Map). Find the frame-constant block that
   receives `view*proj` (0x643B60, 0x4FB320, 0x5184xx) so Option C can target one buffer instead of
   scanning.
7. **[L]** Camera-relative effects: does the terrain/water use g_ViewOffset (0xDA1AD0) for the wrap copies,
   and is there TAA/jitter or a stereo offset on g_Proj (the EXE has stereo convergence settings)?
   Compare g_Proj with the CB contents in RenderDoc.

## Method notes

- Disassembly: `llvm-objdump -d -M intel` of `.text` (0x401000-0x9D5D30); IAT parsed by hand. The
  D3DX9 thunks are at 0x8A86A4..0x8A8734 (LookAtRH 0x8A86E0, PerspectiveFovRH 0x8A871C, Inverse 0x8A86B6,
  Multiply 0x8A868C, Vec3Project 0x8A8734, Vec3Unproject 0x8A86AA, PlaneIntersectLine 0x8A86A4,
  Vec4Transform 0x8A8692). Every camera site was found from the callers of those thunks.
- The Lua C functions were found from `pushcclosure`/`setfield` pairs (0x92396A / 0x923928); notable ones:
  `UI.LookAt` 0x42F310, `UI.CenterCamera` 0x4333D0, `UI.IsCameraMoving` 0x5D6940, `UI.GetMouseOverHex`
  0x6F5940, `UI.GetStraightZoom` 0x6A6890, `HexToWorld` 0x50F370, `ToHexFromGrid` 0x5ED240;
  `Events.CameraViewChanged` / `CameraProjectionChanged` accessors 0x408220 / 0x614790. No Lua getter for
  the camera position, zoom or matrices exists.
- Ghidra: read-only headless decompile of a copy of the `civ5` project
  (0x51DF70, 0x4686C0, 0x6668F0, 0x6881D0, 0x56D480, 0x5A87D0 and others). It matched the disassembly reading.
- PR #13379 names no camera, view or renderer symbols. Its value here is the `g_Civ5App` anchor and the rule
  against writing to `.text` and `.rdata` (CEG).
- The old, commented-out NOP patch at 0x51E031 in `CvDllGame.cpp` (removed by PR #13379) sits inside
  UpdateViews' wrap-copy test. Someone had looked at this function before.
