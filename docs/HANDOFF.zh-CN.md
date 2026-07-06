# Vulkan Model Viewer 交接文档

更新日期：2026-07-06

## 目标与当前状态

本项目是一个自研 Vulkan 模型预览器，用于本地预览导出的 `.obj` / `.cast` 模型，并对 Apex/UE Substrate 风格材质做第一期近似复刻。

当前已具备：

- GLFW + Vulkan 窗口、swapchain、depth、render pass、pipeline、command buffer、frame sync。
- Dear ImGui 控制面板，可导入模型、扫描 Apex 贴图目录、调节材质参数。
- `.obj` 静态模型加载。
- `.cast` 模型加载，包括 mesh、material slot、skeleton、skin weights、animation clips、animation curves、notification tracks。
- Apex 材质贴图自动扫描、按 slot 聚类、自动绑定到模型材质槽。
- Apex PBR 近似 shader，支持 BaseColor、Normal、Gloss/Roughness、Specular、AO、Cavity、Opacity、ScatterThickness、Anisotropy 近似、Emissive。
- 每材质槽 alpha 模式：Opaque、Masked、Translucent、Additive。
- Translucent / Additive 使用 weighted blended OIT 近似路径，避免仅靠简单排序导致的明显 alpha 错误。
- 材质参数和 slot override 保存到 `<model>.apexmat.json`。
- 命令行 probe：`cast_import_probe`、`apex_material_probe`。

仍未完成：

- `.cast` 骨骼动画数据已导入，但还没有做 GPU/CPU skinning 播放渲染。
- mipmap 没有真正开启：贴图 image 仍是 `mipLevels = 1`，sampler `maxLod = 0.0f`。
- 透明度是 weighted blended OIT 近似，不是 Unreal/Substrate 精确半透明、折射或 per-pixel linked-list OIT。
- Shader 是 Apex/UE Substrate 的视觉近似，不解析 `.uasset`，也不依赖 Unreal Engine。
- 材质 descriptor 当前最多支持 16 个 Apex material slots。

## 目录与关键文件

- `src/Application.*`
  - 应用主循环、窗口事件、Dear ImGui 面板、模型导入按钮、Apex Folder 扫描、参数编辑、sidecar 保存。
- `src/VulkanRenderer.*`
  - Vulkan 初始化、swapchain、render pass、pipeline、descriptor、纹理上传、材质 uniform、draw range 渲染。
  - 透明度路径也在这里：opaque subpass、transparent subpass、composite subpass、ImGui subpass。
- `src/ApexMaterial.*`
  - Apex 贴图命名解析、目录扫描、slot 聚类、slot override、alpha mode/source/channel、JSON sidecar 读写、日志格式化。
- `src/CastImporter.*`
  - `.cast` 二进制读取、节点/属性解析、mesh/skeleton/animation 导入。
- `src/Model.*`
  - `.obj` 加载、模型归一化、缺失 normal 生成。
- `shaders/mesh.vert`
  - 主 mesh vertex shader。
- `shaders/mesh.frag`
  - Opaque/Masked 材质 fragment shader。
- `shaders/mesh_transparent.frag`
  - Translucent/Additive fragment shader，输出 OIT accum/reveal/additive attachments。
- `shaders/composite.vert`
  - 全屏三角形 composite vertex shader。
- `shaders/composite.frag`
  - 合成 opaque、transparent accum/reveal 和 additive。
- `tools/cast_import_probe.cpp`
  - 无窗口验证 `.cast` 导入。
- `tools/apex_material_probe.cpp`
  - 无窗口验证 Apex 材质扫描和贴图映射。
- `README.md`
  - 面向使用者的构建、运行、材质命名和 UI 说明。

## 构建环境

需要：

- Windows 10/11
- CMake 3.24+
- Visual Studio C++ Build Tools / MSVC
- Vulkan SDK，要求 `VULKAN_SDK` 已设置，且能找到 `glslc.exe`
- Ninja 推荐

