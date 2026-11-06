# Wii effects the 3DS GPU cannot reproduce exactly

The PICA200 (Citro3D) has no equivalent of two Wii GX features the game relies on. This file
lists what each one does on the Wii, what the 3DS port draws instead, why, and what could
bring the result closer later. Other GX features the PICA does have in another form are
handled in the renderer and are not listed: more than 6 TEV stages or 3 textures (multipass,
block 5), destination alpha counting (stencil, block 4), EFB copies (render to texture and
captures, blocks 9 and 10).

## 1. The depth buffer as a texture

**Wii.** `JUTTexture::capture(..., GX_TF_Z8, ...)` copies the top 8 bits of the EFB depth into a
texture, and a TEV stage reads it as a weight.

| User | Wii effect | 3DS today |
|---|---|---|
| `DepthOfFieldBlur` | blur mixed in by distance (far objects blurred, near ones sharp) | three depth-tested rectangles of the blurred screen along the ramp: a ramp in three steps (the depth cannot be read as a texture, the depth test can gate it) |
| `ScreenBlurEffect` | previous frame blended, weighted by depth | previous frame blended uniformly |

**Why.** Citro3D's display transfer only converts colour formats, and a texture unit cannot
take the depth buffer as a depth format.

**Possible later (not verified on hardware).** The D24S8 depth buffer uses the same 8x8 tiled
layout as textures and render targets. Pointing an RGBA8 texture at its VRAM address would
give the depth's top byte in the G channel (bytes in memory: depth bits 0-7, 8-15, 16-23, then
stencil; RGBA8 texels are A, B, G, R). The values would still need remapping: the 3DS depth
is reversed (clip z in [-w, 0], 0 = far) and not distributed like GX's. It needs one test on
the console before any work.

## 2. Indirect textures

**Wii.** An indirect stage reads a texture and uses it to offset the texture coordinates of a
later TEV stage. This gives water refraction, heat haze, normal-map lighting and wavy screens.

**Why.** The PICA has no indirect stage: texture coordinates come only from the vertex shader
(or the fixed texture coordinate generator), never from another texture inside the same pass.

| User | Wii effect | 3DS today |
|---|---|---|
| J3D materials with indirect stages (`n3d` `FLAG_INDIRECT`) | distorted textures | drawn without the offset |
| `OceanRing*`, `OceanBowl`, `OceanSphere`, `WaterRoad`, `WhirlPool` | water refraction of the scene behind | water surface through `WaterRender3DS`, no refraction |
| `NormalMapBase` (and `SimpleNormalMapObj`, `NormalMapTestObj`) | per-pixel normal-map lighting | not drawn (only the Wii draw path reaches it) |
| `FurDrawer` | fur strands bent by the indirect texture | fur layers drawn straight |
| `SpiderThread` | thread shading | drawn without the indirect term |
| `WaterCameraFilter` | underwater screen distortion | colour filter only |
| JPA particles and nw4r layout materials with indirect stages | heat haze, wavy panes | drawn without the offset |

**Possible later.**
- Refraction, heat haze and wavy screens: draw the screen capture on a grid of small quads
  whose texture coordinates are moved on the CPU by sampling the indirect texture at each grid
  point. The result is smoother than the Wii's per-pixel offset, and it costs CPU time per frame.
- `NormalMapBase`: the PICA fragment lighting has bump mapping (a normal-map texture perturbing
  the normal per pixel). This is the closest native match, but it needs the object's lights
  translated into PICA fragment lights.

## Related limits that are not the GPU's

- **Mii 3D heads** (`MiiFacePartsHolder`, file select): the face library's part shapes and
  textures are not available to a 3DS application. The head is drawn as the Mii's icon on a
  square facing the camera (`platform/3ds/rvl/rfl.cpp`).
- **THP movies**: there is no video decoder yet (plan K). `MoviePlayingSequence` skips each
  movie as the game itself skips FinalBattle.
- **ODH (Wii Message Board photos)**: the capture works, but the 3DS has no Message Board to
  receive it.
