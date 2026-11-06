# Changes to the decompiled code

Changes the 3DS port needs in `src/`, `include/` or `libs/`. They live only on the private
`3ds-port` branch, and each one keeps the Wii build bit-identical (verified against
`build.sha1` after every change).

## Pending

- **Mii heads on 3DS**: `MiiFacePartsHolder::drawExtra()` skips the Wii RFL GX material setup
  and draws each head through `RFLDrawOpa`. The 3DS implementation reads the system
  `CFL_Res.dat` component archive through the host FS bridge and submits the real face, hair,
  forehead, nose, nose-line, beard and glasses model sections as native Citro3D triangles.
  The face-provided hair, nose/glasses and goatee anchors are applied before the shared Wii
  scale; the selected `MiiData` controls the face, hair, nose, beard and glasses indices,
  sizes, positions, hair variant, skin colour and hair colour. Eyes, eyebrows, mouth, mole,
  makeup and wrinkles are decoded into a transparent CFL face canvas and drawn on the
  corresponding face-canvas model. The first implementation covers the normal expression
  with CPU ETC1/ETC1A4 decoding; expression-specific masks and exact dual-colour CFL
  compositing remain approximated. The shared model origin is lowered on 3DS to match the
  Wii file-select placement. The CPU decoder's 128x128 temporary buffer and 64x64 canvas use static
  storage because the 3DS thread stack cannot hold the former; this avoids a stack fault while
  composing the decals. The ETC1A4 decoder keeps its 8-byte alpha plane separate from the
  following 8-byte ETC1 colour block and indexes alpha relative to each block. Eye and eyebrow
  placement reads their full 32-bit packed MiiData fields rather than truncating them to 16 bits.
  All fields after the eye pair use the packed MiiData offsets from libctru, including nose,
  mouth, mustache, beard, glasses and mole.

## Temporary (remove once the sound archive is fully converted)

- `src/JSystem/JAudio2/JAUSectionHeap.cpp` `isWaveLoaded`: returns true for a wave bank that
  does not exist, so the game does not wait forever for the waves the partial sound archive
  leaves out.

## Done

