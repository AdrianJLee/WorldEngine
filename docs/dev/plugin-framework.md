# 插件框架(清单 / 创建入口 / 模板 / 产物布局)

> 状态:2026-09-30(PLUG-AUTH-1 补"创建入口 + 分类模板")。
> ABI 与加载器的唯一事实源是 `Engine/src/World/Plugins/WePluginApi.h` 与
> `Engine/src/World/Plugins/PluginManifest.h`;本页讲**怎么创建、怎么落盘、产物在哪**。
> 版本同步点:升级插件 ABI 时要一起改 `WePluginApi.h`、`plugin.we.yaml` 的 `abi`、
> `PluginManager.*` 与这一页。

## 1. 插件包长什么样

```
<插件根>/<目录名>/            # 目录名 = 插件短名(也是产物 DLL 的文件名)
  plugin.we.yaml              # 必需:清单(发现期的唯一事实源)
  src/plugin.cpp              # 源码(第三方插件模板只有这一个文件)
  CMakeLists.txt              # 插件自己的构建(由插件根收集进构建)
  README.md                   # 面向"拿引擎做游戏的人"的说明
  bin/<目录名>.dll            # 可选:发布布局的预编译产物(开发构建不写源码树)
```

两个**插件根**:

| 根 | 位置 | 清单里的 scope | 构建者 |
| --- | --- | --- | --- |
| 引擎插件 | `<引擎根>/plugins/<name>/` | `engine` | 引擎构建(根 `CMakeLists.txt` 的 `plugins/*/CMakeLists.txt` 收集) |
| 项目插件 | `<项目根>/plugins/<name>/` | `project` | 项目构建(`templates/project-*/CMakeLists.txt` 的收集) |

**位置即 scope**:清单里不要写 `scope:`;写了必须与所在根一致,否则发现期拒绝。
两个根都由 `PluginManager::Discover(engineRoot, projectRoot, devBinaryRoots)` 扫描;
根不存在 = 0 个插件(不是错误)。

## 2. 清单字段(`plugin.we.yaml`)

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `id` | 是 | 反域名形态,插件间唯一(如 `com.studio.my-plugin`) |
| `name` | 否 | 显示名;缺省 = id |
| `version` | 否 | semver(诊断/依赖用);缺省 `1.0.0` |
| `abi` | 否 | 缺省 = 当前 `WE_PLUGIN_ABI_VERSION`;写了必须等值(ABI 演进 = 干净拒绝) |
| `entry` | 否 | 导出入口符号;缺省 `WePluginQuery` |
| `engine` | 否 | 目前只接受 `">=X.Y"`(最低引擎版本)或空 |
| `provides` | 否 | 能力声明列表,与 `WePlugin::Provides` 比对(不一致 = 警告) |
| `depends` | 否 | 依赖的插件 id(加载顺序 + 缺依赖拒绝) |
| `ship` | 否 | `auto`(缺省)/ `always` / `never` |

清单**只解析、不重写**:向导生成时保留模板里的注释,做的是字面占位符替换。

## 3. 创建入口(三条,同一个脚手架)

1. **File ▸ New Plugin…**(`menu.file.new_plugin`)—— 任意形态可用;
2. **插件管理器面板**(「窗口 ▸ 插件」)的「**新建插件…**」按钮(`plugins.action.new`)——
   面板只在项目形态存在;
3. **启动器页**的「新建插件…」(`project.launcher.new_plugin`)—— 没有项目时目标固定**引擎插件**
   (Project 分段禁用并给理由)。

向导字段与稳定 a11y id(自动化按 id 驱动,不依赖标签语言):
`plugin.new.scope.engine` / `plugin.new.scope.project` / `plugin.new.template.<模板id>` /
`plugin.new.name` / `plugin.new.id` / `plugin.new.location` / `plugin.new.error` /
`plugin.new.create` / `plugin.new.cancel`;成功后:`plugin.new.done`(value 含落盘目录)/
`plugin.new.locate`(在内容浏览器中定位,仅项目插件可用)/ `plugin.new.open`(打开插件目录)。

