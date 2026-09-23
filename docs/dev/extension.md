# 扩展点

先记住两条总原则:

- 扩展走**公开接口**与注册表,不改引擎内部实现细节。
- 注册是**显式**的:谁提供、什么时候注册、注册到哪张表,都写在代码里,不靠隐式初始化。

## 1. 加组件 / 加字段

组件是普通 C++ 结构,编辑器界面按 schema 生成,所以"怎么编辑"声明在**字段旁**。

| 步骤 | 位置 |
| --- | --- |
| 声明组件与字段 | `Engine/src/World/Scene/Components.h`(`WE_SCHEMA_META(...)`、`WE_FIELD(...)`) |
| 生成反射代码 | `Engine/generators/schema-compiler`(`--input <头文件> --module World --output Engine/src/World/Schema/Generated --manifest ... --reg-include ...`) |
| 门禁 | 测试 `World.SchemaDrift`:生成结果与注解不一致就红灯(字节级比较) |

字段上可用的编辑元数据包括:显示名与分组、文档字符串(`Doc("…")`,落到属性面板的悬停提示与无障碍节点)、
取值枚举(`Choices(...)`)、资产类型(`Asset("Material")` 或 `Of(UUID)`)、颜色(`Color()`)、只读核心字段(`Core()`)。
这些元数据只影响**编辑体验**,不参与序列化,也不是 ABI。

## 2. 加资产类型

| 步骤 | 位置 |
| --- | --- |
| 注册类型名、扩展名、创建方式 | `Engine/src/World/Core/Asset/AssetTypeRegistry.h/.cpp` |
| 内容浏览器显示与筛选 | `Editor/src/WUI/Panels/ContentBrowserPanel.*` |
| 参考测试 | `tests/World/AssetTypeRegistryTests.cpp`(注册 / 覆盖 / 反注册 / 排序 / 落盘) |

导入型资产(如模型)另走导入器:源文件 → 引擎资产,导入设置存放在资产自身;`.gltf` / `.glb` 只是导入源,
可引用的产物是 `.wmodel`。

## 3. 加编辑器面板

| 步骤 | 位置 |
| --- | --- |
| 面板实现 | `Editor/src/WUI/Panels/`(照抄现有面板的结构:绘制、输入、持久化、无障碍节点) |
| 在编辑器外壳里注册/挂载 | `Editor/src/WUI/EditorShell.*`(停靠面板 / 独立窗口 / Window 菜单) |

约定:

- 每个可交互控件都要有稳定 id 并在无障碍树里登记,自动化与键盘操作都依赖它。
- 面板自己拥有并释放自己的 GPU 资源;有独立窗口的面板要在其窗口销毁前释放。
- 面板状态(尺寸、列宽、最近使用等)走独立的持久化入口,不要写进场景或资产。

## 4. 从 Game 侧接入引擎(跨 DLL)

`Game` 是独立 DLL,不能依赖"同一个单例在两个模块里都存在"。做法是:

1. 引擎提供显式的注册入口(注册表 / 工厂 / 回调),`Game` 在初始化时把自己的实现注册进去。
2. 跨边界只传公共头文件里的类型与导出符号;引擎侧的拥有者始终是引擎模块。
3. 边界变更后宿主与 `Game` 一起重建,避免导出接口与实现不匹配。

## 5. 加脚本绑定

脚本侧(Luau)通过绑定暴露引擎能力:绑定代码在 `Engine/src/World/Script/`(`Bind*.cpp`),
补全声明由编辑器按实际注册结果生成到 `projects/default/assets/scripts/intermediate/WorldEngineAPI.luau`。
新增绑定后重新构建并启动编辑器刷新声明;不要手改生成文件。

## 验证

| 改动 | 最低验证 |
| --- | --- |
| 组件 / 字段注解 | 重跑生成器 + `World.SchemaDrift` |
| 资产类型 | `World.AssetTypeRegistry` 套件 |
| 编辑器面板 | 构建 `Editor` 并至少实际打开一次面板(自动化可走无障碍节点) |
| 跨 DLL 接口 | 重建 `World` + `Game` + 宿主,并跑相关套件 |

完整的构建、测试命令见[构建与运行](build.md)。
