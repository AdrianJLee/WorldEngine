# 引擎自有文件格式登记表

> 规则:新增引擎自有格式**先登记再使用**;扩展名带 `w`/`we` 前缀;路径写法见
> [`naming.md`](naming.md) §5。

| 扩展名 | 名称 | 谁读/写 | 版本字段 | 备注 |
| --- | --- | --- | --- | --- |
| `.we.yaml` | 项目清单 / 场景级清单 | `Engine`(ProjectManifest 等) | 有(清单内版本) | 默认 `projects/<名>/project.we.yaml` |
| `.wmat` | 材质**实例** | `Engine`(MaterialLibrary) | `DocumentFormatVersion` | 只存覆盖值 + 贴图 + `Shader:` 引用;父级继承 |
| `.wmodel` | 模型资产 | `Engine`(ModelIO/导入器) | 有 | 由 glTF 导入器生成(入库策略见 `.gitignore`) |
| `.welevel` | 关卡 | `Engine`(Level) | 有 | 例:`projects/default/levels.welevel` |
| `.slang` | **材质着色器(材质本身)/ 引擎着色器源**(Slang 源) | 用户 + `Engine`(MaterialSurfaceCompiler / ShaderCompiler) | 契约版本在缓存键里 | `//! param` 注解 = 参数事实源;HLSL 语法是 Slang 的子集 |
| `.hlsl`(legacy) | 旧口径的着色器源,只读兼容 | `Engine`(导入 / 编译 / 烘焙都认) | 同上 | 带可读迁移提示;新写路径(向导 / 迁移脚本)产出 `.slang` |
| `.hlsli` | 引擎着色器**共享头片段**(Slang / C++ 共用,非独立翻译单元) | `Engine` | 同契约版本 | 如 `MaterialSurfaceContract.hlsli` |
| `.luau` | 脚本 / 生成存根 | `Engine`(Luau VM) + 编辑器 | — | 存根:工程 `assets/scripts/intermediate/**`,有漂移门禁 |
| `.wui.json`(历史) | 编辑器窗口布局/浏览器/偏好 | 编辑器 | — | **已迁移**为 `local/*.json`(不入库) |
| `.wd` / `.wscene`(待确认) | 场景文件 | `Engine`(Scene) | — | ⚠ 待补:确认实际扩展名与版本字段后填表 |

## 待补清单

- 场景文件扩展名与版本策略;
- 关卡 `.welevel` 与场景的关系;
- 资产 `.meta`(若存在)的字段与生成规则;
- 打包清单(`EditorCooker` 产物)格式与版本。