如果普通 PowerShell 找不到 MSVC 或 CMake，可使用 Visual Studio Developer Prompt，或手动初始化：

```powershell
& cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && powershell'
```

常用构建命令：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

本机验证过的构建命令：

```powershell
& cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --target vulkan_model_viewer apex_material_probe --config Debug'
```

## 运行与验证

启动空场景 / fallback cube：

```powershell
.\build\vulkan_model_viewer.exe
```

加载模型：

```powershell
.\build\vulkan_model_viewer.exe path\to\model.obj
.\build\vulkan_model_viewer.exe path\to\model.cast
```

Smoke test：

```powershell
.\build\vulkan_model_viewer.exe --smoke-test
.\build\vulkan_model_viewer.exe --smoke-test path\to\model.cast
```

CAST 导入验证：

```powershell
.\build\cast_import_probe.exe path\to\model.cast
```

Apex 材质扫描验证：

```powershell
.\build\apex_material_probe.exe path\to\model.cast
.\build\apex_material_probe.exe path\to\Textures
```

最近一次验证结果：

- `vulkan_model_viewer` / `apex_material_probe` 构建通过。
- 默认 `--smoke-test` 连续 5 次通过。
- Horizon CAST 示例 `--smoke-test` 通过，Vulkan pipeline 创建并渲染一帧成功。
- `apex_material_probe` 能识别 Opaque、Masked、Translucent、Additive slot，并列出对应贴图绑定。

## Apex 材质导入规则

扫描入口：

- 加载模型时自动扫描模型附近目录。
- UI 中 `Apex Folder` 可手动选择贴图目录。
- 支持附近的 `Textures`、`textures`、`images`、`_images`、`material` 等目录。

命名规则：

- 忽略文件名前缀 `T_`。
- 最后一个 `_` 后的 token 作为贴图类型。
- 前面的部分作为 material slot name。
- 同一 material slot 的贴图组合成一个 `ApexMaterialSlot`。
- 如果模型材质槽名匹配扫描出的 slot，则自动绑定。
- 不匹配时会写日志，并可在 UI `Bindings` tab 手动 override。

支持的贴图后缀：

- `*_col` / `*albedoTexture`：Albedo/BaseColor，sRGB。
- `*_nml` / `*normalTexture`：Normal，linear，可翻转 green channel。
- `*_gls` / `*glossTexture`：Gloss，linear，shader 中转 roughness。
- `*_spc` / `*specTexture`：Specular/F0，linear。
- `*_ao` / `*aoTexture`：AO，linear。
- `*_cvt` / `*_cav` / `*cavityTexture`：Cavity，linear。
- `*_opa` / `*_msk` / `*opacityMultiplyTexture`：Opacity/Alpha Mask，linear。
- `*_thk` / `*_sctr` / `*scatterThicknessTexture`：Scatter Thickness，linear。
- `*_asd` / `*anisoSpecDirTexture`：Anisotropy，linear，优先 nearest。
- `*_ilm` / `*_ehl` / `*emissiveTexture`：Emissive，sRGB。
- `*_ehm` / `*emissiveMultiplyTexture`：Emissive Multiply，linear。

缺图默认值：

- Albedo：white
- Normal：flat normal
- Gloss：0.5
- Specular/F0：0.04
- AO/Cavity/Opacity：1.0
- Emissive/Thickness：0
- Anisotropy：neutral

## UI 与配置

Dear ImGui 面板名为 `Vulkan Model Viewer`。

顶部操作：

- `Import Model`：导入 `.cast` / `.obj`。
- `Apex Folder`：选择贴图目录。
- `Rescan`：重新扫描当前模型/目录。

`Material` tab：

- Enable Apex Material Mode
- Flip Normal Green
- Roughness Multiplier
- Specular Multiplier
- AO Strength
- Cavity Strength
- Emissive Strength
- Alpha Cutoff
- Enable Subsurface Approximation
- Subsurface Color / Strength / Thickness Scale
- Enable Anisotropy
- Anisotropy Strength
- Substrate Max Closures
- Load Saved Params On Startup