向导的校验(名称/ID/落点/模板)全部来自 `Editor::PluginScaffolder` 的**只读校验函数**,
Create 按钮在这些校验不过时禁用并给可读原因。

## 4. 模板库(`templates/plugin-*/`)

每个模板 = `template.json` + `plugin.we.yaml` + `README.md` + `src/plugin.cpp` + `CMakeLists.txt`。
4 个占位符是**字面替换**:`{{PluginId}}`、`{{PluginName}}`、`{{PluginDir}}`(目录名 = 插件短名)、
`{{PluginScope}}`(`engine` / `project`)。
`template.json` 只是模板元数据(列模板用),**不会**被复制进生成的插件包 —— 插件包里只有
清单 / README / 源码 / CMake 这四件。

| 模板 id | 名称 | `provides:` | `requires` | 说明 |
| --- | --- | --- | --- | --- |
| `empty` | Empty Plugin | (空) | none | 最小可加载插件 |
| `library` | C++ Library Plugin | `cxx.exports` | none | 导出函数表(`<短名>.ping` 示例) |
| `asset-type` | Asset Type Plugin | `asset.type`, `asset.importer` | none | 新资产类型 + 导入器(蓝本:`plugins/hello-import`) |
| `editor-ui` | Editor Extension Plugin | `editor.panel`, `editor.command` | **t3b** | 面板/命令;注册面未就绪,生成可编译骨架 + TODO |
| `component` | Component Plugin | `scene.component` | **t2b** | 组件 schema;同上 |
| `lua-lib` | Lua Library Plugin | `script.library` | **t4** | 脚本库;同上 |

