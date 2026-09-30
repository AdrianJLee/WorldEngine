# 热重载生效矩阵

> 状态：2026-09-30（HOTR-P1/P2/P3 批次）整理。口径以本文件为准；
> 各实现处的注释只保留局部细节。改动热重载行为时**同批更新本文件**。

## 总则

- 热重载**只存在于编辑器进程（`Editor.exe`）**。发行形态（`Runtime.exe` + 打包内容）没有监听器、
  也没有着色器编译器：一切以 cook 产物为准（设计如此，不是缺陷）。
- 分层口径：**L1 资产**（贴图/材质/场景/预制体/本地化/引擎着色器）→ **L2 脚本**（Luau）→
  **L3 原生模块**（`Game.dll` / 插件 DLL）。L1/L2 自动；L3 需要外部构建，编辑器提供两段式重载入口。
- 所有"改盘生效"都遵守两条硬规则：**同内容重写（只动 mtime）不算变化**（内容哈希优先）；
  **绝不用磁盘内容覆盖未保存的编辑**（面板 dirty 时只报告/只提示）。

## 生效矩阵

| 资产/系统 | 监听与触发 | 生效时机 | 失败/回滚 | 已知限制 |
| --- | --- | --- | --- | --- |
| Luau 脚本（`assets/scripts/**.luau`） | `ScriptFileWatch`：150ms 消抖轮询，内容哈希优先 | 帧边界；编辑态作用于文档场景，Play/Simulate 作用于运行副本；不满足安全点自动顺延 | 编译/校验失败保留旧实例，原因写 `ReloadDiagnostic`；字段按稳定 id 迁移 | 回调内/结构提交点内不重载（顺延）；包内脚本只读 |
| 编辑器本地化（`assets/localization/<lang>/**`） | `EditorApp` 的 `LocalizationHotReloadLayer`：0.5s 节流（路径/大小/mtime） | `ReloadLocalization()` 重扫多层目录，`Generation++`，下一帧换文案 | 缺键回落内联英文；层内重复键记冲突 | 插件组件的显示名不随语言切换刷新（要 `plugin.reload` 或重启） |
| 材质 `.wmat` | `MaterialLibrary` + `AssetFileWatch`：150ms；指纹含父级链 | clean 材质原地 `Reload`（实例同一性保持，Revision 前进） | dirty → 只报告；解析失败保留旧内存态 | 父级链深度上限 8 |
| 贴图（材质引用的 Albedo/Normal） | `AssetFileWatch`：500ms；内容哈希 | `MaterialTextureCache::Invalidate` + 引用材质 `InvalidateTextures()`；Vulkan 旧句柄延迟释放 | 坏产物/坏图回退源图；材质贴图上传走**同步路径**（见下） | 只跟踪已加载材质引用的贴图 |
| 贴图导入 `.wtex` / 源图 | 编辑器 `TextureImportWatch`：2s 重扫 + 2s 稳定窗口；工作线程烘焙 | 主线程提交写 `<同目录>/<主名>.wtexc` + 缓存失效 + **引用材质失效**；日志 `texture rebaked` | 烘失败只记日志、保留旧产物；不写 `.wtex`、不碰面板内存 | `WLD_TEXTURE_HOTRELOAD=0` 关闭 |
| 表面材质 `.slang`（材质引用） | `MaterialLibrary`：150ms；编辑器级 `ShaderHotReload`（面板没开也生效） | 工作线程重编译 → 帧边界 `MaterialSurfaceRuntime::Install(<路径键>)`；`shaders/lib/**` 依赖变化映射回根路径 | 编译失败保留旧管线并记首条诊断 | 键分离：面板未保存编辑只走 `<路径>#preview` |
| 引擎内建 shader（`Engine/assets/shaders/**.slang`） | `EngineShaderHotReload`：150ms，绝对路径轮询 | 帧边界 `Renderer::ReloadShaders()`（2D→3D→WUI；只重建 shader+管线，旧句柄延迟释放） | 任一 owner 失败保留其旧管线，`failed` 计数 + 首条可读原因 | `WLD_SHADER_HOTRELOAD=0` 关闭；打包形态读 cooked `shaders/*.spv` |
| 场景 `.wd`（当前文档） | `AssetFileWatch`：150ms | **干净文档 + Edit 态** → 自动重开（按 UUID 保选择、恢复编辑器相机）；否则只提示 | dirty/Play/Simulate 不自动；重开失败保留原文档 | `WLD_SCENE_AUTORELOAD=0` 关闭；未保存修改永不自动覆盖 |
| 预制体 `.wprefab`（场景实例引用） | 每帧同步实例来源路径进 `AssetFileWatch`：150ms | 安全点逐实例 `Gameplay::ApplyPrefabChanges`（保留 overrides） | 结构不一致/读失败 → 不动任何实体 + 可读日志 | 不改变实例身份；同级重排不算结构变化 |
| C++ 模块 `Game.dll`（含 C++ 脚本组件） | 手动：`File ▸ Build & Reload C++ Module` / AI `module.build_reload`（后台构建）；分步 `module.unload`+`module.reload` | 卸载释放 DLL 锁 → 构建 → 加载新 DLL；实例配置属性按稳定字段迁移 | 构建失败保持 unloaded + 输出尾部可见；加载失败自动回滚 `.rollback-<abi>.dll` | 不迁移 C++ 成员可变状态/指针注册；硬崩溃仍是进程终止 |
| 插件 DLL（L3） | 手动：插件管理器「重新加载」/ AI `plugin.reload` | 两段式；blob 组件按实体 UUID + 字段 id 快照写回 | 第二段失败载入 `.rollback-<abi>.dll` 并写回原状态 | 有活实例/Play 下第一段干净拒绝；快照只在内存、不跨重启 |
| glTF/GLB 源（`.gltf/.glb` → `.wmodel`） | 本轮**不自动** | 手动 Reimport；重导会清动画缓存 | 失败保留旧 `.wmodel` | 自动重导入会重写 `.wmodel`/材质/贴图，列为后续单独确认项 |