`Bindings` tab：

- 手动选择模型材质槽和扫描贴图槽。
- 设置每 slot 的 Alpha Mode、Opacity Source、Opacity Channel。
- 显示每个 slot 当前绑定到哪些贴图。

`Log` tab：

- 显示扫描结果、缺图默认、重复贴图、未匹配 slot 等日志。

配置保存：

- 参数保存到模型旁边的 `<model>.apexmat.json`。
- 默认启动使用程序内置的当前材质参数，不再复用旧 sidecar 里的滑块值；需要复用旧值时，在 UI 勾选 `Load Saved Params On Startup`，或把 JSON 的 `startupParameters` 改成 `Saved`。
- `substrateMaxClosureCount` 默认 1，可设为 1-4；1 使用单 slab 参数混合路径，2+ 启用当前简化 vertical-layer 近似路径。
- 支持 `slotOverrides`、`slotAlphaModes`、`slotOpacitySources`、`slotOpacityChannels`。

## Vulkan 渲染管线摘要

当前 render pass 分为四个 subpass：

1. Opaque subpass
   - 输出到 offscreen opaque color。
   - 使用 depth test/write。
   - 绘制 Opaque 和 Masked。
2. Transparent subpass
   - 使用 depth test，但关闭 depth write。
   - 输出 accum、reveal、additive attachments。
   - 绘制 Translucent 和 Additive。
3. Composite subpass
   - 通过 input attachments 读取 opaque、accum、reveal、additive。
   - 输出最终 swapchain color。
4. ImGui subpass
   - 在最终 swapchain color 上绘制 UI。

Alpha 行为：

- Opaque：忽略 opacity。
- Masked：按 alpha cutoff discard。
- Translucent：weighted blended OIT 近似。
- Additive：单独累加，适合 glow/energy 类效果。

## mipmap 当前状态

mipmap 目前没有真正开启。

当前代码中 image 创建固定：

```cpp
imageInfo.mipLevels = 1;
```

sampler 也固定：

```cpp
samplerInfo.maxLod = 0.0f;
```

因此所有贴图实际只采样 mip level 0。远距离高频贴图可能有闪烁、噪声、锯齿，尤其是 hair、mask、emissive、normal、gloss 贴图。

建议后续实现：

1. `TextureResource` 增加 `mipLevels`。
2. 按 `floor(log2(max(width, height))) + 1` 计算 mip 数。
3. `createImage` 支持传入 mip levels。
4. 贴图 image usage 增加 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`。
5. 上传 level 0 后用 `vkCmdBlitImage` 逐级生成 mip。
6. sampler `maxLod = mipLevels - 1`。
7. 确认格式支持 `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`，否则 fallback 到无 mip 或 nearest blit。

## 下一步优先级

建议按以下顺序继续：

1. 实现 mipmap 生成和 sampler LOD。
   - 这是当前画质稳定性收益最高的改动。
2. 做基础截图验证流程。
   - 固定模型、固定相机、输出 PNG，便于比较 alpha/mipmap/material 改动。
3. 做骨骼 skinning 渲染。
   - 当前 `.cast` 动画已导入，但没有实际驱动 mesh。
4. 扩展材质 slot descriptor 上限或改为更弹性的 bindless/descriptor indexing。
   - 当前 16 slot 对复杂资源可能不够。
5. 继续校准 Apex/Substrate 近似。
   - 重点对比 UE 中 BaseColor、Normal green、Gloss/Roughness、Specular、AO/Cavity、Emissive、Alpha。
6. 改进透明材质。
   - 当前 OIT 是工程上可用的近似；若要更接近 UE，需要进一步处理折射、排序、特殊 blend 语义。

## 资产与版权边界

- 不要解析、运行或移植 Unreal `.uasset`。
- 不要引入 Unreal Engine 依赖。
- 不要提交 Apex/EA 专有资产。
- `exports/` 已在 `.gitignore` 中排除。
- 验证可使用本地导出的资源、项目内测试资源或生成的中性占位贴图。