`requires != none` 的模板**现在就能编译**:源码里只留 TODO(说明"该注册面未就绪,
落地后把 TODO 换成真实注册"),不引用尚不存在的 API;向导在描述里显式提示需要哪个注册面。

## 5. 脚手架(`Editor/src/Project/PluginScaffolder.{h,cpp}`)

```cpp
ListTemplates()                                   // templates/plugin-*/template.json 的投影(坏模板也列出,带 Error)
ValidatePluginName(name)                          // 目录名/CMake 目标名口径:字母、数字、'-'、'_',≤64
ValidatePluginId(id)                              // 反域名:小写字母/数字/'-'/'.',至少两段
ValidateTarget(pluginsRoot, name, pluginId)       // 目录已存在 / 已有插件用了同一个 id = 拒绝
Scaffold(pluginsRoot, request)                    // 校验 → 临时目录 → 清单自校验 → 整体改名
```

落盘纪律(与 `ProjectScaffolder` 同款):

1. 先写 `<插件根>/<名>.tmp-<随机>`(同卷),逐文件复制模板 + 字面替换占位符;
2. 生成的 `plugin.we.yaml` **立刻用 `PluginManifest::Load` 自校验** —— 失败就删临时目录并返回
   可读原因(保证落盘的包一定被发现期接受);
3. 全部成功后再整体改名为 `<插件根>/<名>`;目标已存在 = 失败,**绝不覆盖**;
4. 任何失败都不留半成品(临时目录必删)。

## 6. 构建与产物布局

插件**不链接 World** —— 只 `target_include_directories(... "${WLD_ENGINE_ROOT}/Engine/src")`
拿公共 ABI 头,与第三方插件的边界一致。模板的 `CMakeLists.txt` 与
`plugins/hello-import/CMakeLists.txt` 同款:

```cmake
add_library(WePlugin_<目录名> SHARED src/plugin.cpp)
RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
```

- **引擎插件**:重跑引擎 CMake configure(收集新目录)后构建 `ALL_BUILD`,
  产物 `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/<名>.dll`;
- **项目插件**:项目自己的构建(项目根 `build.cmd` / CMake)产出
  `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/<名>.dll`。

发现期按"插件包自带 `bin/<名>.dll` → 开发产物根"的顺序找产物。编辑器的
`EditorLayer::InitPlugins` 会把**引擎**与**项目**两个开发产物根都传给 `Discover`;
缺后者时项目插件会"发现得到、加载不了"。

## 7. 验证

- 探针:`tools/agents/scratch/PLUG-AUTH/verify-plugin-authoring.py`(向导三条入口、负例、
  6 个模板逐个生成 + `PluginManifest::Load` 自校验、编译一个生成的项目插件并断言 DLL 产出);
- 门禁:`ALL_BUILD` + `RUN_TESTS` + `tools/agents/check-layout.ps1` +
  `python tools/agents/skills/worldengine-dev/scripts/audit-localization.py`;
- 报告口径:静态检查 / 编译 / 实际运行分开写(编译成功不等于界面/交互已验证)。

## 8. 组件注册(T2b,2026-09-30)

`WeHostApi` **尾部追加**了组件 schema 注册面(append-only:字段只增不改号,
`WE_PLUGIN_ABI_VERSION` 仍 = 1;旧宿主缺这两个字段时,插件用
`host.StructSize >= offsetof(WeHostApi, UnregisterComponent) + sizeof(host.UnregisterComponent)`
自检并干净拒绝):

```cpp
bool (*RegisterComponent)(void* userData, const WeComponentDesc* desc);
bool (*UnregisterComponent)(void* userData, const char* id);
```

`WeComponentDesc{StructSize, AbiVersion, Id, DisplayName, ComponentId, Fields, FieldCount}`;
`WeComponentFieldDesc{StructSize, AbiVersion, Name, Kind, Offset, Size, Flags, Choices,
ChoicesCount, Doc}`(定义与字段注释见 `Engine/src/World/Plugins/WePluginApi.h`)。

- `Kind` 的数值 = `World::Schema::Kind` 的稳定值(口径见
  `Engine/src/World/Schema/Schema.h`;`PluginManager.cpp` 有逐项 `static_assert` 钉住)。
- `Offset` / `Size` 用 `offsetof` / `sizeof` 填;`Size` 必须等于 Kind 的规范大小,否则注册被拒
  (宿主不会按插件声明的大小做越界 memcpy)。组件结构的总大小由插件拥有,宿主不校验。
- 支持固定大小 POD(布尔 / 整型 / 浮点 / 向量 / 四元数 / 矩阵);String / Enum / Asset /
  Object 与容器字段 = 干净拒绝(返回 false + 可读日志,不半注册)。
- `ComponentId` 本阶段**必须为 0**:组件存储桥(entt Add/Copy)不在本版 ABI 里,非 0 会被拒绝
  (避免"接受但静默忽略"造成组件能注册却不能实例化的误解)。
- 宿主映射:`Schema::SchemaRegistry::RegisterModule(<插件 id>, {TypeSchema})`
  (`Category = Component`,`Storage == nullptr`);注销 = `UnregisterModule(<插件 id>)` ——
  注销单个类型时,宿主把该插件其余类型按原顺序重新注册回同一模块。
- 兜底回收:插件自己没注销的类型,在卸载 / `Register` 返回 false 或抛异常的回滚 /
  管理器析构时被整模块移除并记 WARN(不留指向已卸载插件的类型)。
- 幂等/归属:重复 `Id` = false + 警告(不覆盖);注销"本插件没注册过"的 `Id` = 别的插件拥有
  ⇒ false + 警告,否则 ⇒ true(幂等)。

> **§4 模板表的更正(T2b 落地)**:`component` 行原写 `requires: t2b`、"注册面未就绪" ——
> 该模板的 `template.json` 已改为 `requires: none`,源码从"骨架 + TODO"换成
> `templates/plugin-component/src/plugin.cpp` 里的真实注册;以本节为准。

用法示例即 `templates/plugin-component/src/plugin.cpp`(生成物可直接编译);单测
`World.Plugins` 覆盖:注册可见性(`Find` / `ListByModule`)、字段元数据映射、
`.wd` 同一条序列化 API 的往返(含按 `offsetof` 的逐字节断言)、注销幂等、卸载与析构兜底回收。
