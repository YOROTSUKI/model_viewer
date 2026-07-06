# Vulkan Model Viewer

This project is a small Vulkan-based model previewer scaffold for local exported assets.

## Current Status

- Creates a GLFW window backed by Vulkan.
- Initializes Vulkan instance, device, swapchain, depth buffer, render pass, graphics pipeline, command buffers, and frame sync.
- Renders an orbit-camera lit mesh.
- Loads Wavefront OBJ files from the command line.
- Loads Cast `.cast` model files, including mesh geometry, skeleton bones, skin weights, animation clips, animation curves, and notification tracks.
- Provides a Dear ImGui control panel for importing `.cast`/`.obj` files, scanning Apex-style texture folders, and tuning material parameters at runtime.
- Scans Apex/UE Substrate-style texture names into material slots and renders them with an approximate Vulkan PBR material.
- Falls back to an internal cube when no model path is provided.
- Cast animation data is imported into runtime structures; skeletal playback/skinning is not rendered yet.

## Requirements

- Windows 10/11
- CMake 3.24+
- Visual Studio C++ build tools or another C++20-capable compiler
- Vulkan SDK with `VULKAN_SDK` set and `glslc.exe` available

If the current shell does not expose `cmake` or `cl.exe` on `PATH`, run the build from a Visual Studio developer prompt or initialize the MSVC environment with `vcvars64.bat`.

## Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

## Run

```powershell
.\build\vulkan_model_viewer.exe
.\build\vulkan_model_viewer.exe path\to\model.obj
.\build\vulkan_model_viewer.exe exports\horizon_mythic_v25_pilot_level03_w\mdl\Humans\class\medium\horizon_mythic_v25_pilot_level03_w_LOD0.cast
.\build\vulkan_model_viewer.exe --smoke-test path\to\model.cast
```

## Cast Import Probe

`cast_import_probe` validates Cast import without opening a Vulkan window:

```powershell
.\build\cast_import_probe.exe path\to\model.cast
```

## Apex Material Mode

This first pass approximates the Apex Legends UE Substrate material setup without parsing Unreal `.uasset` files or depending on Unreal Engine. It reuses portable texture naming and parameter semantics, then adapts them to the viewer's Vulkan PBR shader.

When a model is loaded, the viewer scans the model directory and nearby texture directories such as `Textures`, `textures`, `images`, `_images`, and `material`. You can also choose a texture directory explicitly with `Apex Folder` in the ImGui panel.

Texture files are grouped by material slot:

- The optional `T_` prefix is ignored.
- The final underscore token is treated as the texture type.
- The name before that token is treated as the material slot name.
- Matching slots are bound automatically to Cast material names when possible.
- Unmatched, duplicate, and missing textures are reported in the console log.

Supported suffixes:

- `*_col` or `*albedoTexture`: Albedo/BaseColor, sampled as sRGB.
- `*_nml` or `*normalTexture`: Normal, sampled as linear, with optional green-channel flip.
- `*_gls` or `*glossTexture`: Gloss, sampled as linear and converted to roughness.
- `*_spc` or `*specTexture`: Specular/F0, sampled as linear.
- `*_ao` or `*aoTexture`: Ambient occlusion, sampled as linear.
- `*_cvt`, `*_cav`, or `*cavityTexture`: Cavity, sampled as linear.
- `*_opa`, `*_msk`, or `*opacityMultiplyTexture`: Opacity/alpha mask, sampled as linear.
- `*_thk`, `*_sctr`, or `*scatterThicknessTexture`: Scatter thickness, sampled as linear.
- `*_asd` or `*anisoSpecDirTexture`: Anisotropy direction/detail, sampled as linear with nearest filtering.
- `*_ilm`, `*_ehl`, or `*emissiveTexture`: Emissive, sampled as sRGB.
- `*_ehm` or `*emissiveMultiplyTexture`: Emissive multiplier, sampled as linear.

Missing maps use neutral defaults: white albedo, flat normal, 0.5 gloss, 0.04 specular, white AO/cavity/opacity, black emissive/thickness, and neutral anisotropy.

Runtime controls are shown in the Dear ImGui `Vulkan Model Viewer` panel:

- The top row has `Import Model`, `Apex Folder`, and `Rescan` actions.
- The `Material` tab controls Apex material mode, normal green flip, roughness/specular/AO/cavity/emissive multipliers, alpha cutoff, subsurface tint/strength/thickness, and anisotropy settings.
- The `Bindings` tab provides model-slot and scanned-texture-slot combo boxes for manual overrides, per-slot alpha controls, plus a material binding table showing which maps and alpha modes were detected per slot.
- The `Log` tab shows the Apex scan log, including detected texture slots, missing maps, and unmatched material slots.

Parameters are saved next to the model as `<model>.apexmat.json`. The same file can include `slotOverrides` to manually map a model material slot to a scanned texture slot:

```json
{
  "enableApexMaterialMode": true,
  "flipNormalGreen": false,
  "roughnessMultiplier": 1.0,
  "slotOverrides": {
    "body_sknp": "horizon_mythic_v25_pilot_level03_body_sknp"
  },
  "slotAlphaModes": {
    "horizon_mythic_v25_pilot_level03_glass_sknp": "Translucent",
    "horizon_mythic_v25_pilot_level03_glow_sknp": "Additive"
  },
  "slotOpacitySources": {
    "horizon_mythic_v25_pilot_level03_glass_sknp": "OpacityTexture"
  },
  "slotOpacityChannels": {
    "horizon_mythic_v25_pilot_level03_glass_sknp": "R"
  }
}
```

Alpha handling is per material slot. Opaque slots ignore opacity maps, masked slots use alpha cutoff, translucent slots use a weighted blended OIT approximation with depth writes disabled, and additive slots accumulate separately for glow/energy style effects. The scanner infers common slot names such as `hair`, `glass`, `visor`, `glow`, `wisp`, and `lightning`; override the mode/source/channel in the `Bindings` tab when the heuristic is wrong.

Use `apex_material_probe` to validate texture detection without opening a Vulkan window:

```powershell
.\build\apex_material_probe.exe path\to\model.cast
.\build\apex_material_probe.exe path\to\Textures
```

For visual comparison, load the same mesh and texture set in Unreal with `UE-Substrate-Material-for-ApexLegends-resource`, then compare base color, normal direction, gloss/roughness response, specular intensity, AO/cavity shadowing, emissive contribution, and the cheap scatter approximation. The goal is visual proximity in this Vulkan previewer, not exact Unreal Substrate parity.

Controls:

- Hold left mouse button and drag to orbit.
- Mouse wheel zooms.
- Click `Import Model` in the ImGui panel to select and hot-load a `.cast` or `.obj` model.
- Click `Apex Folder` in the ImGui panel to scan a texture folder for the current model.
- `Esc` closes the previewer.

## Asset Notes

The checked-in `.gitignore` excludes `exports/` because exported game/model data can be large and project-local. Do not commit proprietary Apex/EA assets. Use local exports, neutral test textures, or generated placeholder textures for validation.

Current limitations:

- Unreal `.uasset` files are intentionally not parsed.
- The shader is an approximation, not a full Substrate implementation.
- Translucent and additive materials use a weighted blended OIT approximation. Exact Unreal/Substrate translucency, refraction, and per-pixel linked-list transparency are not implemented.
- The material descriptor table currently supports up to 16 material slots.
- Skeletal playback/skinning is imported but not rendered yet.