## 需要重启（或明确的人工动作）的场景

- 渲染后端切换（`project.we.yaml` 的 `renderer:`）：编辑器自动重启进程；
- AI 控制通道端口、插件启用清单（`local/plugins.json`）：重启生效；
- 新增 C++ 脚本 API 后的 `WorldEngineAPI.luau` 存根：重新构建并重启编辑器；
- 插件组件的本地化显示名：`plugin.reload` 或重启；
- 发行 Runtime：无热重载。

## 开关与诊断

| 环境变量 | 作用 |
| --- | --- |
| `WLD_ASSET_HOTRELOAD=0` | 关闭材质/贴图/场景的资产监听（也关闭 `.wtex` 自动重烘的调度） |
| `WLD_TEXTURE_HOTRELOAD=0` | 只关 `.wtex` 自动重烘（默认开） |
| `WLD_SHADER_HOTRELOAD=0` | 关闭引擎内建 shader 监听 |
| `WLD_SCENE_AUTORELOAD=0` | 关闭场景自动重开（保留"只提示"） |
| `WLD_ASSET_HOTRELOAD_TRACE=1` | 资产监听日志（watching/changed/invalidated 等） |
| `WLD_UPLOAD_RING_TRACE=1` | Vulkan 上传环句柄级日志（段/命令缓冲/栅栏/提交） |
| `WLD_VK_PRESENT_TRACE=1` | 交换链呈现路径日志 |
| `WLD_TEX_SYNC_UPLOAD=1` | 强制所有纹理同步上传（诊断） |

回归探针（`tools/agents/scratch/`）：
`HOTR-P1/t1-shader-hot-reload-probe.py`（材质 `.slang`）、`HOTR-P1/t3-engine-shader-probe.py`（引擎 shader）、
`HOTR-P2/t5-scene-autoreload-probe.py`（场景）、`HOTR-P2/t5-texture-rebake-probe.py`（纹理重烘）、
`HOTR-P2/t6-prefab-follow-probe.py`（预制体）、`HOTR-P3/t7-build-reload-probe.py`（构建并重载）。

## 已知遗留

- Vulkan **异步上传环**在"同一帧多次提交"下仍有隐患（材质贴图已改走同步上传绕开；
  根因与复现见 `tools/agents/tasks/20260930-2130-hotreload-p1p2p3/plan.md` 的 P2-c 取证记录）；
- 统一进程级 watch 服务（把 150ms/500ms/2s 的轮询与日志收敛成一处）尚未实施；
- glTF 源自动重导入未做（当前只提示）。