- **Citro3D values and state of the hand-written 3DS draws match the Wii** (2026-10-22, Claude). GX values
  now go through one table (`platform/3ds/include/GXToCitro3D.hpp`, the GX register translation's
  tables): depth comparisons reversed, blend equation/factors, cull (`GX_CULL_BACK` is
  `GPU_CULL_FRONT_CCW`, as the native renderer).
  - Depth: `DrawUtil.cpp` shadow volumes (`GX_GEQUAL`), `ClipFieldFillDraw.cpp` (`GX_ALWAYS`), `KirairaChain.cpp`
    and `ElectricRailMoving.cpp` (`GX_LEQUAL`, world-space view mode), `MarioActorSpecialDraw.cpp` (`GX_GREATER`),
    `WaterRender3DS.hpp`; `DrawUtil.cpp` `clearZBuffer` writes the far plane at z = 0 (z = 1 was clipped).
  - Blend: `MarioSearchLight.cpp`, `StarPointerBlur.cpp`, `MarioActorSpecialDraw.cpp`, `ImageEffectLocalUtil.cpp`,
    `ShadowVolumeDrawer.cpp` (add, then reverse subtract), `VolumeModelDrawer.cpp`, `MarioShadow.cpp`.
  - Cull: `SnowFloor.cpp`, `SnowFloorTile.cpp`, `OceanRingPipeOutside.cpp`, `AstroDomeOrbit.cpp`,
    `ShadowSurfaceDrawer.cpp`, `BigBubbleGoalArea.cpp`.
  - `ClipFieldFillDraw.cpp`: the Wii's pixel projection and fill blend mode; `WhirlPoolAccelerator.cpp` and
    `SpinDriverPathDrawer.cpp`: their `GX_GREATER 0` alpha compare.
  - `ElectricRail.cpp` `ElectricRailShadowDrawer::drawShape` sends only vertices (the pass state is
    `ShadowVolumeDrawer::draw`'s, as on the Wii).
  - `DirectDraw.cpp`: every draw takes the setters' state (blend, alpha compare, depth, scissor); Z is
    written only through an enabled Z test (as `GXSetZMode`); `drawJ2DPictureTriangles` blends further
    textures as J2DPicture's stages (previous x (1 - k) + texture x k, then the vertex colour).
  - Particles (`JPABaseShape.cpp`): `setGX` sets blend, Z mode and alpha compare; prm/env TEV colours
    and the shape's colour/alpha modes are drawn by `TDDraw::drawParticleTriangles`. The billboards
    passed one colour where six were read.

- **3DS frame end runs the native renderer's per-frame work** (`src/Game/System/MainLoopFramework.cpp`):
  `endRender` calls `rvl3DSEndFrame()` before `hostGfxEndFrame`. The GX parser that used to run it at
  the frame copy is no longer entered, so the native and texture caches never aged and the per-frame
  log never ran. (2026-10-22, Claude)
- **Layouts, J2D and 2D particles use their own projection and matrices on 3DS** (2026-10-22, Claude):
  - `src/Game/Util/DrawUtil.cpp` `setupDrawForNW4RLayout` loads the layout's orthographic projection
    (the Wii's `GXSetProjection`), culling and depth test off;
  - `src/nw4r/lyt/lyt_pane.cpp` `Pane::LoadMtx` loads the pane matrix as the DirectDraw view matrix
    (the Wii's `GXLoadPosMtxImm`);
  - `src/nw4r/lyt/lyt_material.cpp` `Material::SetupGX` sets the material's alpha compare and blend mode;
  - `src/JSystem/J2DGraph/J2DOrthoGraph.cpp` `setPort`/`setLookat`, `J2DGrafContext.cpp` `fillBox` and
    `J2DPicture.cpp` draw with the graph's projection and their own matrix (DirectDraw view mode 3);
  - `src/JSystem/JParticle/JPAEmitterManager.cpp` (+ `JPAEmitterManager.hpp` `JPASetDirectProjection`)
    and `src/Game/Effect/ParticleDrawExecutor.cpp` `draw2D`: the 2D groups keep the orthographic
    projection, and `closeDirect` no longer runs after every emitter (it put the camera view back, so
    every emitter but the first was transformed twice);
  - `src/Game/Util/DirectDraw.cpp`: view mode 3 keeps the caller's matrices, `closeDirect` resets
    only the 3D modes, `setDirectScissor` stores x0, y0, x1, y1 as `HostGfxState` expects.
  Before, the layout quads used whatever camera projection and view an unrelated draw had left: a
  pane at z = 0 with an identity view got clip w = 0 (the "native UI quad rejected" log lines).

- **3DS startup no longer initializes the Wii GX FIFO** (`src/Game/System/GameSystem.cpp`):
  `GameSystem::initGX` returns before allocating the Wii FIFO or calling `GXInit` on 3DS.
  The `GXInit.c` translation unit remains in the target only because it owns the shared
  `__GXData` compatibility state still consumed by the parser and SDK texture/state helpers;
  no 3DS startup path enters the Wii FIFO initialization.
- **3DS draw-sync recovery no longer re-enters the Wii FIFO** (`src/Game/System/DrawSyncManager.cpp`,
  `src/Game/System/MainLoopFramework.cpp`): reset handling, draw-done waits and the GX abort alarm
  are native-target no-ops. The Wii callback, alarm, FIFO reset and abort command sequence remain
  unchanged for the original build; frame presentation still uses the existing native host end-frame
  path.
- **3DS frame clear and presentation use the host render target** (`platform/3ds/host/host.h`,
  `platform/3ds/host/gfx.c`, `src/Game/System/MainLoopFramework.cpp`): clear operations begin and
  clear the Citro3D target directly, while XFB exchange/copy is skipped and `hostGfxEndFrame`
  presents the target at `endRender`. This removes the Wii EFB-copy protocol from the 3DS frame
  lifecycle while retaining GX FIFO processing for legacy command streams that have not yet been
  migrated.
- **J3D display-list drawing is explicitly disabled on 3DS** (`src/JSystem/J3DGraphBase/J3DShape.cpp`,
  `src/JSystem/J3DGraphBase/J3DPacket.cpp`, `src/JSystem/J3DGraphBase/J3DShapeDraw.cpp`,
  `src/Game/System/ShapePacketUserData.cpp`, `src/Game/Player/J3DModelX.cpp`): native `.n3d`
  packets remain the only model draw path; direct Wii display-list entry points return without
  submitting GX commands on 3DS. The original implementations remain unchanged for Wii.
- **Remaining legacy draw callers no longer submit GX on 3DS** (`src/JSystem/JUtility/JUTVideo.cpp`,
  `src/JSystem/JParticle/JPABaseShape.cpp`, `src/Game/System/Overwrite.cpp`,
  `src/Game/Enemy/KirairaChain.cpp`, `src/Game/MapObj/ElectricRail.cpp`,
  `src/Game/MapObj/ElectricRailMoving.cpp`, `src/Game/MapObj/NormalMapBase.cpp`):
  single-buffer XFB copy, the rotational particle shape, JPA compatibility display lists,
  Kiraira's display-list rope, electric-rail display-list shadows, and normal-map shape setup
  use the existing native implementation where available or return explicitly on 3DS.
  Object-level auditing confirms no 3DS object retains unresolved references to
  `GXCallDisplayList`, `GXCopyDisp`, `GXDrawDone`, or display-list/FIFO setup APIs.
- **Camera/progression projection setters no longer write GX state on 3DS** (`src/Game/Util/CameraUtil.cpp`,
  `src/Game/Scene/ScenarioSelectScene.cpp`): native projection state remains owned by the Citro3D
  direct renderer; the original GX projection writes remain available on Wii. `JUTVideo` also
  avoids registering the Wii draw-done callback on 3DS.
- **Direct draw utility services are native-only on 3DS** (`src/Game/Util/DirectDraw.cpp`,
  `src/Game/Util/DirectDrawUtil.cpp`, `src/Game/Util/DrawUtil.cpp`): shared camera,
  projection, primitive setup, texture, model and legacy GX utility paths are excluded from
  the 3DS translation units. The existing Citro3D direct helpers are used where available;
  unsupported EFB-copy, J3D setup and framebuffer utility operations are explicit no-ops
  rather than hidden GX fallbacks.
- **Procedural-geometry limitations are recorded for the next compositing pass**:
  `BezierSurface` still omits Wii normal/texture-generator interpolation, arbitrary J3D
  shadow-volume meshes remain skipped on 3DS, and the EFB/raster-scroll/dark-mask services
  of `MarioActorSpecialDraw` remain explicit workarounds. These are visual-parity gaps,
  not hidden GX fallbacks.
- **Screen effects and compositing are native-only in the 3DS target** (`src/Game/Screen`):
  the shared capture/blur/bloom, depth-of-field, screen-alpha, star-pointer blur, Home
  Button, ODH capture and THP GX entry points no longer emit GX. Existing direct overlays
  remain available where already converted; Wii EFB-copy, TEV/indirect blur, depth-alpha
  capture, video YUV conversion and ODH JPEG capture are explicit 3DS skips pending a
  framebuffer-native implementation. This preserves the zero-GX invariant but does not
  claim visual parity for those effects.
- **Star Pointer depth peek uses the native 3DS render target** (`src/Game/Screen/StarPointerDirector.cpp`):
  the 3DS path calls `hostGfxPeekEfb` directly and runs the depth callback immediately,
  replacing Wii draw-sync/`GXPeekZ` without entering the GX parser. Color EFB capture,
  alpha/depth texture capture, THP YUV conversion and ODH JPEG capture remain open.
- **Star Pointer blur uses native textured strips** (`src/Game/Screen/StarPointerBlur.cpp`):
  the Wii two-pass texture strip is rebuilt as horizontal and vertical Citro3D triangle
  strips using the existing direct 2D submitter. Pointer depth probing and layout
  behaviour are unchanged; the texture remains subject to the native JUT texture bridge.
- **Framebuffer capture services use the native render target** (`src/Game/Screen/CaptureScreenDirector.cpp`,
  `src/Game/Screen/ScreenAlphaCapture.cpp`, `src/Game/Screen/GalaxyMapController.cpp`,
  `src/Game/Screen/ImageEffectLocalUtil.cpp`): color and alpha capture requests no
  longer return before invoking the native JUT texture bridge, and image-effect
  overlays submit through DirectDraw. Tile requests currently capture the native
  target as a whole because the host texture bridge owns the render-target crop.
- **ODH JPEG capture uses native CPU readback** (`platform/3ds/host/gfx.c`,
  `src/Game/Screen/OdhConverter.cpp`): the requested 488x328 render-target region is
  read back and encoded by the existing ODH RGB565 encoder without GX copy commands.
- **THP YUV video uses a native conversion path** (`src/Game/Screen/THPDraw.c`,
  `src/Game/Screen/THPDrawNative.cpp`): decoded YUV420 frames are converted to RGBA
  on the CPU, uploaded through a JUT texture and drawn with DirectDraw. Wii GX setup
  and TEV conversion remain unchanged.
- **NormalMapBase submits converted native model packets** (`src/Game/MapObj/NormalMapBase.cpp`):
  the 3DS path no longer drops the object. It enumerates the model's converted `.n3d`
  material packets and submits representable shapes through `rvlNativeBegin`/
  `rvlNativePacket`, preserving model transforms, animation, and the actor's resolved
  `ActorLightInfo`. The Wii indirect normal-map TEV lighting is still not equivalent:
  normal/gradient/indirect textures, BTK transforms, light matrices, thresholds, and
  back-light modes require a dedicated native pass rather than the ordinary material
  conversion.
- **Mario Search Light uses a native 2D beam fallback** (`src/Game/Player/MarioSearchLight.cpp`):
  the 3DS path preserves the swim/jet/talk/cooldown gates and fade timing, then submits
  a depth-disabled translucent projected beam rectangle through `TDDraw`. The Wii
  stencil-like front/back cone intersection remains unavailable, so this is an explicit
  visual approximation rather than a GX fallback.
- **Talk depth peek uses the native 3DS render target** (`src/Game/NPC/TalkDirector.cpp`):
  the 3DS path uses `hostGfxPeekEfb` and reconstructs the world position with the active
  camera matrices, replacing Wii draw-sync and `GXPeekZ`. `JUTTexture::capture` is also
  explicitly disabled on 3DS until a native texture ownership path is added; screen
  effects therefore cannot silently enter the GX copy implementation.
- **Legacy DOL texture capture is disabled on 3DS** (`src/Game/System/Overwrite.cpp`,
  `src/Game/Player/MarioShadow.cpp`): `captureDolTexture` and its copy-filter setup no
  longer emit GX. The Mario shadow alpha-copy result remains a documented visual gap until
  native EFB-to-texture ownership is implemented.
- **Removed 2026-10-22 (Claude):** the scenario/zone-list, `GalaxyStatusAccessor` and
  `StageDataHolder` null guards added on 2026-10-20. They only hid the consequence of a CIA built
  with the non-existent stage `BeachBowlGalaxy:1` (Atollo is `HeavenlyBeachGalaxy`); the files
  are back to their committed state.
- **Ring beam shadow strips use DirectDraw** (`src/Game/Enemy/RingBeam.cpp`, 3DS):
  the four shadow-volume triangle strips now submit world-space vertices through the
  Citro3D direct strip helper; Wii immediate-mode setup and submission remain unchanged.
- **FireBall light colour uses `Color8`** (`src/Game/Enemy/FireBall.cpp`):
  the light colour constant no longer depends on the Wii GX colour type; its channel values
  and point-light behaviour are unchanged.
- **Boss point-light colours use `Color8`** (`src/Game/Boss/TripodBossCore.cpp`):
  the three point-light requests now use the engine's platform-independent colour type
  instead of the Wii GX colour type; this removes a non-rendering GX dependency from the
  Boss sources without changing the colour values or light behaviour.
- **JParticle submits use Citro3D** (`src/Game/Effect/ParticleDrawExecutor.cpp`,
  `src/JSystem/JParticle/*`, `include/Game/Util/DirectDraw.hpp`,
  `src/Game/Util/DirectDraw.cpp`, `libs/JSystem/include/JSystem/JParticle/JPAResourceManager.hpp`):
  the 3DS particle executor and JPA shape paths now submit billboard, point, line and stripe
  geometry through the direct Citro3D backend. Particle point/line sizing, emitter TEV
  registration, child-shape setup, extended-texture setup, clip-mode restoration and
  resource TEV/array/indirect setup are excluded from the 3DS target; they cannot invoke
  the GX backend after native geometry submission. Wii GX state, array, TEV and indirect
  setup remains available only to the non-3DS build.
- **J3D DrawBuffer is native-only on 3DS** (`src/Game/System/DrawBuffer.cpp`,
  `platform/3ds/gx/native.cpp`, `platform/3ds/host/debug.c`):
  the 3DS draw buffer always routes model packets through the `.n3d` Citro3D renderer,
  and model registration is no longer gated by the debug `native=1` option. Unsupported
  packet features are counted and skipped by the native path instead of falling back to
  GX display lists; `native=2/3/4` remains available for material diagnostics.
- **J3D palette textures use the native Citro3D decoder** (`platform/3ds/gx/native.cpp`):
  C4, C8 and C14X2 textures are accepted by the native material path because
  `rvl3DSGetNativeTIMGTexture` expands their palettes to RGBA8; they no longer trigger a
  GX fallback.
- Native submit failures now propagate to the J3D packet path, so a rejected native draw is not
  reported as a successful native submission or silently counted as rendered.
- **Native J3D packets no longer reject model LOD flags** (`platform/3ds/gx/native.cpp`):
  LOD selection is resolved by the active J3D packet before native submission, so models
  carrying `J3DMdlFlag_EnableLOD` use their converted shape/material tables instead of
  being discarded before the packet renderer runs.
- **J2D base rectangles and pictures use the direct Citro3D path on 3DS**
  (`src/JSystem/J2DGraph/J2DGrafContext.cpp`, `src/JSystem/J2DGraph/J2DOrthoGraph.cpp`,
  `src/JSystem/J2DGraph/J2DPicture.cpp`, `include/Game/Util/DirectDraw.hpp`,
  `src/Game/Util/DirectDraw.cpp`): colored fill boxes, port setup, scissor setup,
  orthographic matrix setup, `draw`, `drawOut` and `drawTexCoord` no longer emit GX commands
  on 3DS. The direct picture path preserves corner colours, alpha modulation, mirroring,
  rotated coordinates and the explicit picture transform. Up to three picture textures are
  composed by native Citro3D stages; a fourth texture is submitted by an explicit native
  alpha-blended pass because the host interface exposes three texture coordinates. The
  native scissor state is applied to direct geometry, and `setTevMode` is a 3DS no-op so
  it cannot re-enter the GX backend.
- **J3D special cases use native workarounds** (`src/Game/System/ShapePacketUserData.cpp`,
  `src/Game/System/Overwrite.cpp`, `src/Game/Player/J3DModelX.cpp`,
  `platform/3ds/gx/native.cpp`):
  CPU-skin vertices now retain their source VTX1 position index in `.n3d`; the native path
  reads the current `J3DVertexBuffer` arrays after `J3DSkinDeform` and uploads them with the
  packet base matrix. Matrixless shapes use the packet base matrix, missing matrix tables use
  the base matrix, and LOD packets use the active J3D shape selection. Packed U8/S8/U16/S16
  position and normal arrays are decoded according to their declared GX component type rather
  than being treated as signed 16-bit data. The associated GX display-list calls are excluded
  from the 3DS target.
- **Native material conversion reports texture-unit overflow** (`tools/3ds/native/mat.py`):
  materials that require more than the three texture units exposed by the native packet format
  now fail conversion instead of silently binding an unassigned texture source. Existing
  `FLAG_INEXACT` records retain their previous approximation/statistics meaning and remain
  rejected by the runtime for compatibility with already-generated files. They still require
  native multi-pass decomposition before this coverage item can be considered complete.
- **Native packets reject truncated materials** (`platform/3ds/gx/native.cpp`):
  materials exceeding the six-stage/three-unit native limits are counted as material fallbacks
  and skipped before upload. Deliberate `FLAG_INEXACT` approximations remain renderable; this
  prevents valid approximated materials from disappearing while the native multipass
  representation is being implemented. The packet never re-enters the Wii GX path on 3DS.
- **Native packets reject malformed shape tables** (`platform/3ds/gx/native.cpp`):
  unknown shape modes and groups with more than ten matrix entries are counted as shape
  fallbacks instead of being silently clamped to the native palette size.
- **Native model registration validates `.n3d` tables** (`platform/3ds/gx/native.cpp`):
  header offsets, count arithmetic, shape modes/strides, group-table locations and material
  stage/unit limits are checked before allocating or retaining the registration block. Invalid
  files are rejected without partially initialized model state.
- Shape vertex strides are checked against the attribute mask during registration, including
  position format, source-position, normal/colour, UV and texture-matrix fields; malformed
  vertex records cannot reach the decoder.
- Group records are checked after loading as well: matrix count and vertex/index count limits
  are enforced before the model is retained, while zero-sized groups remain accepted for
  assets whose primitive decoder intentionally emits no triangles.
- The loader uses the source file size to validate every table and vertex/index payload range;
  aligned but out-of-file offsets are rejected before any native draw can read them.
- Triangle index counts must be multiples of three, and each decoded index is checked against
  its group's vertex count before the index is rebased into the native arena.
- Group matrix references are checked against the J3D model draw-matrix table; only the
  `0xFFFF` carry-forward sentinel may bypass that range check.
- A group cannot begin with `0xFFFF`: the converter resolves carry-forward entries before
  serialization, so an unresolved first entry is malformed and rejected by registration.
- Native packet failures now propagate even after an earlier group was submitted; partial
  geometry is never reported as a fully successful packet. The 3DS display-list path remains
  disabled, so this does not reintroduce a GX fallback.
- Matrix submission checks both animation sources before dereferencing them; missing
  `J3DMtxBuffer` or draw matrices are reported as unsupported packet data instead of crashing
  or using an incomplete transform.
- Native packet entry now validates the packet, shape, model, model-data and base-matrix
  pointers before reading any J3D state.
- Native group uploads invalidate the cached `Upload` record before rebuilding it, close the
  source file on every allocation/range failure, and reject missing CPU-skinned normal data;
  failed transient or persistent uploads cannot be reused as successful submissions.
- Native material validation also rejects texgen tables larger than the eight entries supported
  by the serialized material and deferred texture-matrix decoder.
- Native packet submission verifies that its `rvlNativeBegin()` model context is initialized,
  live and belongs to the packet's model; out-of-order or stale packet calls are rejected
  instead of using global draw state from another model.
- Concat-view packets now reject a missing animation matrix table instead of silently drawing
  with the packet base matrix, which could produce visibly incorrect joint transforms.
- CPU-skinning setup now validates position/normal component types and fractional-bit ranges
  before decoding; unknown formats no longer fall through to signed-16 decoding or undefined
  shifts.
- Material snapshots and native begin now reject missing J3D TEV/color blocks, out-of-range
  material indices and invalid material nodes before reading animated colors, constants or
  texture matrices.
- Native begin rejects models without model data before consulting the registered `.n3d`
  resource table.
- The 3DS RVLFaceLib bridge now retains per-character model matrices and expressions,
  validates model lifecycle and draw settings, and exposes `RFLGetExpression`; these values
  are ready for the native Mii geometry submit instead of being discarded by empty stubs.
- EFB capture now rejects null destination addresses and coefficient buffers at both the GX
  cache boundary and host copy boundary; invalid copies cannot leave a consumer with a
  success-shaped texture mapping.
- 3DS depth-of-field now uses the native texture blur helper over the captured frame instead
  of compositing an unfiltered copy; the remaining difference from Wii is the absence of
  depth-weighted multi-target reconstruction.
- 3DS bloom now also uses the native multi-sample blur helper for its additive composite; the
  remaining approximation is that Wii's luminance threshold and two-level blur chain are not
  yet represented as separate native render targets.
- Native THP YUV conversion now handles odd frame dimensions with ceil-divided chroma
  addressing and rejects non-positive draw rectangles.
- Native ODH capture validates framebuffer/encoded-image limits, rejects oversized encoder
  results, and returns numeric zero for an unavailable image size instead of a null pointer
  converted to `u32`.
- Material records are checked for valid active texture-map/coordinate pairs and explicit
  `0xFFFF/0xFF` sentinels on unused units before registration. Active texture indices are also
  checked against the J3D model's actual texture table; a missing texture table is accepted only
  when the material has no active texture units.
- The currently generated native asset set contains 7 models and 7780 material records, all
  within the single-pass limits. The converter still rejects future overflow assets explicitly;
  no overflow material is silently packaged or rendered.
- Native draw failures are now separated from light-configuration diagnostics: a rejected host
  submit is counted as `FB_SUBMIT`, while `FB_LIGHTS` remains reserved for an actual
  non-representable actor-light configuration.
- Actor-light channel masks are checked before submission; colour channels may request only
  the two physical `ActorLightInfo` lights, while alpha channels may additionally request the
  dedicated alpha slot. Other GX light slots are rejected as `FB_LIGHTS` instead of silently
  dropping their contributions.
- The native shader exposes six light uniform slots. `ActorLightInfo` supplies at most two
  physical lights for each colour/alpha channel, so a representable actor-light submission
  stays within that budget; `buildGpuTransform` remains the final guard for any future light
  source extension.
- **The native converter preserves matrix-table overflow for validation** (`tools/3ds/native/n3d.py`):
  SHP1 groups are no longer truncated to ten matrices while decoding. An asset exceeding the
  `.n3d` group limit now fails conversion with an explicit error before a file is written.
- **Native asset conversion fails on incomplete output** (`tools/3ds/native/build_native.py`):
  conversion failures are still written to `report.txt`, but the converter now exits nonzero
  after reporting them. Build automation cannot mistake a partially converted native asset set
  for complete coverage.
- **Residual gameplay rendering utilities are native-only on 3DS** (`src/Game/Util`,
  `src/Game/Map`, `src/Game/MapObj`, `src/Game/Player`, `src/Game/LiveActor`,
  `src/Game/Enemy`, `src/Game/Boss`): the remaining direct geometry, water, shadow,
  footprint, fur, snow, search-light, ribbon and special-draw paths either use the
  existing Citro3D helpers or explicit 3DS workarounds. A per-source 3DS preprocessing
  audit found no active GX function calls in these directories; Wii implementations
  remain in the non-3DS branches.
- **J3DModelX delegates packet-owned draws to native J3D** (`src/Game/Player/J3DModelX.cpp`):
  when a standard `J3DModel` owner exists, its packet is submitted through the native
  Citro3D renderer; standalone J3DModelX display-list-only draws are skipped on 3DS rather
  than entering GX.
- **Shadow volume model fallbacks are explicit on 3DS** (`src/Game/LiveActor/ShadowVolumeFlatModel.cpp`,
  `VolumeModelDrawer.cpp`): arbitrary J3D display-list volume meshes use a native bounding-box
  approximation rather than invoking `MR::drawSimpleModel`/GX; parametric box, line, sphere,
  oval and cylinder shadows continue through the direct Citro3D geometry helpers.
- **Collision shadow alpha passes use direct Citro3D geometry** (`src/Game/Player/MarioShadow.cpp`):
  the two Wii destination-alpha texture passes now write the equivalent alpha mask from the
  available parametric collision-shadow box instead of becoming unconditional no-ops.
- **Bezier surface strips use direct Citro3D geometry** (`src/Game/Util/BezierSurface.cpp`,
  `src/Game/Ride/BigBubble.cpp`): triangle strips are expanded to triangle submits on 3DS,
  avoiding immediate GX geometry. The native workaround intentionally ignores Wii-only normal/
  texture-generator state; the generated positions remain identical.
- **Mario special draw GX services are excluded on 3DS** (`src/Game/Player/MarioActorSpecialDraw.cpp`):
  screen-box EFB capture, raster-scroll framebuffer copies, dynamic display-list material
  updates, and the dark-mask alpha-buffer path return explicitly on 3DS; procedural spin
  lines, sphere masks, and wall-blend rectangles use DirectDraw state where supported.
- **J3D reset generators and collision-shadow GX paths are excluded on 3DS**
  (`src/JSystem/J3DGraphBase/J3DSys.cpp`, `src/Game/Player/MarioShadow.cpp`):
  J3D draw/state reset methods no longer emit Wii GX command streams, texture-cache
  initialization is skipped, and collision-shadow modes that require destination-alpha
  or immediate-mode geometry return explicitly. Parametric volume shadows continue
  through the existing DirectDraw box helper; Wii behavior is unchanged.
- **Unsupported search-light and bubble material generators are explicit 3DS skips**
  (`src/Game/Player/MarioSearchLight.cpp`, `src/Game/MapObj/BigBubbleDrawer.cpp`):
  the Wii destination-alpha/TEV search-light pass and BigBubble's legacy material
  setup no longer submit GX on 3DS. Native model draws remain owned by the regular
  J3D path; the Wii implementations are unchanged.
- **Remaining Wii-only state generators are explicit 3DS skips**
  (`src/Game/NPC/MiiFacePartsHolder.cpp`, `src/Game/MapObj/FallOutFieldDraw.cpp`,
  `src/JSystem/JUtility/JUTTexture.cpp`, `src/JSystem/JUtility/JUTPalette.cpp`):
  Mii TEV/material setup, FallOutField destination-alpha compositing, and GX texture/TLUT
  object loading no longer emit GX on 3DS. Native texture decoders continue to consume
  the resource metadata directly; the original Wii paths remain unchanged.
- **Wii draw-done signaling is excluded on 3DS** (`src/JSystem/JUtility/JUTVideo.cpp`):
  `drawDoneStart` no longer enters the emulated Wii FIFO.

## Rendering paths on 3DS (state on Oct 24 2026)

This section replaces the 2026-10-21 list of "remaining active Wii-compatible rendering paths": since block 8 the 3DS target links no GX or GD function, and the GX interpreter (`processor`, `fifo`, `pixel`, `texture`, `pe`, `capture`, legacy `draw`) is deleted.
- J3D models draw natively from pre-converted `.n3d` files (`platform/3ds/gx/native.cpp`, `native_submit.cpp`, `native_clip.cpp`). Packets the native path cannot draw are skipped and counted in the log ("skipped unsupported packets"). They do not fall back to GX.
- Direct geometry (particles, lines, screen effects, procedural surfaces) draws through `TDDraw` (`src/Game/Util/DirectDraw.cpp`) and `hostGfxDraw`.
- nw4r layouts and glyphs use their TEV compiled into Citro3D combiners (block 3, `tev_compile.cpp`); `CharWriter` keeps its own native glyph path.
- Mii heads are drawn as the Mii's icon on a square (block 12): `src/RVLFaceLib` is not in the target.
- Limits the PICA cannot reproduce (depth as a texture, indirect textures) are in `platform/3ds/HARDWARE_LIMITS.md` (block 13).

## Native rendering migrations completed in the 2026-10-21 pass

- **nw4r glyphs use native textured triangles** (`src/nw4r/ut/ut_CharWriter.cpp`,
  `src/Game/Util/DirectDraw.cpp`, `platform/3ds/rvl/jut_texture.cpp`): supported
  3DS glyph quads now use the native texture decoder and colored triangle
  submission, preserving per-corner colors and the Wii path.
- **nw4r layout quads use the native texture path** (`src/nw4r/lyt/lyt_common.cpp`,
  `src/nw4r/lyt/lyt_material.cpp`): supported single-texture layout pictures are
  submitted as native triangles with vertex colors. Complex multi-stage/indirect
  TEV materials remain on the compatibility path.
- **Native J3D texture-matrix coverage was expanded** (`platform/3ds/gx/native.cpp`):
  material snapshots now cover the supported J3D matrix range through slot 7 and
  deferred POS/NRM/environment texture-matrix user data uses available native
  state. When a packet has user data but no calculated `J3DTexMtxObj`, the native
  path builds a temporary matrix object from the current material snapshot rather
  than dropping the packet. CPU-skinned models still require source vertex mappings
  absent from `.n3d`.
- **Native procedural strip submission was tightened** (`src/Game/Util/BezierSurface.cpp`):
  3DS strips use bounded native strip buffers with vertex-count validation.
- **DirectDraw state fidelity was improved** (`src/Game/Util/DirectDraw.cpp`):
  native line and textured-quad paths now honor explicit alpha-test and blend
  equation/factor state.
- **Native asset discovery is incremental** (`tools/3ds/convert_smg_files.py`,
  with `tools/3ds/native/build_native.py` as the project-side equivalent):
  `convert_smg_files.py --source smg\files --discover-native --native smg\native`
  scans already-converted little-endian archives, compares every discovered J3D
  model with the existing `.n3d` tree, and writes `smg\native\missing-native.txt`.
  It does not require the original Dolphin dump and never attempts unsafe reverse
  conversion of converted archives. Full conversion from original Wii files remains
  available separately.
- **Native extraction also accepts already converted J3D models**: the file converter detects
  the little-endian J3D signature produced by `build_files.py`, restores only the CPU-readable
  J3D tables to Wii layout, and then passes the model to the `.n3d` converter. Vertex arrays,
  display lists, texture pixels, and palettes are not byte-swapped indiscriminately. This
  allows native generation from an existing converted `smg\files` tree without the Dolphin
  originals.
- **The converter menu exposes all local native workflows**: local conversion, native-only
  generation from an existing converted tree, and missing-native discovery. The command-line
  equivalents are `--native-only` and `--discover-native`.
- **The converter now opens a local Tkinter GUI without arguments**:
  it provides `smg\files` and `smg\native` directory fields, Browse buttons, operation
  buttons, a log/progress view, and a recovery dialog for original, complete, and mixed
  conversion states. Each operation has an English caption, and the source validation checks
  required game directories, non-empty content, expected archive extensions, and recognizable
  Wii/3DS archive headers instead of accepting an arbitrary folder. The obsolete FTP mode has
  been removed. Validation runs on a worker thread, reports determinate progress while
  enumerating files and sampling archive headers, and keeps the Tkinter window responsive.
- **Native matrixless shapes now use the packet base matrix**:
  `J3DShpFlag_NoMtx` is handled explicitly in `platform/3ds/gx/native.cpp`, matching the
  original `J3DShape::drawFast` behavior instead of attempting to read joint matrices.
- **Native LOD models use the active packet**: models with `J3DMdlFlag_EnableLOD` are submitted
  through the native path using the shape and matrix selection already resolved by J3D.
- **Miis from Mii Maker's database** (`platform/3ds/rvl/rfl.cpp`, `platform/3ds/host/main.c`): the
  official RVLFaceLib database is Mii Maker's `CFL_DB.dat` (shared extdata 0xF000000B), read once,
  so `MiiSelect` lists and pages the Miis exactly as the Wii lists the Mii Channel's; the system
  Mii Selector applet is no longer opened (its call in `MiiSelect::appear` is gone, the file is as
  the original). Heads are drawn from the console's `CFL_Res.dat` with Citro3D, icons by a CPU
  rasteriser from the same parts. `MiiSelect::collectValidMiiIndex` keeps its 3DS reset of the
  counts.
- **Mirror plane read big-endian** (`src/Game/LiveActor/MirrorCamera.cpp`, `#ifdef __3DS__`):
  `setMirrorMapInfo` takes the mirror plane from the first position and normal of the mirror
  model; those arrays are big-endian for the GX layer, so they are now read big-endian. Before,
  the plane had a garbage position and orientation (wrong or missing reflections).
- **`MR::downFracVtx` halves big-endian positions** (`src/Game/Util/ModelUtil.cpp`, `#ifdef __3DS__`):
  it lowers a model's position fraction by one bit and halves every s16 coordinate in place;
  the array is big-endian for the GX layer, so the halving now reads and writes big-endian.
  Before, every coordinate was destroyed (Dino Piranha's planet in EggStarGalaxy drawn as a box).
- **Fur layers read the GPU's big-endian data** (`src/Game/Util/FurShader.cpp`, `#ifdef __3DS__`):
  `CShader::calc` reads the model's positions, normals and texture coordinates and writes the
  pushed-out layer positions; `makeIndexData` / `checkBorderVtx` read vertex indices from the
  display lists. Those arrays stay big-endian for the GX layer, so they are now read and
  written big-endian. Before, every fur layer had garbage positions (Dino Piranha's planet in
  EggStarGalaxy drawn as a box).
- **Display lists read big-endian** (`J3DShapeDraw.cpp`): vertex counts in model display
  lists are read and written byte by byte.
- **`GXColor` read as a word** (`GXTev.c`, `GXLight.c`, `GXPixel.c` through
  `GX_COLOR_AS_U32` / `GX_COLORS10_*` in `GXRegs.h`, and `J3DTevStage::load` in
  `J3DTevs.hpp`): the colour bytes are assembled explicitly instead of read as a u32.
- **Functions with only an assembly body** (`TVec3::dot`, `JMAFastSqrt`,
  `JMAHermiteInterpolation`, `J3DCalcZValue`, `PSVECKillElement`, the s16
  `J3DHermiteInterpolation`): C versions that perform the same operations. GCC treats a
  missing return as unreachable, which made functions fall through into the next one.
- **`void` functions with only an assembly body** (`TVec3::operator=`,
  `JMath::gekko_ps_copy3/6/12/16`, `JMAQuatLerp`'s dot product, `JMAVECScaleAdd`,
  `JMAVECLerp`, `J3DPSMtx33Copy`, `J3DPSMtx33CopyFrom34`, `J3DMtxBuffer::setNrmMtx`,
  `J3DTexMtxInfo::setEffectMtx`, `J3DTextureSRTInfo::operator=`, `MR::vecScaleAdd`,
  `MR::PSvecBlend`): an `#else` with the same operations in C. Without it they did nothing.
- **ARAM transfers** (`Overwrite.cpp`, `JKRAramPiece::startDMA`): the Wii refuses a transfer
  whose main-RAM address is below `0x80000000`; on the 3DS main RAM is there, so only a null
  address is refused. The ARAM itself is the Wii SDK's own dummy ARAM in MEM2
  (`src/RVL_SDK/aralt/aralt.c`, now built for the 3DS; it includes `<string.h>` there because
  it is compiled as C). Before, ARAM functions were empty stubs, the audio ARAM heap had a null
  base and every stream and wave allocation failed.
- **Sound IDs** (`JAISound.hpp`, `JAISoundID`): the section/group/wave parts overlay the u32
  composite; on a little-endian CPU they are declared in reverse so that the composite keeps
  the section in its top byte. Without it every sound ID pointed at the wrong table entry (the
  title waited forever for its music, so the logo never appeared).
- **Word/byte/bitfield views fixed after an audit of unions and bitfields** (3DS only):
  `GXTev.c` (`GXSetTevOp` tables are bitfield structs read as words: packed from the fields;
  text was drawn with garbage TEV stages), `MarioEffect.cpp` (FlagWord bytes reversed so
  mByte0 is the top byte of the table's word), `MarioSound.cpp` (the first flag byte taken
  from the word's top byte), `RushEndInfo.hpp` (damage type bitfield declared in reverse to
  match the `(_20 >> 24) & 15` reads), `JAUSoundAnimator.hpp` (flag bits of the converted .bas
  word declared in reverse). Left for block J: `JAUAudible.hpp` / `JAUAudibleParam.hpp`
  (bitfields over the audible switch word, also read as bytes).
- **J3DModelX flags tested as a word** (`J3DModelX.cpp`, `drawIn`): the extra display lists
  are chosen by testing bits 0-15 of the `mFlags` bitfield read as a u32; Metrowerks allocates
  bitfields from the top bit, so on the 3DS bit i is field (31 - i). Without it the shadow
  mode (`_1E`) display list never ran and Mario's shadow pass left a black square in the
  top-right corner of the screen.
- **Texture numbers read from display lists big-endian** (`J3DTevs.cpp`, `getTexNoReg`): BDL
  models keep the texture number in a BP command of their (big-endian) display list; read as a
  native u32 it came out as the register number (e.g. 148), so materials loaded garbage
  textures. Every BDL model was affected.
- **Material and ambient colours as words** (`J3DMatBlock.cpp`, `loadMatColors` /
  `loadAmbColors`): the colours are assembled R-G-B-A instead of read as a native u32.
- **Depth-clear texture as bytes** (`MainLoopFramework.cpp`, `clearZTexData`): the Z24X8
  texture that resets the depth buffer was a `u32` array, whose bytes come out reversed on a
  little-endian CPU; on the 3DS it is written as the same bytes in GX order.
- **Collision files relocated once** (`KCollision.cpp`, `isBinaryInitialized`): the Wii
  marks a relocated `.kcl` by its first pointer being negative (0x8xxxxxxx); on the 3DS the
  check compares the first field with the file's address instead. Without it a collision file
  shared by several objects was relocated again and again, and the game wrote to wild
  addresses (seen in EggStarGalaxy and in the title scene).
- **Ghost race data read big-endian** (`GhostPacket.cpp`): the u32, s16 and TVec3s reads
  assemble their bytes in big-endian order; the `.gst` files stay unchanged.
- **Returning the address of a temporary** (`SimpleEffectObj.hpp`, `MorphItemObjNeo.hpp`,
  `EffectObjGravityDust.cpp`, `FloaterFloatingForce.hpp`, `MarioCollision.cpp`,
  `TVec2::subInline`): a static copy (or a copy by value) is returned; GCC returns null.
- **`FunctionAsyncExecutor::isEnd` / `waitForEnd` with a removed name**: the Wii read the
  stale entry past the end of the list; the 3DS reports "ended" and returns.

- **Linked lists with hard-coded node offsets** (nw4r `lyt/arcResourceAccessor.h`,
  `group.h` x2, `layout.h`, `pane.h`, `types.h`). Metrowerks places the vtable pointer after
  the data members and GCC places it first. The decomp already used `offsetof` for
  Metrowerks and `0` otherwise; the 3DS now uses the `offsetof` branch too.
  `JUTConsole`'s list (debug console) is unchanged.
- **`nw4r::ut::Color` from a number** (`libs/nw4r/include/nw4r/ut/Color.h`): on the 3DS
  the u32 conversions use shifts so `0xRRGGBBAA` means the same as on the Wii. The builder
  converts the layout vertex and text colours (declared u32) to match.
- **GPU sync messages** (`src/Game/System/DrawSyncManager.cpp`): pointers are recognised
  as `>= 0x10000` on the 3DS instead of `>= 0x80000000`.
- **Sound type IDs** (`libs/JSystem/include/JSystem/JAudio2/JAUSoundTable.hpp`): the
  type is read as the top byte of the item word (`>> 24`) on the 3DS, where it is not the
  word's first byte.
- **Sequence operands** (`libs/JSystem/include/JSystem/JAudio2/JASSeqReader.hpp`): read
  big-endian byte by byte on the 3DS, so sequence bytecode stays as on the disc.

- **GX command stream capture** (`libs/RVL_SDK/include/revolution/gx/GXRegs.h`, `GXVert.h`):
  on the 3DS, writes to the write-gather pipe go to a buffer (`platform/3ds/include/rvl_gx_fifo.h`)
  that `platform/3ds/gx` interpreted in early builds (the interpreter is gone: nothing writes this stream now). `GXWGFifo` becomes a C++ object whose members append to it.
- **Register bit insertion** (`GXRegs.h` `FAST_FLAG_SET`, `GXFDLShortcut.h` `FAST_GPFLAGSET`):
  these were empty for non-Metrowerks compilers, which left every GX register at zero. The
  3DS uses a portable version of `rlwimi`.
- **Paired-single FIFO writers** (`src/RVL_SDK/gx/GXTransform.c`, `GXLight.c`): portable C
  versions next to the assembly. The light colour is written byte by byte to keep the order.
- **Hardware register addresses** (`revolution/os.h`): `OSPhysicalToUncached` maps the Wii's
  register blocks to a dummy buffer on the 3DS.

- **HOME Menu module** (`src/Game/System/HomeButtonMenuWrapper.cpp`): the Wii loads the HOME
  Menu as a PowerPC module, which cannot run on the 3DS; its entry points are empty functions.
- **Stack slot reuse**: the game keeps pointers into buffers whose block has ended
  (`LayoutManager::LayoutManager`), which Metrowerks tolerates. The 3DS build passes
  `-fstack-reuse=none` instead of changing the code.

## Handled without changing the decomp

- `nw4r::lyt::TexMap::ReplaceImage` binds a TPL again whenever `descriptorArray < 0x80000000`.
  `platform/3ds/rvl/tpl.cpp` makes `TPLBind` idempotent instead.
- MSL prints a null `%s` as an empty string (newlib prints "(null)"); `LayoutHolder::count`
  relies on it. `platform/3ds/rvl/printf.cpp` reproduces MSL behaviour for the sprintf family.
- The DSP is emulated in `platform/3ds/rvl/dsp.cpp`, replying to commands as the real one does.

## Inventory by file

Every decomp file changed on the port branches, by kind of change. Development scripts (not
published) regenerate the raw list, show changes outside `__3DS__`/`__MWERKS__` conditionals
and check that every changed file is named here. Unguarded changes are allowed only when they
keep the Wii build byte-identical.

- **Weak symbols for definitions GCC sees twice** (nerves defined in headers, SDK functions the
  game overrides): `Nerve.hpp` (`NEW_NERVE_WEAK`), `BenefitItemObj.hpp`, `MorphItemObjNeo.hpp`,
  `MarioActor.hpp`, `Trapeze.hpp`, `JASBank.cpp`, `JASWaveArcLoader.cpp`, `JPABaseShape.cpp`.
- **Standard C++ that GCC requires** (Wii-neutral): dependent names and template arguments
  (`JASGadget.hpp`, `TMatrix.hpp`, `TVec.hpp`), explicit specializations declared before use
  (`JMATrigonometric.hpp`, `DemoStartRequestHolder.cpp`, `MapPartsBreaker.cpp`), allocation
  operators taking `size_t` (`JKRHeap.hpp`, `JASHeapCtrl.hpp`), `friend` syntax
  (`LinkList.h`), member function pointers written `&Class::f` (`GalaxyMapController.cpp`,
  `ResourceHolderManager.cpp`), float literals `0.0f` instead of `0f`
  (`MainLoopFramework.cpp`), `nullptr` returned for a pointer (`DemoUtil.cpp`), `va_end`
  with its argument and `va_copy` (`IntermissionScene.cpp`, `JUTException.cpp`,
  `ReplaceTagProcessor.cpp`), name clashes with the C library (`TalkBalloon.cpp`), missing
  includes (`TUtil.hpp`, `JKRAram.hpp`), attribute placement (`Inline.hpp`, `types.h`),
  Metrowerks intrinsics replaced by portable code (`runtime.h`, `arithmetic.h`,
  `OSFastCast.h`, `OSTime.h`, `os.h`, `MathUtil.hpp`).
- **Jumps over initializations**: braces around `case` bodies and labels, added by
  a script of the development repository (`XanimeResource.cpp`, `ChangeBgmCube.cpp`, `KCollision.cpp`,
  `MarioActor.cpp`, `MarioWarp.cpp`, `SurfRay.cpp`, `GameEventFlagChecker.cpp`,
  `JMapInfo.cpp`, `JKRMemArchive.cpp`).
- **Undefined behaviour in the original that GCC exploits**: two vectors accessed as an array
  through a union, now one array (`Mario.hpp`, `MarioEnforce.cpp`, `MarioInit.cpp`); conditions
  on member function pointers that are always true (`MapPartsRailPosture.cpp`, `Sandstorm.cpp`);
  plus the returned temporaries and assembly-only helpers listed under "Done"
  (`SimpleEffectObj.hpp`, `FloaterFloatingForce.hpp`, `EffectObjGravityDust.cpp`,
  `MarioCollision.cpp`, `MathUtil.cpp`, `J3DMtxBuffer.hpp`, `J3DDrawBuffer.hpp`,
  `J3DStruct.hpp`, `J3DStruct.cpp`, `J3DShapeMtx.cpp`, `J3DAnimation.cpp`, `JMath.hpp`,
  `JMath.cpp`, `TVec.hpp`).
- **C versions of assembly-only functions**: `J3DTransform.cpp` (inverse transpose, normal
  matrix scaling, projection concat, matrix array concat), `J3DMtxBuffer.cpp`
  (`calcWeightEnvelopeMtx`), `StarPointerDirector.cpp` (`forceOutsideScreenEdge`, not yet
  decompiled in Petari: the C version translates the original assembly for GCC only).
  `os.h` (`OSf32tos16` / `OSf32tou8`: without the paired-single store they returned an
  uninitialised value, so every layout colour and alpha animation gave 0: invisible text and
  HUD).
- **Metrowerks-only code hidden from GCC** (pragmas, code-layout helpers): `math_types.hpp`,
  `HashUtil.cpp`, `LayoutUtil.cpp`, `GalaxyMap.cpp`.
- **Platform entry and system differences**: `GameSystem.cpp` (`smgMain` called by the 3DS
  `main`), `dsptask.cpp` (DSP task addresses), `db_assert.cpp` (no PowerPC stack walk),
  `HomeButtonMenuWrapper.cpp`, `DrawSyncManager.cpp`, `FunctionAsyncExecutor.cpp` (also: a task is
  marked ended before its waiter is woken, else the late store hit the next task's info and a
  scene was reported initialized while still placing objects),
  `ParticleEmitterHolder.cpp` (out-of-line definition GCC needs), `MainLoopFramework.cpp`
  (the triple-buffer wait polls for the retrace interrupt, which the port only delivers at OS
  calls; without it the main thread could spin forever).
- **Endianness and the GX command stream** (details under "Done"): `GXRegs.h`, `GXVert.h`,
  `GXFDLShortcut.h`, `GXTransform.c`, `GXLight.c`, `GXPixel.c`, `GXTev.c`, `J3DTevs.hpp`,
  `J3DShapeDraw.cpp`, `JASSeqReader.hpp`, `JAUSoundTable.hpp`, `JAUSectionHeap.cpp`,
  `JKRArchive.hpp` (bitfield order), `JKRUnitHeap.cpp` (the unit bitmap, written by bytes, is
  searched by big-endian words: without the swap units were handed out twice), `Color.h`, `GhostPacket.cpp`, and the nw4r list offsets
  (`arcResourceAccessor.h`, `group.h`, `layout.h`, `pane.h`, `types.h`).
- **No Wii Remote screens on the 3DS** (owner's request, 3DS only): `LogoScene.cpp` skips the
  wrist strap reminder (boot goes straight to the title, its layout is not loaded);
  `PlayerActionGuidance.cpp` (spin and rolling-ball tutorials), `SurfingGuidance.cpp` (tilt
  tutorial) and `StarPointerDirector.cpp` (pointer guidance) do not show their Wii Remote
  pictures.
- **Debug hooks for the port's test tools** (3DS only, under `__3DS__`):
  `GameSequenceProgress.cpp` (debug.txt `stage=`: start a stage instead of the title when the
  boot screens end), `HeapMemoryWatcher.cpp` (logs the heap sizes when a scene heap is
  created, and which heap ran out before the out-of-memory panic).

- **Sky debug switch** (`src/Game/Util/ObjUtil.cpp`, `MR::connectToSceneSky` / `connectToSceneAir`,
  `#ifdef __3DS__`): debug.txt "sky=0" keeps sky actors out of the scene (the title's sky model was ~95% of its
  draws; the clear colour shows instead, as in Wii stages without a sky); "air=0" does the same for air
  (atmosphere) actors. The sky was hidden by default for a while; since 2026-10-16 it is drawn by default again
  (owner's request, for the announcement video).

- **Frame phase timing** (`src/Game/System/GameSystem.cpp`, `GameSystem::frameLoop`, `#ifdef __3DS__`):
  six `rvlGXFramePhase` marks (a tick read each) split the frame into draw / endRender / update / calcAnim /
  endFrame+wait for the "Frame phases" log line next to every "GX frame" line. No behaviour change.

- **Stall phase diagnostics** (`src/Game/System/GameSystem.cpp`, `src/Game/System/MainLoopFramework.cpp`,
  `platform/3ds/gx/processor.cpp`, `platform/3ds/host/debug.c`, `#ifdef __3DS__`): track the current game frame
  phase in a volatile diagnostic value. The VBlank watchdog logs it after 300 retraces without a rendered frame.
  CIA 0.4.2 reported the broad `end render` phase; 0.4.3 adds subphases for `endGX`, framebuffer copy and
  `calcCombinationRatio`. CIA 0.4.4 marks the `endGX` setup, console draw, assertion flush and `GXFlush` separately.
  CIA 0.4.5 logs the GX parser's current FIFO offset, opcode, display-list depth, command count and draw/parser
  stage if the watchdog catches a freeze in `GXFlush`. CIA 0.4.7 adds primitive/VAT/vertex-size/count and draw
  pipeline substage (setup, decode, GPU submit or CPU submit). CIA 0.4.8 adds a 256-entry RAM trace ring for frame
  phases, GX commands, per-vertex progress, cache/range work, arena allocation, Citro3D frame begin and draw submit;
  the watchdog dumps recent entries for stalls in any frame phase. CIA 0.4.9 uses `C3D_FRAME_NONBLOCK` and drops a
  frame if the previous Citro3D queue remains busy, so the main thread does not wait indefinitely there; busy and
  resumed states are logged. 3DS-only diagnostic/recovery behavior; Wii rendering is unchanged and the trace ring
  writes no SD log entries on the hot path.

- **Retrace callback during JUTVideo construction** (`src/JSystem/JUtility/JUTVideo.cpp`, `#ifdef __3DS__`):
  `VISetPreRetraceCallback`/`VISetPostRetraceCallback` can restore interrupts and synchronously deliver a pending
  3DS VBlank before `JUTVideo::createManager` assigns `sManager` (the assignment occurs after the constructor).
  The callbacks now return while `sManager` is null; the first callback after construction proceeds normally. This
  prevents a data abort at `sManager->mPreCallback` (`FAR=0x24`) and leaves Wii behavior unchanged.

- **Scene category timing** (`src/Game/Scene/SceneFunction.cpp`, `CategoryList::execute` / `entryDrawBuffer*` /
  `drawOpa` / `drawXlu`, `#ifdef __3DS__`): a scoped timer per call feeds the "Game categories" line of the
  profile log (debug.txt profile=1; without it the timer only checks a flag). No behaviour change.

- **Audio: missing instrument bank** (`src/Game/AudioLib/OverwriteJAudio.cpp`, `JASTrack::channelStart`,
  `#ifdef __3DS__`): when a note cannot start and the track's bank is null, the note is dropped (logged) instead of
  running the debug check that calls through the null bank (jump to address 0). Happens while the 3DS sound archive
  has no instrument banks (tools/3ds/builder/baa.py WAVE_BANKS).
- **Audio: wave archive load logging** (`OverwriteJAudio.cpp` `JASWaveArc::setFileName` / `load`,
  `src/JSystem/JAudio2/JASWaveArcLoader.cpp` `loadToAramCallback`, `#ifdef __3DS__`): log lines for a wave archive
  file not found, a load refused, and each load to ARAM (start, done, failed). No behaviour change.

- **Wave data wait polls interrupts** (`src/Game/Scene/GameScene.cpp`, `GameScene::init`, `#ifdef __3DS__`): the
  empty `while (!isLoadDoneScenarioWaveData()) {}` loop calls `rvlPollInterrupts()`. The port delivers interrupts
  only at OS calls; without it the disc read completion of the wave archive being loaded never arrives.

- **Audio mixer bus bitfield order** (`libs/JSystem/include/JSystem/JAudio2/JASChannel.hpp`, `JASChannel::MixConfig`,
  `#ifdef __3DS__`): the bitfields are declared in reverse so `upper` is the high byte of `whole` as on the Wii
  (the output bus index into TChannel::setBusConnect's 12-entry table). Read as the low byte it indexed far past
  the table: every note channel got garbage bus ids (FF64...) and was silent. JAISound's u8 flag unions were checked:
  only single fields are used, never the whole byte.

- **J3D model draw knock-out** (`src/Game/System/DrawBuffer.cpp`, `DrawBufferShapeDrawer::draw`, `#ifdef __3DS__`):
  debug.txt `j3dskip=1` returns at once (no model drawn) to time what the J3D models cost; off by default.

- **Native J3D drawing, block N** (`#ifdef __3DS__`, debug.txt `native=1`, off by default):
  - `src/Game/System/ResourceHolder.cpp`, `ResourceHolder::createAndRegisterObject`: after a .bdl/.bmd becomes a
    J3DModelData, `rvlNativeRegister` loads its converted .n3d (sdmc:/smg/native, tools/3ds/native) when native=1.
  - `src/Game/System/ResourceHolderManager.cpp`, `ResourceHolderManager::createResourceHolder`: tells the port the
    archive path (`rvlNativeSetArchive`) while the holder creates its models (the archives are in memory: no disc
    entry number).
  - `src/Game/System/DrawBuffer.cpp`, `DrawBufferShapeDrawer::draw`: with native=1 the packets are drawn by
    `rvlNativePacket` (platform/3ds/gx/native.cpp) without the material and shape display lists; a packet it does
    not take (billboards, per-joint texture matrices, CPU skinning, palette textures) is drawn as before, after the
    material display list. Lights are still loaded by ActorLightCtrl::loadLight.
  - `src/Game/LiveActor/DisplayListMaker.cpp`, `DisplayListMaker::diff`: after each material's diff (the shared
    J3DMaterial then holds this model instance's animated values), `rvlNativeSnapshot` keeps a copy of them for the
    native drawing of that instance (only when native=1).
  - `platform/3ds/host/gfx.c`: native indexed submits have a per-frame budget (currently 2048). Once reached,
    remaining native packets are dropped without falling back into GX, preventing a pathological scene from
    filling the Citro3D command queue before `C3D_FrameEnd`.
  - The deferred Citro3D texture deletion queue was increased from 256 to 4096 entries. Native J3D texture
    cache eviction can churn while the GPU queue is busy; silently dropping deferred deletions exhausted VRAM
    and made subsequent native texture lookups fail.
  - The direct native J3D texture cache was increased from 64 to 256 entries so the initial Atollo scene can
    retain its material textures instead of repeatedly decoding and uploading them during scene assembly.
  - Native J3D `ResTIMG` palette textures now use their embedded `mPaletteDataOffset`, palette count and
    palette format during RGBA8 conversion instead of being rejected before upload. This removes the
    systematic native material fallback seen when the scene uses C4/C8/C14X2 textures.


- **Mario Foo beam e trail direct drawing** (`src/Game/Player/MarioFoo.cpp`, `#ifdef __3DS__`):
  the beam and ribbon quads retain the Wii geometry and texture coordinates but submit triangulated,
  textured quads through `TDDraw::drawTexturedTintedTriangles`; the Wii TEV/GX path remains unchanged.

- **Submit e presentazione Citro3D (solo host 3DS)** (platform/3ds/host/gfx.c, CIA 0.4.10): abilita GSPGPU performance logging e registra i primi ritorni C3D_FrameBegin/C3D_FrameEnd; durante la coda occupata campiona delta P3D/PPF, tempi Citro3D e gspIsPresentPending(GSP_SCREEN_TOP). Serve a distinguere completamento GPU, display transfer e swap del top screen; e diagnostica, non cambia il percorso Wii. Il perflog libctru ha limiti di sincronizzazione documentati, quindi usare i delta insieme agli altri indicatori.


- **Dump command buffer PICA iniziale** (platform/3ds/host/gfx.c, CIA 0.4.11): prima di C3D_FrameEnd salva una volta il buffer GPUCMD del primo frame in sdmc:/smg/c3d_first_frame.bin (header CMD1, capacita e lunghezza in parole, poi stream raw). Serve ad analizzare il command list rimasto senza completamento P3D; diagnostica solo host 3DS, nessun effetto Wii.

- **Indici uniform Citro3D corretti** (`platform/3ds/host/gfx.c`, CIA 0.4.12): `shaderInstanceGetUniformLocation` restituisce ID con base `0x60` per gli interi e `0x68` per i booleani; `C3D_IVUnifSet` e `C3D_BoolUnifSet` applicano già la sottrazione interna. Rimosse le sottrazioni duplicate, che producevano indici negativi: in particolare `pretransformed` restava falso nei draw con vertici già trasformati e l'ID del loop luci poteva scrivere fuori dagli array Citro3D. Solo renderer host 3DS; nessun effetto Wii.
### Native J3D animated texture matrices

The 3DS native `.n3d` material path now snapshots all eight J3D texture-matrix
slots when `__3DS__` is active.  The previous snapshot loop covered only slots
0-3 even though the native material format and submit path support slots 0-7;
animated complex materials using TEXMTX4-7 therefore used the identity matrix.
Wii code keeps the original four-slot snapshot behavior and is not changed.
- Frame lifecycle on 3DS no longer emits unconditional Wii GX cache/pixel-format commands:
  `MainLoopFramework::preGX()` is native-only no-op, and `endGX()` flushes the emulated FIFO only
  when legacy GX state or commands are pending. Native Citro3D frames therefore avoid the GX FIFO,
  while fallback command ordering remains available.
- 3DS direct drawing now tracks its view mode: closing a 2D direct draw no longer reads the 3D
  `CameraContext` just to restore camera state during early startup.
- The 3DS GX FIFO now copies each pending command batch before parsing it, preventing parser
  callbacks from overwriting their input. Overflow processing is guarded against recursion and
  the pending FIFO grows dynamically when legacy commands exceed its initial capacity.
- Main FIFO overflow now grows the capture buffer without parsing a partial command. This keeps
  multi-byte GX commands intact until an explicit flush point.
- GX FIFO writes are serialized because asynchronous scene initialization can emit GX commands
  concurrently with the main thread on 3DS; each complete value is appended under the FIFO lock.
- The main FIFO starts at 1 MiB and is capped at 16 MiB. Complete command batches are
  flushed automatically at 512 KiB, keeping the capture buffer and its temporary processing
  copy bounded while preserving headroom for scene heaps.
- GX parsing remains on the main/render thread. Asynchronous executor jobs may append
  serialized commands, but do not call the parser or Citro3D from their worker thread.
- **Removed 2026-10-22 (Claude):** the low-memory null guards added during CIA 0.8.x
  (DinoPackun/DinoPackunVs2 including the extra `mVsCount` member, `NerveExecutor`,
  `ModelManager`, the `MR::start*` animation helpers, `LightFunction`, `LightDirector`,
  `LightDataHolder`, `LightZoneDataHolder`). They chased symptoms of exhausted scene heaps; the
  root cause, the global preload of every galaxy's scenario archive, is addressed instead
  (per-screen archive loading). The files are back to their committed state.
- GX FIFO parsing is now restricted to the thread that initialized the FIFO (the main/render
  thread). Async workers may serialize GX bytes into the FIFO, but cannot invoke the
  Citro3D/Wii GX parser while another thread is producing commands.
- Added central runtime counters for legacy GX command-stream draws, vertices, and display-list
  calls. The counters are logged with the GX frame profile before disabling the fallback renderer,
  so remaining UI, particle, special-drawer, and other legacy paths can be identified separately
  from native J3D draws.
- Disabled the 3DS runtime GX fallback: frame writes outside display-list construction are
  discarded, `rvlGXProcess()` no longer interprets command streams, and pending-work checks never
  schedule the legacy renderer. Native Citro3D paths are now the only runtime renderer on 3DS;
  legacy UI, particles, RFL, and special GX drawers require explicit native implementations.
- Layout UI already has a native Citro3D quad path; 3DS `GXLoadTexObj` and `GXLoadTlut` now
  skip Wii texture-register setup so `nw4r::lyt::Material::SetupGX` reaches that native path
  without dereferencing disabled GX texture state.
- Layout UI materials without a texture now use a native vertex-colour triangle path instead of
  falling through to `GXBegin`; the texture-backed path also uses the real native texture
  conversion in normal builds, while the Wii implementation remains unchanged.
- Native 2D direct draws now force the affine clip-space `w` component to one. Wii-compatible
  matrix producers may leave the fourth projection row unspecified, but Citro3D cannot accept
  the resulting zero homogeneous coordinate.
- The 3DS layout quad entry point is now unconditional under `__3DS__`, including color-only
  materials with zero texture generators; those quads use the native vertex-color path and never
  reach the Wii `GXBegin` implementation.
- `nw4r::ut::CharWriter` now skips GX texture/state setup on 3DS. Its existing glyph triangle
  submission is the sole native font path; the Wii GX setup and immediate quad path remain
  unchanged.
- `ElectricRailShadowDrawer` now submits the rail shadow extrusion as native triangles on 3DS.
  The Wii display-list construction and submission path remains unchanged.
- `JUTTexture::capture()` now uses the native host EFB-copy bridge on 3DS. Captured textures are
  owned by the 3DS texture bridge and are invalidated with their `JUTTexture`; Wii GX texture
  copies remain unchanged. The current bridge copies the full top-screen render target, so
  effect-specific crop/coordinate remapping remains a follow-up limitation.
- `ScreenBlurEffect` now consumes the native captured render target on 3DS and composites it as a
  native screen quad. This is a native fallback for the Wii depth-aware blur: it preserves the
  effect's visible overlay without entering GX, while depth-aware multi-pass filtering remains
  unsupported until native crop/remap support is added.
- JPA particle matrix-generation and projection helpers are now no-ops on 3DS, matching the
  existing native particle submitters in `JPABaseShape`. Particle billboard, rotation, point,
  line, and stripe draws therefore cannot enqueue GX matrix or display-list commands.
- `GXCallDisplayList` is explicitly discarded on the 3DS target. Display lists remain available
  for Wii construction and compatibility, but no runtime 3DS draw can re-enter the legacy GX
  processor through a display-list call.
- `BloomEffect::postDraw()` now captures the current native top-screen target and composites it
  with Citro3D additive blending on 3DS. This is intentionally a single-pass approximation of
  the Wii threshold plus multi-pass blur chain; it removes the previous no-op while the native
  crop/remap and intermediate-target pipeline is completed.
- `NormalMapBase::draw()` submits converted native packets on 3DS and passes the actor's resolved
  light information to the native renderer. The Wii custom normal/gradient/indirect texture
  graph, BTK transforms, thresholds, and back-light modes are still not equivalent and remain
  an explicit native-rendering gap; no Wii GX setup is emitted by this path.
- `BloomEffectSimple::draw()` uses the same native single-pass additive capture as `BloomEffect`
  instead of returning immediately on 3DS. Thresholding and the Wii multi-pass blur remain
  approximations until native intermediate textures and crop/remap are available.
- `DepthOfFieldBlur::draw()` (0.18.3): the blur only beyond the area's distance, by depth-tested rectangles (`TDDraw::drawTextureTapsAtDepth`); it was on the whole screen. Before that:
- `DepthOfFieldBlur::draw()` now captures and composites the native top-screen target on 3DS
  instead of returning immediately. This preserves a controlled DOF-like overlay without
  touching GX; the Wii depth mask, blur pass, and focus-distance interpolation still require
  native depth/crop intermediate targets for a faithful implementation.
- `ElectricRailMoving::draw()` now submits its rail strips through `TDDraw::drawDirectStrip()`
  on 3DS. The Wii display-list and GX strip paths remain unchanged.
- The final special-drawer audit confirms `MarioActorSpecialDraw` and `WaterCameraFilter` keep
  their GX-only operations behind Wii guards and already use native direct drawing where needed.
  `KirairaChain` remains an explicit 3DS no-op because its Wii model display-list path has no
  native chain-material equivalent yet. `MarioShadow` volume drawing is native on 3DS; its
  trailing Wii destination-alpha reset is now also excluded from the 3DS target.
- (Superseded: these files were deleted later, see "Block 8" below.) The 3DS build intentionally still compiled `platform/3ds/gx/draw.cpp`, `pixel.cpp`,
  `texture.cpp`, `pe.cpp`, and `processor.cpp` as compatibility shims. Symbol-level inspection
  confirms they are not native draw entry points; `rvlGXProcess()` is empty, FIFO processing
  discards pending commands, and `GXCallDisplayList()` returns immediately on 3DS. Removing the
  objects would risk breaking SDK state-generation and diagnostic links without reducing the
  active native rendering path.
- `KirairaChain::draw()` now renders a native oriented strip through the simulated rope points
  on 3DS. This preserves the chain's dynamic silhouette and depth-tested visibility while
  intentionally approximating the Wii chain link material, whose display-list geometry is not
  available to the native material translator.
- `NormalMapBase` has been audited against the native `.n3d` material reader. The native path
  supports ordinary normals, texture units, model transforms, and actor-light inputs, but not
  this actor's Wii-specific normal texture, gradient texture, indirect light matrices, BTK
  transforms, thresholds, and custom TEV combiner as one material. The 3DS draw deliberately
  submits only the representable native packet and does not re-enter the GX fallback.

## Phase 4 end (Oct 22 2026): remaining invisible GX geometry

- `J3DModelX.hpp/.cpp`: `J3DModelXVisibleDraw` scope; inside it `directDraw(nullptr)` draws the model's own packets natively. Used by `MarioActor::drawMarioModel` (body, cap, hands), `GhostPlayer` and `FlexibleSphere`. Shadow, reflection, blur and indirect uses stayed undrawn until stencil/multipass (now drawn: blocks 4 and 11).
- `FlexibleSphere.cpp`: `shapeCallBack`'s texture matrix through `rvlNativeSetTexMtxOverride`.
- `ClipArea.cpp`: volume counted in destination alpha only (front: add, back: reverse subtract), alpha write mask, reversed depth test; `ClipAreaShape.cpp` includes `GXImmediate3DS.hpp`, so `GXLoadPosMtxImm` is `TDDraw::loadDirectPosMtx` before `drawSimpleModel`.
- `Overwrite.cpp`: superseded by "Particles drawn as the Wii draws them" (Oct 28 2026) below.
- `BigBubbleDrawer.cpp`: GX material helpers Wii-only.
- Platform: `intrinsics.cpp` also exports `__frsqrte` with C linkage.
- Not ported in phase 4: `MarioShadow` volumes (since done with the stencil, block 4) and the `NormalMapBase` indirect normal map (PICA has no indirect textures; the model draws with its material, block 13).

## Block 3 (Oct 22 2026): nw4r layout materials with their TEV

- `lyt_material.cpp`: `Material::SetupNativeTev` (3DS) gives the Wii `SetupGX` as data: channel and material colour, swap tables, TEV registers and konst colours, the material's TEV stages or nw4r's default stages for its texture count (plus the C0/C1 and raster stages), compiled into Citro3D combiners by `rvlTevCompile` (`platform/3ds/gx/tev_compile.cpp`, the same rules as `pixel.cpp` through `tev_terms.h`). Textures come from `rvl3DSGetNativeTexture`, coordinates from the texgen source and SRT matrix. The 3DS `SetupGX` calls it; the dead `__3DS__` blocks inside the Wii branch are gone.
- `common.h` / `lyt_common.cpp`: `detail::NativeQuadMaterial` and `SetNativeMaterial` replace `SetNativeTexture`/`ClearNativeTexture`; `DrawQuad` draws its two triangles with the material's combiners, UVs per unit and material colour.
- `DirectDraw.hpp/.cpp`: `TDDraw::drawStateTriangles` (clip transform, given Citro3D state plus the direct blend/depth/alpha-test state).
- Platform: `tev_terms.h` constants of 0/255 are flagged zero/one (simpler lerps); a lerp between two constants driven by the previous result is split into modulate + add/subtract (one constant per combiner).
- Not covered: TextBox (CharWriter) keeps its own path.

## Block 6 (Oct 22 2026): no J3D display lists generated on 3DS

- `J3DPacket.cpp`: `J3DDisplayListObj::newDisplayList`/`newSingleDisplayList` allocate nothing, `single_To_Double` copies nothing; `beginDL`/`endDL` point GD at a scratch buffer (a writer left running cannot touch freed or file memory).
- `J3DMaterial.cpp`: `makeDisplayList_private`, `patch` (after setting the packet's diff flag) and `diff` write nothing. The native renderer reads the material blocks (`DisplayListMaker::diff` still snapshots them per model).
- `J3DShape.cpp`: `makeVcdVatCmd` only runs `makeVtxArrayCmd`'s vertex format/flag part (the vertex types and fractions are read by native skinning); no GD commands.
- `J3DModelData.cpp`: `indexToPtr` does not patch texture pointers into display lists.
- `MarioActorDraw.cpp`: `createTextureDL` leaves an empty holder; `MarioShadow.cpp`: `CollisionShadow::createDL` builds nothing.
- Known gap (already present): Mario's texture switches done through `J3DModelX` texture display lists (`_1C8`) are not represented natively.
- Not changed: geometry display lists from the files (`J3DShapeDraw`, read by `FurShader`), `addTexMtxIndexInDL`; the GD/J3DGD code itself stays compiled until phase 7.

## Block 7 (Oct 22 2026): GX state calls out of the 3DS game code

On 3DS every GX call already had no effect (the FIFO interpreter discards it); they are now out of the 3DS target, so that the GX SDK can be unlinked (block 8). All under `#ifndef __3DS__` (or `#ifdef __3DS__ … #else … #endif` where a 3DS branch returned early and the Wii code followed it):
- Bodies made Wii-only: `LightFunction` light loads (lights reach native draws as `ActorLightInfo`), `ElectricRailShadowDrawer::loadModelDrawMtx`, `ElectricRailHolder::initDraw`, FlexibleSphere `shapeCallBack`, `FallOutFieldDraw::setUpFillScreen/setUpEdgeAndClearAlpha`, `MiiFacePartsHolder::setTevOpa/setTevXlu`, `ShapePacketUserData::init/loadTexMtx`, `CollisionShadow::draw1/sendVtx/sendZsortedVtx`, `DrawSyncManager::pushBreakPoint`, `MainLoopFramework::prepareCopyDisp`, `JPAGenTexCrdMtxAnm`, nw4r `SetIndTexMtx` and `SetVertexFormat`, and the NormalMapBase GX material functions (`loadTexMtxBtk`, `updateLightMtx`, `shapeAnalyzeDraw`, `loadDiffuseGradTex`, `loadNormalTex`, `drawMaterialSetting`, `drawSetting`, `setTevForObject(_Material)`, `setTevForDebug`, `drawSettingForDebug`, `indirectCapture`, `loadDirectLightTex`, `drawSettingForCapture`).
- Single statements made Wii-only: GameScene `drawMirror`/`draw3D` colour/alpha update, dst alpha and dither; GhostPlayer and MarioActorDraw (`drawMarioModel`, `drawReflectModel`) update/dst-alpha/z-offset; `CollisionShadow::drawAndCaptureTex` projection/viewport/scissor/update state; DrawSyncManager breakpoints and draw-sync callback; MainLoopFramework flushes and clear-Z texture object; Overwrite JPA projection texture matrices; LayoutUtil cull/Z; TextBox alpha compare; ResourceHolderManager's GD current-list restorer.
- Wii code after a returning 3DS branch put under `#else`: ClipArea, `MarioActor::createTextureDL`, `CollisionShadow::createDL`, `J3DModelData::indexToPtr`, `J3DMaterial::makeDisplayList_private/diff`, `J3DDisplayListObj::beginDL/endDL`, `J3DShape::makeVcdVatCmd`, `CharWriter` print, DirectDraw `close`/`setup`, nw4r `DrawQuad`, `TexMap::Get`.
- Data paths given a 3DS form (JUTTexture3DS.hpp, jut_texture.cpp): `rvl3DSTexBufferSize` replaces `GXGetTexBufferSize` in `JUTTexture(int, int, GXTexFmt)`; `JUTTexture::initTexObj` now records the texture in its `GXTexObj` (`rvl3DSInitTexObj/LOD`) and nw4r `TexMap::Get/Set` read and write that record, so `TexMap(texture.getTexObj())` (LayoutUtil, LayoutManager, MiiSelectIcon) gets the real texture (before, the object was left empty on 3DS).

## Block 8 (Oct 22 2026): the GX interpreter and the GX/GD SDK leave the 3DS target

- Platform: `platform/3ds/gx/{processor,fifo,pixel,texture,pe,capture,draw}.cpp` and `gx_capture.h` deleted; `tools/3ds/configure.py` no longer builds `src/RVL_SDK/gx` or `src/RVL_SDK/gd`. `native_submit.cpp` holds the native draw submission (the native part of the former `buildGpuTransform`), the profile counters, the frame-phase and category timers and `rvl3DSEndFrame` (the "GX frame" log line keeps its form for the test suite). `gx_state.h`/`gx_internal.h` keep only the native renderer's declarations. `jut_texture.cpp`: screen captures are recognised by its own capture registry; `os_core.cpp`: cache flushes no longer notify a GX vertex cache; `vi.cpp` defines `GXNtsc480Int` (JUTException); `debug.c` no longer reports a GX parser state.
- Decomp (all `__3DS__`): `MainLoopFramework::endGX` flushes nothing; J3D bodies made Wii-only because they only write display lists or FIFO commands: in `J3DMatBlock.cpp` the colour/texgen/TEV/indirect/PE blocks' `load`, `patch*`, `diff*` (TEV blocks' `diffTevReg`, `diffTevStage`, `diffTexCoordScale`, `diffTevStageIndirect` included), `ptrToIndex`, `indexToPtr_private`; in `J3DTevs.cpp` `J3DLightObj::load`, `loadTexCoordGens`, `loadTexNo`, `patchTexNo_PtrToIdx`, `J3DTexMtx::loadTexMtx/loadPostTexMtx`; in `J3DShapeMtx.cpp` the matrix `load*`/`loadNrmMtx` functions and the concat-view `load`s (`J3DDifferedTexMtx::loadExecute` keeps computing matrices for native.cpp, without loading them); `J3DSys::loadPosMtxIndx/loadNrmMtxIndx`; `J3DLoadCPCmd`; the whole of `J3DGD.cpp`; `J3DDisplayListObj::beginDL/endDL/endPatch` do nothing.

## Block 9 (Oct 22 2026): faithful bloom

- `BloomEffect.cpp` (3DS): `preDraw` keeps the scene in `_24` and clears the screen as on the Wii; `postDraw` runs the Wii's chain in frame-buffer tiles (reduction, brightness mask with `_20`, blurs on the rings of `sL1RadAndOfs`/`sL2RadAndOfs` from the `initBlurMtx` matrices, intensity, scene back plus bloom); `initDraw`, `drawTexture` and `blurTexture` have 3DS bodies. `BloomEffectSimple::draw` likewise (mask channel from `mMaskFilter`, threshold `mThreshold`).
- `ImageEffectLocalUtil.cpp` (3DS): `capture` copies the rectangle the Wii copies (from the tile, the texture's size, doubled for the mipmap argument; the last argument clears it); `drawTexture` multiplies the colour by the intensity (alpha for type 2); `blurTexture` samples the Wii's circle of offsets.
- `DirectDraw.hpp/.cpp` (3DS): `TDDraw::drawTextureTaps` (up to three offset samples summed with a weight) and `TDDraw::drawTextureBrightPass` (luminance or channel mask as a steep ramp, x16, the combiners having no comparison); `drawTexturedQuadDirect` samples screen captures through their mapping (they are stored on their side).
- Platform: `rvl3DSCaptureJUTTextureRegion` and `rvl3DSMapJUTTextureCoord` (jut_texture.cpp) keep each capture's coordinate mapping.

## Block 4 (Oct 23 2026): shadows with the stencil

Where the Wii counts or masks in the destination alpha, the 3DS frame buffer alpha holds the native materials' alpha, so the count goes in the stencil (`GPU_RB_DEPTH24_STENCIL8`, cleared with the depth at each frame start).
- Platform: `HostGfxState` gains a stencil test; `hostGfxSetStencilAmbient` gives draws that set none a stencil (gfx.c). `TDDraw::setDirectStencil` (DirectDraw). `RvlGXDelta` (GXDelta3DS.hpp, gx_delta.cpp): GX state changes as data, applied by the native renderer over a material (`rvlNativeSetDelta`; cull, Z, alpha compare, blend and updates, destination alpha, a single TEV stage compiled with `rvlTevCompile`, TEV registers, channel control and colours, fog, a texture map's image, stencil). `rvlNativeSetViewport` (projection folded with a `GXSetViewport` rectangle, scissor). `rvlNativeDrawSimpleModelSlots` (a matrix per position-matrix slot). Native materials whose texture image is a screen capture's (`rvl3DSGetNativeTIMGCapture`) sample it through its mapping (texfinal rows).
- `J3DModelX.cpp`: `drawIn` builds the sixteen mode lists of the constructor as `RvlGXDelta`s (same calls and values; lists 3, 10, 15 use the stencil; 6-9 unsupported, draw skipped) and merges the dynamic list and the material texture list (both hold an `RvlGXDelta` on 3DS); special modes draw without a `J3DModelXVisibleDraw` scope.
- `GameScene::draw3D`: the destination-alpha-0 window (start of the 3D scene to the lens flare token) resets the stencil wherever something is drawn (`hostGfxSetStencilAmbient`).
- `ShadowVolumeDrawer::draw`: back faces increment, front faces decrement the stencil (saturating, as the clamped alpha), colour off; the shared `setupShadowVolumeDraw` state stays for the next drawer (no `closeDirect`).
- `DrawUtil.cpp`: `fillSilhouetteColor` fills black 0x80 (SRCALPHA/INVSRCALPHA) where the stencil is not 0, resetting it.
- `VolumeModelDrawer::draw`: the real mesh (`MR::drawSimpleModel`) in the Wii's four passes (clear, back faces mark, front faces clear, colour added once) with the stencil; the bounding box approximation is gone.
- `ShadowVolumeFlatModel`: `loadModelDrawMtx` computes the root and drop joint matrices into their slots and `drawShape` draws the mesh with them (was skipped).
- `MarioShadow.cpp` (`CollisionShadow`): `drawAndCaptureTex` saves the frame buffer corner, draws the model from the light (mode lists 1 and 14 plus a colour change: colour and alpha alike, as the I8 texture reads), masks it with the circle, copies it into `_300` (`rvl3DSCaptureJUTTextureRegion`) and restores the corner; `draw1` projects the polygons (GEQUAL, alpha > 1, black by the texture's alpha); `draw2` marks the floor inside the box in the stencil (`drawVolumeBox`) and blackens it under the texture; `draw3` is drawn as `draw2` with the sorted, offset polygons (the Wii's per-polygon alpha refinement is not reproduced); `drawVolume` uses mode list 10.

## Block 10 (Oct 23 2026): screen captures with their rectangles

- `DirectDraw.cpp`: every function that samples a `JUTTexture` (textured, coloured, tinted, colour-lerp, masked, J2D picture, particle triangles, and the immediate path's textures) goes through `rvl3DSMapJUTTextureCoord`, so screen captures (stored on their side) are sampled right. `drawTextureTaps` sums the taps' alpha as their colour. New `TDDraw::drawTextureStages` (a texture over a rectangle with given combiner stages, optionally on the far plane).
- `ClipArea::draw`: the shape colour reaches the vertex colour as 0xRRGGBBAA (it was passed as 0xAABBGGRR, so the count added 0), and the volume keeps `setupShadowVolumeDraw`'s `GX_GEQUAL` (it was `GX_LEQUAL`, which counts nothing for a closed volume) and state for the next areas. `ClipAreaHolder` (3DS) clears the alpha in the pre-draw: the Wii's earlier draws leave it at 0 (destination alpha 0).
- `FallOutFieldDraw::draw`: the Wii's steps with the same captures, tiles and blend modes (mask from the clip areas' alpha as a x64 ramp, its 5-tap sum, the edge colour blended, the fill on the far plane); the TEV each step sets is given as combiner stages. `capture(texture, 0, 1, ...)` is taken as division 1, tile 0 (Broadway's divide by zero gives 0).
- `MarioActorSpecialDraw.cpp`: `captureScreenBox` copies the box and clears it (colour 0, depth far), `writeBackScreenBox` puts it back where the reflection left the alpha at 0 (INVDSTALPHA, DSTALPHA) at depth 0.9999; `drawRasterScroll` and `drawColdWaterDamage` redraw each row shifted from one copy of the screen; `drawDarkMask` marks the noise texture's pixels in the stencil (the texture is re-read each frame), which J3DModelX's list 15 tests. Not done: `drawWallShade` (noise shade on the wall during the bee's wall walk) and the bee wall-walk destination-alpha box in `drawScreenBlend` keep their former 3DS forms.

## Block 11 (Oct 23 2026): Mario's reflection, blur and indirect model

The display lists Mario's models call after their material hold, on 3DS, the same GX changes as an `RvlGXDelta` (block 4), which `J3DModelX::drawIn` applies:
- `MarioActorSpecialDraw.cpp`: `updateAlphaDL` (texture colour with alpha from TEVREG2, blended), `updateSimpleAlphaDL` (destination alpha), `updateReflectAlphaDL` (stage-0 alpha from TEVREG2: the draw's final alpha), `updateLightDL` (ambient and material colour of COLOR0A0, linear fog with the Wii's start/end; channel 1 not used natively), `createRainbowDL` (the blur's TEVREG0 colours; buffers sized for the delta).
- `MarioActorDraw.cpp`: `createTextureDL` records the eye-lid texture swap (`GDSetTexImgPtr` of map 0) as a delta, so `swapTexture` reaches the native draw (closes the gap noted in block 6); `drawIndirectModel` draws in a `J3DModelXVisibleDraw` scope like `drawMarioModel`.
- With the mode lists of block 4, `drawReflectModel` (lists 5, 2, 11 with the reflection view and the alpha list), `drawModelBlur` (list 12 with the rainbow colours over the blur matrices) and the indirect model draw natively. Not reproduced: `GXSetZScaleOffset(1, 0.00001)` of the reflection's depth pass, and `J3DModelX`'s shape replacement (`_12C`).

## Block 12 (Oct 23 2026): Mii heads, THP movies, ODH

- `MiiFacePartsHolder::drawExtra`: no face library draw settings or TEV on 3DS; each head is drawn through `MiiFaceParts::drawOpa` with no setting, which reaches `RFLDrawOpa` (`platform/3ds/rvl/rfl.cpp`): the Mii's icon on a square facing the camera, at the head's place (the face library's part shapes and textures are not available to a 3DS application).
- `MoviePlayingSequence::exePlayStart`: there is no THP video decoder yet (plan K), so the movie never starts playing and the sequence would wait forever (new file prologue, final battle, endings). It forces the wipe closed and goes to `EndWait`, as `trySkip` does for FinalBattle.
- ODH: unchanged (the capture works; there is no Message Board on 3DS).

## Block 13 (Oct 23 2026): hardware limits

`platform/3ds/HARDWARE_LIMITS.md`: the depth buffer as a texture (`DepthOfFieldBlur`, `ScreenBlurEffect`) and indirect textures (water refraction, `NormalMapBase`, fur, heat haze, indirect J3D/JPA/layout materials), with what the 3DS draws instead and possible later approximations. No code change.

## Fixes (Oct 23 2026, Good Egg test)

- `FurDrawer::createFurMap`: the IA8 texels are byte-swapped on 3DS, so they hold alpha then intensity as the Wii's big-endian `u16` stores them (the intensity was read as the alpha: every fur layer opaque, Good Egg's Dino Piranha planet a green block).
- `GameScene::isValidScenarioOpeningCamera`: false when the debug `stage=` option starts the stage (`opening=1` keeps the camera), for faster tests.

## Fixes (Oct 23 2026, CIA 0.11.0: lights and Mario's shadow)

- `LightFunction::loadActorLightInfo` (3DS): records the loaded light (`rvlNativeLoadLight`, `platform/3ds/gx/native.cpp`), as GX keeps the loaded lights.
- `DrawBufferShapeDrawer::draw` (3DS): calls each packet's light loader where the Wii does and draws every packet with the light loaded last (`rvlNativeLoadedLight`). Packets without a light of their own (map planets, Good Egg's pipe) were drawn with no light at all: the rim effect of their materials (alpha channel lit by light 2, its inverse added as white) whitened them.
- `CollisionShadow::drawAndCaptureTex` (3DS): the circle that masks Mario's silhouette goes from 0xC0 at the centre to 0 at the edge, as the Wii's vertex colours (it was 0xC0 everywhere: a hard-edged, uniformly dark shadow).
- `ut::CharWriter::PrintGlyph` (3DS): the glyph's combiner follows `SetupGX` — the colour mapping (`SetupGXWithColorMapping`: texture from the min to the max colour, times the vertex colour) and the intensity setup of I4/I8 fonts (vertex colour, texture alpha); `TDDraw::drawNativeTexturedTriangles` takes the mode and the two colours. Text boxes with black/white colours (the life meter's number: yellow to orange with a dark outline) were drawn with the glyph's own white and grey.
- `ShadowVolumeSphere`, `ShadowVolumeOval`, `ShadowVolumeCylinder`, `ShadowVolumeOvalPole::drawShape` (3DS): the volume model, as on the Wii (`ShadowVolumeModel::drawShape`, a native draw transformed by the GPU), instead of spheres and cylinders of 24 segments built and transformed on the CPU in both passes (the shadow volume category cost about 94 ms a frame in Good Egg, CIA 0.10.15).
- `J3DModelX.cpp` (`drawIn`, 3DS branch): packets are drawn with `rvlNativeLoadedLight()`, the light loaded last, as GX keeps lights as global state. `SceneExecutor` loads the player's light before `DrawType_Player`. With no light, the rim-light alpha channel was unlit, its inverse was added as white, and Mario's red turned pink (CIA 0.11.8 photo). `Plant.cpp` leaves (`rvlNativeDrawShape`, platform side) use the light loaded last too.
- `J3DModelX.cpp` (`drawIn`, 3DS branch): the native draw begins with the material packet's material (`matPacket->mpMaterial`), as the Wii calls that packet's display list. `MarioActor::copyMaterial` lends the face model (MarioFace) Mario's "Face" material this way. With the face model's own material, texture 0 of Mario's texture table (MarioBody.0, the blue overalls) covered Mario's face. On the platform side, `rvlNativeBegin` reads the record from the .n3d of the model that owns the material (`findMaterialOwner`).

## Performance (Oct 24 2026, CIA 0.12.5)

- `DirectDraw.cpp` (3DS `TDDraw` draws: `drawPrimitiveDirect`, `drawParticleTriangles` and the other textured/coloured triangle draws, 11 functions): the stack vertex array is cleared only for the vertices used (`count`). Before, the whole array was zero-initialised on every call: 8 KB for 192 vertices, 22 KB for 514. A particle quad uses 6 vertices, and about 240 such draws a frame cleared about 2 MB.

## Fixes (Oct 24 2026, CIA 0.12.9: particle textures)

- `JPAResourceManager` (`libs/JSystem/include/JSystem/JParticle/JPAResourceManager.hpp`, 3DS): `getJUTTexture` returns null past `mTexNum`. `load` records the texture it binds to GX_TEXMAP0/1 in `sLoadedTexture` (defined in `JPAResourceManager.cpp`), as GX keeps the bound textures as state, and skips an index past the array.
- `JPAResource::setCTev` (3DS): loads the child shape's texture (GX_TEXMAP1), as the Wii does at its end.
- `JPABaseShape.cpp` particle draws (3DS: billboard, rotated billboard, direction, rotated direction, point and the stripe): they take the texture the Wii samples (`particleTexture`: the last one bound to GX_TEXMAP0, or GX_TEXMAP1 for child particles, status 0x80). Before, they indexed the textures with the particle's own `mTexAnmIdx`. That gave texture 0 for shapes without a texture animation and child particles, and read past the manager's array for some particles: a data abort in `rvl3DSGetJUTTexture` during Good Egg's play, CIA 0.12.8.

## Performance (Oct 24 2026, CIA 0.12.11-0.12.12: actor clipping distance, shadows)

- `ClippingJudge` (`ClippingJudge.hpp/.cpp`, 3DS members `mActorFrustums`, `mActorScale`, `isJudgedToClipActor`, `updateActorScale`) and `ClippingActorInfo::isJudgedToClip` (3DS): actor clipping uses its own volumes. No level reaches past `hostDebugFarClip()` metres (300, `farclip=` in debug.txt, 0 gives the Wii distances). On the Wii the "far max" level (planets, most map objects, `setClippingFarMax`, FarClip -1) reaches the camera's far plane (8 km) and levels 1 to 3 reach 400 to 600 m. Every level is scaled by the host's detail level (`hostPerfDetail`, since CIA 0.14.6: 40-100%, falling while the game runs behind the clock), never under 50 m. A clipped actor does not move, animate, draw or emit effects. `mFrustum` and `mClipFrustums` keep the Wii distances, so the game's on-screen tests (`isJudgedToClipFrustum`, `isJudgedToClipFrustum100m/300m`) are unchanged. Sky and Sun have clipping off and stay.
- `ShadowControllerHolder` (`ShadowController.cpp`, 3DS): the 40 m shadow far clip (`mFarClip`, camera Z) is always on. The Wii turns it on only in AstroGalaxy and the Peach castle stages. A shadow past it casts no collision ray and is not drawn.

## Performance (Oct 24 2026, CIA 0.12.13: dropped frames not drawn)

- `GameSystem::frameLoop` (3DS): `draw()` runs only when `hostGfxFrameDropped()` says the frame will be shown. A frame begun in the vertical blank the last one ended in (the game catching up after a slow frame) is dropped by the host, which shows one frame per vertical blank. The game used to build all of its draws first, 14-17 ms of CPU in Good Egg, and 165 of 2400 frames in the CIA 0.12.10 log were dropped. `endRender`, `update` and `calcAnim` run as before.

## Performance and fixes (Oct 24 2026, CIA 0.12.14: flowers, ocean sphere)

- `PlantGroup::initAfterPlacement` (3DS): flower and grass groups clip at 50 m (`setClippingFar50m`) instead of 100 m. Each member is a model of its own, 7 a group by default. In the CIA 0.12.13 log their draw buffer (NoSilhouettedMapObjWeakLight, CutBushModelObj) cost 2-2.5 ms a frame in Good Egg.
- `OceanSphere` (3DS `draw`, `drawSphere`): the sphere is drawn. The 3DS branch built every strip's vertices (~1 ms a frame, draw type OceanSphere) and never sent them. `draw` runs the Wii's passes:
  - the environment-mapped back with `loadMaterialBack`: C0 · (1 - texture), opaque, cull front;
  - the face with `loadMaterialFace`, translated to three PICA stages: texture 0 · K0, then texture 1 · previous · 2, then C1 · (1 - previous) + previous with alpha A1. That is GX's lerp(C1, C2, previous), as C2 is white in both colour sets;
  - the face is drawn culling front, then culling back with the back colours.
  GX stage 2 (`GX_TEV_COMP_R8_EQ`, adds C0 where the red equals A0) is left out, as the PICA has no comparing combiner. The strips are built once per frame (`sOceanSphereVertices`) and the face's second pass draws them again.

## Performance and profiling (Oct 24 2026, CIA 0.12.15: ocean sphere draws, object timing)

- `OceanSphere::drawSphere` (3DS): the strips go to the GPU as one indexed triangle list a pass (`sOceanSphereIndices`, strip winding kept: odd triangles swap their first two vertices). Drawn strip by strip they were 152 draws a pass and two or three passes a frame: in the CIA 0.12.14 log the OceanSphere draw type cost up to 885 ms per 100 frames (about 9 ms a frame) in Good Egg.
- `NameObjCategoryList::execute` (3DS): with the profile log on (`profile=1`) and a scene category running, each object's call is timed (`rvlGXObjectMark`, `rvlGXObjectEnd` in `platform/3ds/gx/native_submit.cpp`). The log's new "Game objects ms" line lists the twelve costliest objects by category and name. Off, the loop is the Wii's.
- `SceneFunction.cpp` `CategoryTimer` (3DS only): passes its kind and type to `rvlGXCategoryMark`, which keeps the running category for the object timing.

The native model files are version 7 (`platform/3ds/gx/native.cpp` `kVersion`, `tools/3ds/native/n3d.py`). The converter (`mat.py`) now reads each model's register animations (`.brk` in the same archive): a TEV colour register a material adds is converted with the signs its animated values take. Before, the file's own value decided, and Luma's body (`Body1_v`: texture lerp + C0, C0 animated by `ColorChange.brk` with values mostly below zero) added a positive constant and drew white. Older `.n3d` files are refused and must be converted again.

## Particles drawn as the Wii draws them (Oct 28 2026, CIA 0.13.7: JPA equivalence)

The JPA draws keep the Wii's code; where they send GX their vertex arrays, display lists, matrices and TEV state, the 3DS branches hand the same values to `platform/3ds/gx/particle.cpp` (`JPA3DS`, `platform/3ds/include/JPADraw3DS.hpp`). It builds the vertices and texture coordinates GX would and draws them with the combiners of `platform/3ds/gx/particle_tev.inc`, generated and checked against the Wii TEV by scripts of the development repository (3159 of 3327 effects the same; 162 use the indirect texture, which the PICA lacks; 6 use an alpha test on a range, reduced to its lower bound).
- `JPABaseShape.cpp` (3DS):
  - `JPASetPointSize`, `JPASetLineWidth`: the sizes go to `JPA3DS::setPointSize` / `setLineWidth`.
  - `JPAGenTexCrdMtxIdt/Anm/Prj`: `JPA3DS::setTexGen`.
  - `JPAGenCalcTexCrdMtxAnm`, `JPALoadCalcTexCrdMtxAnm`, `loadPrj`, `loadPrjAnm`: computed as on the Wii, their matrix loaded with `JPA3DS::loadTexMtx`.
  - `JPALoadPosMtxCam`: `JPA3DS::loadPosMtx`.
  - `JPADrawBillboard`, `JPADrawRotBillboard`, `JPADrawRotation`: `loadPosMtx`, the Wii's `p_prj`, then `JPA3DS::callDisplayList` of `jpa_dl` or `p_dl[mDLType]`. `JPADrawQuad3DS` and `particleTexture` are gone.
  - `JPADrawPoint`, `JPADrawLine`: `JPA3DS::begin/vertex/end` with the Wii's vertices.
  - `JPADrawStripe`, `JPADrawStripeX`: empty on the 3DS; `Game/System/Overwrite.cpp`'s versions are the ones linked. `drawDirectStripe` is gone.
  - `setGX`: the alpha test goes to `JPA3DS::setAlphaCompare`, which reduces the two comparisons to one exactly over the 256 alpha values (a range keeps its lower bound); the colour and alpha modes to `JPA3DS::setModes`.
- `Game/System/Overwrite.cpp` (3DS): `loadPrj` and `loadPrjAnm` call `JPA3DS::loadTexMtx`. Direction, RotDirection, DBillboard, YBillboard and RotYBillboard draw through `loadPosMtx` + `p_prj` + `callDisplayList`. `JPADrawLine`, `JPADrawStripe` and `JPADrawStripeX` are built on the 3DS too, with each GX call replaced by `JPA3DS` (the vertex descriptor calls stay Wii-only).
- `JPAResource::setPTev` (3DS): `JPA3DS::setArrays` with `jpa_pos`/`jpa_crd` at the Wii's offsets (scale centre, base plane, tiling) and `setExTex` (second texture, indirect). `setCTev`: the child arrays, no extra texture, identity coordinates, then the child texture as before.
- `JPALoadExTex` (`JPAExTexShape.cpp`, 3DS): loads the indirect texture into GX_TEXMAP2 and the second texture into GX_TEXMAP3.
- `JPAResourceManager` (3DS): `sLoadedTexture[4]`, recording maps 0 to 3.
- `TDDraw::drawParticleTriangles` (`DirectDraw.hpp/.cpp`): takes the combiner stages, two textures and two sets of coordinates. New `TDDraw::directViewUnitsPerPixel` sizes points and lines.

Fixed by this: quads were 25 times too small, the texture was flipped vertically, the scale centre and tiling were ignored, the XZ plane was drawn on X/Y, texture coordinate animation (708 effects) and projected textures (192) were ignored, stripes (214) were flat bands, point and line sizes were ignored, the second texture (279) was not drawn, and the alpha test kept only its first comparison.

## Image effects in frame buffer pixels (Oct 28 2026, CIA 0.13.9)

- `ImageEffectLocalUtil::setupDrawTexture` (`src/Game/Screen/ImageEffectLocalUtil.cpp`, 3DS): DirectDraw view mode 3 with the Wii's orthographic projection in frame buffer pixels (0 to `getFrameBufferWidth`, 0 to `getFrameBufferHeight`) and an identity view. View mode 2 is in screen units (832 wide in 16:9). With it, the image effects' rectangles (in frame buffer pixels, like their captures) covered 640 / 832 of the width. On camera cuts `CameraCover` showed the captured screen squeezed into the left 77% with black beside it. The bloom, blur and depth of field tiles were misplaced the same way.

## Movies on the New 3DS video decoder (Oct 28 2026, CIA 0.13.10, block K)

The THP movies are converted by `tools/3ds/builder/movie.py` (builder `build_files.py` `convert_movies`, converter `convert_movies_in_place`, with ffmpeg). The output keeps THP's layout, little-endian, version 0x3D50001. Its video is H.264 baseline, 400x194 in 512x256, one picture every two frames, for the New 3DS decoder (`platform/3ds/host/movie.c`, MVD). Its audio is the THP's DSP-ADPCM, unchanged. All nine movies take about 86 MB, against 2.3 GB.
- `THPDrawNative.cpp` (3DS), the game-side glue:
  - `THPNativeIsMoviePlayable`: a New 3DS and a converted file.
  - `THPNativeMovieBegin`, `THPNativeMovieDecode`, `THPNativeMovieEnd`: the decoder.
  - `THPNativeAudioActive`: the sound output runs.
  - `THPNativeDraw`: draws the last picture over the Wii's polygon, in frame buffer pixels. The CPU YUV conversion is gone.
- `THPSimplePlayerWrapper.cpp` (3DS):
  - `endReadHeader` accepts the 3DS version.
  - `tryFinishDvdOpen` starts the decoder from the video info's type word (the encoded size); `close` and `quit` stop it.
  - `decode` passes the video component's size to `videoDecode`, which sends the NAL units to the decoder (an empty component keeps the picture).
  - Without sound output, the audio component is left out: the mix callback that empties its buffers never runs, and the movie would stop after 20 frames.
- `MoviePlayingSequence::exePlayStart` (3DS): skips the movie, as before, only when `THPNativeIsMoviePlayable` fails (an Old 3DS, or a file not converted).
- `TDDraw::drawHostTexture` (`DirectDraw.hpp/.cpp`, 3DS): a host texture over a rectangle.
- `platform/3ds/rvl/compat.cpp` `THPInit` returns TRUE.
- `platform/3ds/host/audio.c` gains `hostAudioActive`.

## Effects drawn by object code, checked against the Wii (Oct 28 2026, CIA 0.13.11)

A development script (not published) lists every draw function outside JPA with the Wii branch's GX state and the 3DS branch's draws (52 functions in 47 files). The 13 seen most often were compared stage by stage. New `TDDraw::drawStageTriangles` (`DirectDraw.hpp/.cpp`, 3DS) draws with given combiner stages, each with its own constant, and a vertex colour (or one per vertex); `TDDraw::stageConstant` packs a colour for it. The effects now translate their Wii TEV into it.
- `WarpPod::drawCylinder`: the alpha is 1, as the Wii's `(TEXA * K + 1/2) * 2` clamps. It was the texture alpha, which faded the additive cylinder.
- `MarioFoo::draw3D` (Red Star beam and trail):
  - colour `lerp(C0, C1, TEXA)` and alpha `TEXA * A0 * 2`, as on the Wii; it blended toward white before;
  - the camera view (view mode 0); it was mode 1, which kept the previous view;
  - the trail's own texture coordinates;
  - the trail colour packed as (255, 0, 0, alpha); before, the alpha went into red.
- `ClipFieldFillDraw::draw`: colour C0, and alpha = (TEXA > 1/255 ? A0 : 0), or the reverse, as the Wii's comparing stage. It is rebuilt with five combiners: subtract, ×4 four times (saturating), then times A0. It used to blend toward white with the texture alpha.
- `TrampleStar::drawSelf`: lit per vertex on the CPU as the Wii's channel. COLOR0 is ambient (0x7F, 0x7F, 0x93) + 0xCC · max(N·L, 0) · the actor light's first light. ALPHA0 is 0xC3 + max(N·L, 0) · alpha 2, from the camera. Then the Wii's two stages (RASC, K·RASA·2; CPREV·TEXC + APREV, TEXA). Before, it was the texture only. Mario's shadow on it (stage 2) is not drawn.
- `MarioActor::drawSpinEffect`: the Wii's line widths (36 and 18 sixths of a pixel).
- `MarioActor::drawWallShade`: drawn (it was empty): a black fan with alpha TEXA · RASA where the depth test passes GREATER.
- Found the same as the Wii: GCaptureRibbon, SpinDriverPathDrawer, DashRing, ClipAreaDropLaser, MarineSnow, FootPrint, drawSphereMask.
- Still different: KirairaChain (a grey band instead of the chain link model at each point, which needs a native model draw per instance); the search light (`drawSearchLight`, a translucent box instead of the cone counted in destination alpha, which needs the cone model drawn into the stencil).

## Color8 from a u32 (Oct 31 2026)

- `Color8(u32)` (`include/Game/Util/Color.hpp`, 3DS): sets the bytes from 0xRRGGBBAA. Stored whole, a little-endian u32 gave them in reverse: every effect colour of `AutoEffectList` came out with red 0, and green and blue swapped.

## Full screen blur without the Wii's off-screen area (Oct 31 2026)

- `MR::drawFullScreenBlur` (3DS): no small copy of the screen (on the Wii off screen, here it would be visible) and no last frame's copy (`nonFilteredCapture` does nothing here, its texture was black): the screen texture is drawn again at the second offset.

## Unchanged poses are not recalculated (Oct 31 2026)

- `ModelManager::calcAnim`, new `isPoseUnchanged` (3DS): `calc` is skipped for a model at the place and scale of its last calculation with no skeleton or visibility animation, joint callback, deformation or calculation callback. Materials are still calculated.

## The Wii's draw distances on the New 3DS (Oct 31 2026)

- Asked by the owner. These now apply only with 96 MB (`hostLowMemory`): actor clipping's limit and scaling (`ClippingJudge`, through `hostDebugFarClip`, default 0 otherwise), early lower detail models (`LodCtrl::update`), the 40 m shadow far clip (`ShadowControllerHolder`), and dropping far small draws (`native_clip.cpp`).
- Not distances, unchanged: fewer particles and spin driver ribbon points while behind the clock.

## Plain J3D draw buffers drawn natively (Oct 31 2026)

- `J3DMatPacket::draw` (3DS): `rvl3DSDrawMatPacket` (`DrawBuffer.cpp`), the native renderer for packets of a plain `J3DDrawBuffer`. Only `ScenarioSelectScene` has such buffers: its stars and sky were entered and never drawn.

## Scenario selector on dropped frames (Oct 31 2026)

- `ScenarioSelectScene::calcViewAndEntry` (3DS): empties its draw buffers first, and enters nothing on a frame the host will not draw. A frame that is dropped has no draw to empty them, and a packet entered twice pointed to itself.

## Dome orbits and the scenario selector's camera (Oct 31 2026)

- `AstroDomeOrbit::drawCelling`, `drawSide` (3DS): strips of 130 vertices instead of 384 as triangles.
- `CameraUtil.cpp` (3DS): `rvl3DSSetCameraContext`, an override of `getCameraContext`.
- `ScenarioSelectScene::draw` (3DS): sets its own camera as the override while it draws.

## Mario on undrawn frames (Oct 31 2026)

- `MarioAnimator::calc` (3DS): with `gRvl3DSSkipJointCalc`, no joints; `_88` of the players is cleared so the animation advances. The first calculation of an animator is always made.
- `ModelManager::updateDL` (3DS): nothing with `gRvl3DSSkipJointCalc`.
- `GameSystemObjHolder::update` (3DS): `SysTimer3DS` around each of its four calls.

## Profile timers in Mario and collision (Oct 31 2026)

- `SysTimer3DS` (3DS, active only with the profile on) at the start of `Mario::update`, `checkAllWall`, `updateGroundInfo`, `MarioActor::updateBehavior`, `controlMain`, `CollisionCategorizedKeeper::checkStrike*`, `PlanetGravityManager::calcTotalGravityVector`, `SensorHitChecker::movement`, `HitSensorKeeper::doObjCol`.

## Shadows on undrawn frames (Oct 31 2026)

- `ShadowControllerHolder::movement` (3DS): with 96 MB, a frame the host will not draw leaves the shadow requests in the list for the next drawn frame.

## Square roots without the estimate (Oct 31 2026)

- `TUtil::sqrt`, `sqrtInline`, `inv_sqrt`, `TQuat4::normalize`, `JMASqrt` (3DS): the Wii refines the `frsqrte` estimate with a Newton step; the ARM's `vsqrt` is exact, so the step and the division are skipped.

## Undrawn frames on the Old 3DS: no joints (Oct 31 2026)

- `GameScene::calcAnim` (3DS): with 96 MB (`hostLowMemory`), on a frame the host will not draw, `gRvl3DSSkipJointCalc` is on during the scene's calcAnim list.
- `LiveActor::calcAnmMtx` (3DS): with the flag on, the base matrix is still set; joints and materials are skipped, unless the actor has collision and an animation player. Direct calculations outside the list (`MR::calcAnimDirect`) are not affected. Mario's own `calcAnim` does not pass here.
- `ModelManager::calcAnimSkipped`, `mCalculatedOnce` (3DS only): clears `XanimePlayer::_88` so the animation frame still advances; a model never calculated is calculated.

## Mario's shadow on the Old 3DS (Oct 30 2026)

- `MarioActor::drawShadow` (3DS): with 96 MB (`hostLowMemory`) the silhouette is captured every third drawn frame; the captured texture stays between them.

## Scene archives that do not fit the file cache (Oct 30 2026)

- `FileLoaderThread` (3DS): before loading a file bound for the file cache, checks its real size; if it does not fit, the file goes to the scene heap with the most room, as `MR::getAproposHeapForSceneArchive` sends the later ones.

## Particles and text in batches (Oct 30 2026)

- `TDDraw::drawParticleTriangles` and `drawParticleDistortedTriangles` (3DS): a colour per vertex instead of one `prm`; `platform/3ds/gx/particle.cpp` keeps the particles until a texture or a mode changes.
- `JPABaseShape::setGX` and `JPAEmitterManager` draw (3DS): `JPA3DS::flush` before the direct state changes and before `closeDirect`.
- `CharWriter::PrintGlyph` (3DS): while `TextBox::DrawSelf` has the batch open (`rvl3DSGlyphBatch`), the glyphs wait for one draw. `TextWriterBase< wchar_t >::PrintImpl` (`CustomTagProcessor.cpp`, 3DS) flushes before and after a tag.

## Old 3DS memory (Oct 30 2026, block R)

- `GameSystemSceneController::initializeScene` (3DS): with 96 MB (`hostLowMemory`) the file cache heap is 8 MB instead of 16.25; archives that do not fit already go to the scene heap (`MR::getAproposHeapForSceneArchive`).

## Memory budget measurements (Oct 28 2026, block R: Old 3DS)

- `HeapMemoryWatcher::destroySceneHeap` (3DS): before destroying the scene heaps, logs the KB they used (and their size) and the file cache's. They are solid heaps, so the use at the end of the scene is the peak. The numbers size the emulated MEM2 for the Old 3DS's 96 MB.
- The log is written by a shared `reportSceneHeaps`, called from `destroySceneHeap` and `destroyGameHeap`. Scene changes go through either one (`GameSystemSceneController`), and the file cache may already be destroyed: it is then reported as 0.

## Collision built at run time (Oct 28 2026)

- `DynamicCollisionObj::createCollision` (3DS): the octree root is written as the two halves of the s32 `0x80000002` in little-endian order (`2`, `0x8000`). In the Wii's order `searchBlock` read `0x00028000`, a node offset outside the array, and the console stopped with a data abort while flying with a Launch Star in Gateway Galaxy (CIA 0.13.15).

## Sun visibility and movie subtitles (Oct 28 2026, CIA 0.14.3)

- `BrightObjBase::drawSphere` (3DS): the second sphere writes the alpha buffer alone, as the first. The Wii keeps colour updates off for both: on the 3DS the second one wrote its green, a green sphere where the sun is (BrightSun), and the alpha the visibility check (`checkVisible`) peeks was never marked 255.
- `MoviePlayingSequence::tryStartSubtitles` (3DS): a subtitle starts once the movie is at or past its start frame, after the last frame looked at (file-static `sSubtitlesFrame`, reset in `exePlayStart`). The console updates the game less often than the movie shows frames, and the Wii's equality test missed subtitles whose start frame went by between two updates.

## Pause button (Oct 28 2026, CIA 0.14.5)

- `PauseButtonCheckerInGame::update` (3DS): the hold before the pause menu opens is counted in 60 Hz retraces (`hostVBlankCount`, at most 8 per update), not in game updates. The console updates the game at 15-30 fps, so the Wii's 12 updates needed START held for up to a second. A count crossing 12 stops on it, as `isPermitTo*Pause` tests for equality.

## Game speed before frame rate (Oct 29 2026, CIA 0.14.6, block T)

The console's game logic costs 12-18 ms of the 16.7 ms retrace, and the game runs one update per loop: a loop over a retrace slows the game down (CIA 0.14.5: 17-50 ms loops, the game at 35-100% of its speed). While the game runs behind, the host draws a frame only every third retrace (20 fps, `gfx.c`), so the undrawn loops catch up, and lowers its detail level (`hostPerfDetail`).

- `GameScene::calcAnim` (3DS): `executeCalcViewAndEntryList` is skipped when the host will not draw the next frame (`hostGfxNextFrameDrawn`). View matrices and draw entries serve only that draw.
- `JPADynamicsBlock::create` (3DS): steady emitters (not fixed-interval ones) create `hostPerfDetail()` times their particles.
- `LodCtrl::update` (3DS, member `mRadius3DS`): while `hostPerfDetail()` is under 1, the LOD distance is the distance from the high model's surface (bounding radius times the largest scale) divided by the detail level, when that is larger than the Wii's distance. Near or on a planet it is not, so the planet keeps its model (the CIA 0.12.12 early LOD was dropped for showing the low model there).

## Hitches and paths (Oct 29 2026, CIA 0.14.8, block T)

- `HeapMemoryWatcher::destroySceneHeap` (3DS): empties the GPU vertex arena (`hostGfxArenaReset`) as the scene's models go. Left full of the previous scene's vertices, it emptied in the middle of the next one and every model there uploaded again (about 4 s entering the Comet Observatory, CIA 0.14.7).
- `SpinDriverPathDrawer::draw` (3DS): each point of a ribbon is three shared vertices drawn as indexed triangles (`TDDraw::drawMaskedTexturedIndexed`, new), not 12 vertices a segment; the paths cost about 6 ms a frame in the Comet Observatory. While `hostPerfDetail()` is under 1, every other point is drawn (the last one always).
- `SceneFunction::executeMovementList` and `GameSystem::update` (3DS, profile log only): kind "sys" timers (`SYS_TIMER`, `rvlGXCategoryMark` kind 6) around the whole movement list (1), `SceneNameObjMovementController::movement` (2), the requirement connect/disconnect calls (3), `GameSystemObjHolder::update` (4), `checkRequestAndChangeScene` (5) and the system nerve where the scene updates (6). In HeavensDoorInsidePlanet about 5 ms of the 10.6 ms update a frame were outside every category (CIA 0.14.7).

## Point light (Oct 29 2026, block U)

- `LightFunction::loadPointLightInfo` (3DS): passes the point light (`PointLightInfo`) to the native draws (`rvlNativeLoadPointLight`, `platform/3ds/gx/native.cpp`), which give it as GX_LIGHT4 to materials whose colour channel enables light 4 (165 materials: Mario's, Luigi's, Bowser's and other bodies, U3 audit). The Wii loads it with `GXInitLightDistAttn` and the view-space position; the native side does the same (the GX SDK's attenuation formula). Before, the 3DS never had this light.

## Particle alpha ranges (Oct 29 2026, block V)

- `TDDraw::drawParticleTriangles` (3DS, `DirectDraw.hpp`/`.cpp`): a last parameter, the stages whose alpha the combiner buffer takes (default 0, as before). `platform/3ds/gx/particle.cpp` uses it when GX's two alpha comparisons pass a range inside 0-255 (the swimming bubbles' GEQUAL 60 XNOR LEQUAL 39, a story demo's thunder): the alpha test takes the lower bound and four more combiners the upper one (a saturating step taken off the alpha). Before, the 3DS drew the range's lower bound only.

## Effects drawn by code (Oct 29 2026, block V part 3)

- `TDDraw::drawTextureBrightPass` (3DS, `DirectDraw.cpp`): the bloom mask compares as the Wii's `GX_TEV_COMP_R8_GT` does: the mask value (luminance with the I8 copy's +16, or one channel for `BloomEffectSimple`'s R8/G8/B8 filters) goes in the alpha and the alpha test keeps the pixels over the threshold, over a black rectangle. Before, a ramp 16 times steeper than the threshold step half-lit the pixels just over it, and the R8/G8/B8 filters took the +16 too. Comments in `BloomEffect.cpp` and `BloomEffectSimple.cpp` follow.
- `TDDraw::drawStageTriangles` (3DS, `DirectDraw.hpp`/`.cpp`): two last parameters, a second texture and its coordinates (unit 1); defaults as before.
- `OceanSphere::draw` (3DS): after each face pass, the pixels where loadMaterialFace's red equals A0 (`GX_TEV_COMP_R8_EQ`) get stage 2's C0: a pass with the red in the alpha, an equal alpha test and the constant difference added (`makeGlintState`). Before, that stage was left out.
- `TrampleStar::drawSelf` (3DS): the Wii's stage 2, Mario's shadow texture at the position through the same texture matrix (and the same early returns), within the player's shadow height.
- `WaterCameraFilter` (3DS): `init` loads the filter texture as on the Wii (the `#ifndef` around it removed; the Wii's code is the same), `initScreenTex` makes an 8x8 texture that names the screen capture, and `draw` is the Wii's pass: the screen captured and drawn back as screen - C0 at the water alpha, moved by the filter texture's indirect offsets taken on the CPU at the corners of a 40 x 30 grid. Before, a flat blue tint up to opaque.
- Comments only (3DS branches): `FurDrawer::setupMaterial` (the indirect offset and the specular channel are never on in the game's data), `J3DModelX.cpp` (which mode lists are skipped and why).

## Report on CIA 0.15.0: ambient, lower detail models, sun flare (Oct 30 2026)
- `DrawBufferShapeDrawer::draw` (3DS): tells `rvlNativePacket` whether a `loadLight` ran since the material. On the Wii the material's display list writes the ambient colour (XF 0x100A), so a batch without a loader keeps the material's ambient, not the last actor light's (SphereAir's alpha 0: the atmosphere's fade).
- `MR::syncMaterialAnimation` (3DS): also calls `rvlNativeSyncMaterials`, so a lower detail model draws with its main model's material snapshots (RosettaMiddle/Low have A1 = 0 in the file).
- `LensFlareDirector::setDrawSyncToken` (3DS): no GX draw sync, so the brightness check runs here, inside a flare area, with the camera of the last finished frame. Before it never ran and no flare appeared.
- `BrightObjBase::checkVisible` (3DS): the depth buffer of the last finished frame against the point's depth, instead of the alpha mark, which later draws overwrite by the time the frame ends. `drawSphere` (3DS) draws nothing now (replaces the entry on it above).

## Models drawn for 2D, depth kept for reads (Oct 30 2026)
- `MR::drawInitFor2DModel` (3DS): the Wii's orthographic projection for native draws (`rvlNativeSetViewport`, reset by `rvlNativeEndFrame`) and the identity view. Before it returned at once and the lens flare models went through the camera's projection.
- `MR::clearZBuffer` (3DS): calls `hostGfxKeepDepthForReads` first, so EFB depth reads (the sun's check, `TalkPeekZ`, `StarPointerPeekZ`) see the scene's depth and not the clear that ends `draw3D`.

## Mario's shadow blended by its texture (Oct 30 2026)
- `CollisionShadow::draw3` (3DS): `setupDirect(1, 1, 0)` instead of `(1, 0, 0)`: the polygons were drawn without blending, so every pixel above alpha 1 (the enlarged halo too) came out solid black. The texture itself was checked on the console (core about 0xC0, halo about 0x1E).

## Text blend and alpha test as on the Wii (Oct 30 2026)
- `nw4r::ut::CharWriter::SetupGX` (3DS): sets the blend mode of the Wii's `SetupGXCommon` (source alpha, inverse source alpha). The text took the previous pane's blend: the life counter's digit was added over the additive `PicHiLight`, gold plus cyan giving white and the dark outline vanishing.
- `nw4r::lyt::TextBox::DrawSelf` (3DS): alpha test always, as the Wii's `GXSetAlphaCompare` there.

## Indirect textures from two samples (Oct 30 2026)
- `JPAResource::setPTev` (3DS): gives the extra texture's indirect matrix to `JPA3DS::setIndirect`.
- `TDDraw::drawParticleDistortedTriangles` (3DS): particle draw with a texture sampled twice, around a mean displacement, and the distortion texture in unit 2.
- `Material::SetupNativeTev` (3DS): a layout's indirect stage on its only texture as the same blend of two samples (PicSmoke).

## Scenario selection camera (Oct 30 2026)
- `ScenarioSelectScene::draw` (3DS): the native draws take this scene's projection through `rvlNativeSetViewport`, as the Wii's `setupCameraMtx` loads it with `GXSetProjection`; reset after them.

- `MR::drawFullScreenBlur` (0.18.4): the darkening the Wii's second layer gives, as a black rectangle (about 74% at alpha 80, measured on Wii footage of the Dino Piranha's roars).
- `GameScene::calcAnim`, `ShadowControllerHolder::movement` (0.18.6): the undrawn loops' savings also on a New 3DS that is behind (`hostGfxFallingBehind`).
- `OceanSphere` (0.18.8): later passes reuse the first's vertices (`hostGfxDrawAgain`).
- `Rock::control`, `Rock::exeMove` (0.18.10): out of view and over 3000 units from Mario, a rock follows its rail with no collisions.
- `Rock::control` (3DS): out-of-view rocks over 3000 units from Mario skip the remaining collision and velocity preparation; `Rock::exeMove` continues their rail motion.
- `TDDraw::drawParticleTriangles`, `drawParticleDistortedTriangles` (0.18.11, 3DS): take clip-space vertices and indices (new `TDDraw::particleVertexToClip`, split from `transformToClip`).
- **Schermo nero nei frame showcase senza draw** (`platform/3ds/host/gfx.c`): i build showcase inviano un primo frame anche durante il caricamento del titlescreen e ripuliscono in nero i frame senza draw, evitando il blu lasciato dal buffer Citro3D non inizializzato o dal clear del gioco. Il build normale non cambia.

## Warp pod paths: one indexed draw, skipped off screen (Nov 4 2026)
- `WarpPod::drawCylinder` (3DS): the same quads from the same points, but each cross-section's four vertices are shared by the quads on both sides of it (240 vertices a path instead of 708 transformed on the CPU) and the path is one indexed draw instead of four of 192 vertices. A path whose bounding sphere is outside the view (`MR::isJudgedToClipFrustum`) is not drawn. The pods' paths cost 192 ms every 100 frames in the Observatory (CIA 0.19.8 log), a fifth of the draw phase.
- `TDDraw::drawStageIndexed` (3DS, new): `drawStageTriangles` for an indexed triangle list of up to 512 vertices, with a static vertex buffer.

## Far models: one draw for a fixed model small on screen (Nov 5 2026)
- `DrawBuffer::drawOpa` (3DS): `rvlNativeFarPrepare(mModel)` before the shape drawers, which builds the far draw's state from the model's main material once and counts the buffer's draw.
- `DrawBufferShapeDrawer::draw` (3DS): a packet goes to `rvlNativePacket` only when `rvlNativeFarPacket` did not take it. An instance whose far model (`.far` next to its `.n3d`, written by the converter, `tools/3ds/native/far.py`) is under 16 pixels of radius on screen (back at 20) is drawn as that one draw at its first packet, and its other packets are skipped. Only models with no joint, colour, pattern or visibility animation, no billboard or weighted vertex and only opaque materials have one; an instance with a hidden shape is drawn in full.

## Captures, lower detail colours, joint callbacks, LOD count (Nov 5 2026)
- `JUTTexture::~JUTTexture` (3DS): `rvl3DSInvalidateJUTTexture(this)`. A texture made later at the same address took the screen capture of the one before it: GalaxyMapController's, made in each scene's heap, showed the scenario selector's galaxy (or Gateway's planet) on the Observatory's map board.
- `GalaxyMapController::init` (3DS): the texture is cleared to black instead of copying the frame buffer while the scene loads (black behind the wipe on the Wii; here the last 3D frame left in the render target).
- `LiveActor::calcAnmMtx` (3DS): a model with a joint callback this loop (JointController) is calculated in the loops that are not drawn too. Dino Piranha's tail nodes read their joints' places and directions back every loop; a loop or two old, the tail shook and stretched.
- `LodCtrl::update` (3DS): counts the instances showing their high, middle and low model (and hidden) for the periodic log (`gRvl3DSLodShown`).

## Mario's parts timed (Nov 5 2026)
- `MarioActor::controlMain` and `MarioActor::updateBehavior` (3DS): their calls grouped under `SysTimer3DS` 50-58 (`SysTimer3DS.hpp` lists them), printed every profile window on the log line `Mario parts ms`. Mario's update was about 1.9 ms a loop in Good Egg (CIA 0.19.10), half of it outside `Mario::update`.

## Mario::update phases timed (Nov 5 2026)
- `Mario::update` (3DS): a `SysPhase3DS` marks five consecutive phases (59-63, listed in `SysTimer3DS.hpp`) on the 'Mario parts ms' log line. CIA 0.19.12: `Mario::update` was 90-140 ms of Mario's 110-160 ms every 100 loops, ground 20 and walls 5 of it.

## Mario's rainbow trail in the loops the 3DS draws (Nov 5 2026)
- `MarioActor::calcViewBlurModel` (3DS): a pose is stored once three loops have passed since the last one, instead of when `_37C % 3 == 0`. The function runs only in the loops the 3DS draws (one in three when behind), which can all fall on another remainder: the trail kept its first pose, a white Mario (seven colours added) where the recovery bubble started (CIA 0.19.15).
## Particles of several emitters in one draw (Nov 5 2026)
- `JPABaseShape::setGX` (3DS): blending and depth go through `JPA3DS::setShapeState`, which draws the kept particles first only when they change, instead of `JPA3DS::flush` before every emitter; `JPA3DS::setAlphaCompare` likewise draws them first only when the PICA test changes. Emitters sharing their state and textures join one batch (the 3D particles were about 5 ms a drawn frame in the paused Comet Observatory, CIA 0.20.12).
- `JPAEmitterManager::draw` (3DS): `JPA3DS::beginDraw()` after its direct setup, so the first emitter sets that state again.

## Upstream syncs

- **5 Nov 2026: merged 47 Petari commits (base `6804bd317` to `50c1eae28`).** Conflicts resolved:
  - `OceanSphere.cpp`, `SnowFloor.cpp`, `SnowFloorTile.cpp`: upstream's `};  // namespace` closer kept after our 3DS code.
  - `SpinDriverPathDrawer.cpp`: upstream's `GXCmd1f32` writes on the Wii side of our `#ifndef __3DS__`.
  - `LayoutUtil.cpp`: upstream version with our two guards put back (`__MWERKS__` template instantiation, `__3DS__` GX state).
  - `HashUtil.cpp`: upstream version as is. Its local `tolower` moved to `<cctype>`, so our `__MWERKS__` guard is no longer needed.
  - `README.md`: the port's README stays; upstream's new one is kept, renamed, in the development repository.

  Wii `main.dol` SHA-1 is still `9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd`, and the 3DS build is clean.

## Game data inside main.dol read from the SD card

- **5 Nov 2026.** `src/Game/System/ErrorMessageArchive.cpp`, `GalaxyInfoTable.cpp`, `StoryProgressTable.cpp`: under `__3DS__` they skip `#include "assets/<name>.hpp"`.
  - These are the byte arrays the Wii build extracts from `main.dol`.
  - The 3DS build now defines `cStoryProgressTable`, `cGalaxyInfoTable` and `cErrorMessageArchive` in `platform/3ds/rvl/port_data.cpp`. It fills them at startup from `sdmc:/smg/port/`, which the converter writes from the user's own `main.dol` (SHA-1 checked).
  - **Why:** the CIA contains no Nintendo data and can be shared.
  - `tools/3ds/builder/convert_assets.py` is removed, and `tools/3ds/configure.py` no longer needs the Wii build first.
  - Wii SHA-1 unchanged; the Wii view of the files shows no difference.

## Why the streamed music stops for good (Nov 6 2026)

- `src/JSystem/JAudio2/JASAramStream.cpp`: under `__3DS__`, `report3DSStreamStop` writes to the log, once per place, each of the nine places that set `UNK_BOOL_B`, with their values.
  - `UNK_BOOL_B` is shared by every stream and never cleared. Once it is set, `channelProc` pauses every stream, so the music stops for good while sound effects and voices go on.
  - **Why:** in the CIA 0.21.1 test the music stopped for good at a random moment of play, and the log could not say why.
  - Logging only; behaviour unchanged. Wii SHA-1 unchanged; the Wii view of the file shows no code difference.

## A comment on every 3DS block (Nov 6 2026)

- Every `#ifdef __3DS__` / `#ifndef __3DS__` block of `src/`, `include/` and `libs/` that had no comment got a one-line comment above it saying what the 3DS does there and why (703 blocks in 197 files). The comment sits above the directive, so both builds see only a comment.
- 18 empty `#ifdef __3DS__ ... #endif` blocks (leftovers of earlier edits) were removed.
- `src/Game/Screen/WipeFade.cpp`: a dead copy of `GXSetColorUpdate`/`GXSetBlendMode` under `#ifdef __3DS__` inside the Wii branch (never compiled by either build) was removed; the Wii's own calls stay above it, under `#ifndef __3DS__`.
- Wii SHA-1 unchanged (full Wii rebuild); the Wii view of the 197 files (each file as the Wii compiler sees it) differs in comments only.
