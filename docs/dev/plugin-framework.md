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
| `contributes` | 否 | **打包期引用索引**(T5):`components` / `asset_types` / `importers` / `script_namespaces`;见 §12 |

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

## 9. 编辑器扩展(命令与面板,T3b,2026-09-30)

`WeHostApi` 继续**尾部追加**编辑器扩展注册面(append-only:`WE_PLUGIN_ABI_VERSION` 仍 = 1;
旧宿主缺这些字段时,插件用
`host.StructSize >= offsetof(WeHostApi, UnregisterEditorPanel) + sizeof(host.UnregisterEditorPanel)`
自检并干净拒绝):

```cpp
bool (*RegisterEditorCommand)(void* userData, const WeEditorCommandDesc* desc);
bool (*UnregisterEditorCommand)(void* userData, const char* id);
bool (*RegisterEditorPanel)(void* userData, const WeEditorPanelDesc* desc);
bool (*UnregisterEditorPanel)(void* userData, const char* id);
```

```cpp
struct WeEditorCommandDesc { StructSize, AbiVersion, Id, Label, Tooltip, Callback, UserData };
struct WeEditorPanelDesc   { StructSize, AbiVersion, Id, Title, Draw, UserData };
struct WeEditorUiApi       { StructSize, AbiVersion, UserData, PluginId,
                             Label, Button, Separator, Checkbox, InvokeCommand };
```

### 命名契约(宿主统一加命名空间;插件只给面板内唯一的短 id)

| 对象 | 完整名字 | 用途 |
| --- | --- | --- |
| 面板 | `plugin.panel.<插件 id>.<面板 id>` | 面板注册表 id / `ui.open` / a11y 根 id / 挂靠标签 `shell.attach.<面板 id>` |
| 命令 | `plugin.command.<插件 id>.<命令 id>` | AI 通道 `plugin.command.run` 触发 |
| 面板内控件 | `plugin.panel.<插件 id>.<面板 id>.<控件 id>` | 面板 a11y 节点(`Label` 自动编号 `#<n>`) |

### 生命周期与回收

- 面板是**独立窗口形态**(`PanelForm::Independent`,与插件管理器 / Project Settings 同款):
  默认打开 = `TogglePanel → OpenPanelAttached`(附加到主窗口的标签切换),可拖出为独立 OS 窗口;
  布局存档里不保留停靠记录(与其它独立面板同一套白名单规则)。
- 卸载(`Unload` / `UnloadAll`)/ `Register` 返回 false / `Register` 抛异常 / 管理器析构四条路径
  都会把该插件的命令与面板从编辑器注册表移除(未自己注销 = 记 WARN 后强删);
  已打开的面板窗口由 `EditorShell` 在帧末按注册表核对关闭(不留空窗口)。
- 幂等:重复 `Id`(同一插件)= false + 警告(不覆盖);注销"本插件没注册过"的 `Id` = true。
- 生命周期契约:`PluginEditorHost` 宿主对象**必须比 `PluginManager` 活得久**,或宿主析构前先
  `SetEditorHost(nullptr)`(`EditorLayer::ShutdownPlugins` 的收尾顺序就是这条)。
- 启动器形态(没有项目 / 没有编辑器宿主):扩展注册被干净拒绝并记可读诊断,插件加载失败 ——
  与 E2「插件管理器只在打开项目时可用」同一口径。

### 面板绘制(`Draw`)——首期小组件表

宿主每帧调用 `WeEditorPanelDesc::Draw(userData, ui, uiContext)`,按调用顺序**垂直排列**
控件(面板高度不足 = 停止绘制,首期不做滚动):

- `ui->Label(uiContext, text)`(只读文本;宿主登记 a11y `label` 节点);
- `ui->Button(uiContext, id, label)` 返回是否被点击;
- `ui->Separator(uiContext)`;
- `ui->Checkbox(uiContext, id, label, &value)` 返回是否被点击改值(新值写回 `*value`);
- `ui->InvokeCommand(uiContext, "<命令 id>")` 触发宿主命令面里的命令 —— 与 AI 通道
  `plugin.command.run` 走同一条执行路径(执行次数在 `plugin.commands` 账本里可观测)。

`Draw` 是只读渲染:不得在绘制里改宿主状态;不得让异常跨过 ABI 边界(抛异常 = 契约违约,
宿主停止渲染该面板并记 ERROR)。命令回调(`Callback`)同理。

### AI 通道命令

| 命令 | 说明 |
| --- | --- |
| `plugin.commands` | 已注册插件命令 + 执行次数(JSON;面板按钮与 AI 通道同源计数) |
| `plugin.command.run <id>` | 触发一条(`plugin.command.<插件 id>.<命令 id>`,也接受唯一命中的裸 `<id>`) |

### 验证

单测 `World.Plugins` 用 ABI-only 测试插件(`tests/plugins/WePluginTestEditorUi.cpp` 等三个)覆盖:
注册/重复拒绝/坏描述干净拒绝、账本可见、命令触发计数、面板渲染入口、
按需注销幂等,以及卸载 / 抛异常 / 管理器析构三条兜底回收路径(不留悬停回调)。
端到端探针:`tools/agents/scratch/PLUG-T3b/verify-plugin-editor-ui.py`
(模板生成 → 编出 DLL → 重启后 loaded → 面板 `ui.open` + 挂靠标签形态 → 面板按钮触发命令 →
AI 通道触发同一条命令 → 禁用重启后面板与命令全部消失)。

> **§4 模板表的更正(T3b 落地)**:`editor-ui` 行原写 `requires: t3b`、"注册面未就绪" ——
> 该模板的 `template.json` 已改为 `requires: none`,源码换成
> `templates/plugin-editor-ui/src/plugin.cpp` 里的真实注册;以本节为准。

## 10. Lua 函数库(脚本函数,T4,2026-09-30)

`WeHostApi` 继续**尾部追加**脚本函数注册面(append-only:`WE_PLUGIN_ABI_VERSION` 仍 = 1;
旧宿主缺这两个字段时,插件用
`host.StructSize >= offsetof(WeHostApi, UnregisterScriptFunction) + sizeof(host.UnregisterScriptFunction)`
自检并干净拒绝):

```cpp
bool (*RegisterScriptFunction)(void* userData, const WeScriptFunctionDesc* desc);
bool (*UnregisterScriptFunction)(void* userData, const char* name);
```

```cpp
struct WeScriptFunctionDesc { StructSize, AbiVersion, Name, Signature, Doc, Callback, UserData };
struct WeScriptCallApi      { StructSize, AbiVersion, UserData, ArgCount,
                              GetArgNumber, GetArgString, GetArgBool,
                              PushNumber, PushString, PushBool, PushNil };
```

### 名字与绑定

- `Name` = `<命名空间>.<函数名>`(**恰好一个点**;两段都必须是合法 Lua 标识符)。
  宿主把函数绑成全局表 `<命名空间>` 的成员 —— 脚本里写 `hello.ping(...)`
  (**点号调用**,不是 `hello:ping(...)`;插件函数没有隐式 self)。
- 命名空间可以**跨插件共享**(两个插件都能往 `hello` 里加函数);完整名字**全局唯一**,
  重复(本插件内或跨插件)= 注册被拒绝(不覆盖)。命名空间清空后宿主移除该全局表。
- 引擎/沙箱保留的全局名(`Input`/`Level`/`Save`/`ui`/`events`/`timers`、Luau 标准库、
  沙箱禁用名单、已登记的绑定类型)= 注册期干净拒绝;绑定发生时再用 VM 的真实全局表复核。

### 参数、返回值与失败语义

- 参数按位置读:`GetArgNumber/GetArgString/GetArgBool`(越界 / 类型不符 = false,
  不改 `*out`;字符串指针只在本次调用期间有效)。参数只支持 number / string / boolean
  能表达的值,表 / 函数等读不到。
- 一次调用**最多一个返回值**:首次 `Push*` 生效,再次 Push = false(宿主记一条 WARN)。
- 插件回调抛异常 = 契约违约:宿主转成 **Lua error**(脚本可 `pcall` 捕),不让异常穿过
  ABI 或把 VM 带走。

### 存根(`Signature` / `Doc`)—— 确定性是硬约束

- `Signature` 形如 `"(name: string, count: number?): boolean"`:参数类型 ∈
  {number, integer, boolean, string, any};`?`(参数名后或类型后)= 可选;`: 返回类型`
  可省略(返回类型 ∈ {number, integer, boolean, string, nil, any});空 = 无参数、无返回值。
  非法签名 = 注册被干净拒绝。`Doc` 是单行说明(不能以 `@` 开头 / 不含控制字符)。
- 存根里插件块追加在**既有全部块之后**(Lua 类型 → 服务 → UI → 组件 → WorldScript →
  插件块):块按**命名空间升序**,同一命名空间内函数按 **(插件 id, 函数名)升序** ——
  与插件装载顺序、注册顺序无关。被禁用 / 被拒绝的插件不入账本,因此不参与渲染。
- **零插件时渲染输出与入库夹具逐字节一致**:`LuaStubGenerator` 的既有重载与
  "带插件表(空)"重载输出相同,`World.ScriptWorkflow` 的漂移门禁不变。

### 运行时绑定与回收

- 唯一账本在 `Engine/src/World/Script/PluginScriptLibrary.h`(运行时绑定与存根渲染**共用
  同一份描述**)。`ScriptEngine::Init` 把账本里的函数统一绑进 VM;VM 已初始化时,
  `RegisterScriptFunction` **立即绑定**(绑定失败 = 干净拒绝,不入账本)。
- 卸载 / `Register` 返回 false / `Register` 抛异常 / 管理器析构四条路径都会把该插件的函数
  从账本与 VM 全局表里回收(未自己注销 = 记 WARN 后强删)。
- 幂等与归属:注销"本插件没注册过"的名字 = true;名字归**别的插件** = false + 警告
  (`is not owned by this plugin; unregister ignored`)。卸载后重载同一批名字不留脏账。

### 验证

单测 `World.Plugins`(用例 T4①/T4②)用 ABI-only 测试插件
(`tests/plugins/WePluginTestScriptLib.cpp` / `WePluginTestScriptTwo.cpp` /
`WePluginTestThrowScriptLib.cpp`)覆盖:坏描述/重复注册干净拒绝、账本可见、
真 Luau VM 调用(`hello.ping(2,3)==5`、共享命名空间、`pcall` 捕获插件异常)、
跨插件归属保护、存根两次渲染逐字节一致 + (插件 id, 函数名)顺序、
零插件等价路径,以及卸载 / 抛异常 / 析构三条兜底回收路径。
模板 `templates/plugin-lua-lib/**` 生成物可编译(`PLUG-AUTH` 探针的构建断言);
引擎插件与项目插件在编辑器里加载后,`plugin.list` 状态为 `loaded`。

> **§4 模板表的更正(T4 落地)**:`lua-lib` 行原写 `requires: t4`、"注册面未就绪" ——
> 该模板的 `template.json` 已改为 `requires: none`,源码换成
> `templates/plugin-lua-lib/src/plugin.cpp` 里的真实注册(命名空间 = 插件 id 最后一段);
> 以本节为准。

## 11. 组件存储桥(T2c,2026-09-30)

T2b 的组件注册只有 schema(`Storage == nullptr`,不能挂到场景实体上)。T2c 在
`WeComponentDesc` **尾部追加** `Size` / `Alignment`(append-only:不升
`WE_PLUGIN_ABI_VERSION`),宿主据此为插件组件**合成 entt blob 存储** —— 组件从此能挂到
场景实体上,Add/Remove/Copy/`.wd` 序列化/属性面板全部走既有 schema 通路。

```cpp
struct WeComponentDesc {
    StructSize, AbiVersion, Id, DisplayName, ComponentId, Fields, FieldCount,
    uint32_t Size;       // sizeof(插件组件结构);0 = 保持 T2b 的 schema-only 行为
    uint32_t Alignment;  // alignof(插件组件结构);0 = 未声明
};
```

- 声明规则:`Size > 0` 才启用存储桥;`Alignment != 0` 时必须是 2 的幂且 ≤ 16
  (宿主 blob 的 `alignas(16)`),且 `Size` 是它的整数倍;`Size = 0` 时 `Alignment` 必须为 0。
  每个字段必须落在 `Size` 之内(`Offset + 字段规范大小 <= Size`),否则注册被拒绝。
  `Size > 2048`(最大档)= 干净拒绝;所有失败都不半注册。
- **尺寸档位**:16 / 32 / 64 / 128 / 256 / 512 / 1024 / 2048 字节。宿主取**最小覆盖**
  声明大小的一档,blob 类型 = `PluginComponentBlob<Id>`(对齐 16,blob 地址 == `Bytes`
  地址 ⇒ 字段访问器写的 `实例 + Offset` 就是 `Bytes + Offset`)。
- **组件 id 段(写死)**:

  | 位 | 含义 |
  | --- | --- |
  | bit31 | `1` = 插件组件(保留高位段,与 entt 类型哈希段区分) |
  | bit 8..10 | 尺寸档位 0..7(对应档位表) |
  | bit 0..7 | 槽位 0..63(管理器分配的在册序号;首期容量 64) |

  `id = 0x80000000 | 档位 << 8 | 槽位`。档位进 id 是必需的:entt 的
  `registry.storage<Blob>(id)` 把 id 绑到存储类型上,复用槽位换档位必须落在不同 id。
  槽位/档位都相同的重载 = 同 id 同类型,直接复用(不留脏账)。
  与引擎/Game 现有组件同 id 时,`SchemaRegistry` 的 `DuplicateComponentId` 让注册
  干净失败(可读诊断),不会半注册。
- `TypeSchema::Size` = 插件声明的 `Size`;`Storage = { ComponentId, Add, Copy, CopyAll }`:
  `Add` = 给实体加一个清零的 blob;`Copy` = `Scene::DuplicateEntity`;`CopyAll` =
  `Scene::CopyScene`(编辑器 Play 的活动场景副本)—— 与引擎组件
  (`ComponentSchemaBridge.h` 的 `MakeComponentStorage<T>`)同语义。
- **卸载门(活实例)**:插件组件在**活场景**(同一 `WorldContext`)里还有实例时,
  `PluginManager::Unload` 返回 `Status::HasLiveInstances` + 可读原因
  (列出类型与实例数),**不调用插件 Unregister、不注销类型、不释放 DLL**。先移除组件 /
  销毁场景再重试;`UnloadAll` 对这类插件记 WARN 后跳过。活实例查询 =
  `Scene::CountLiveComponentInstances(componentId, context)`(Scene 构造/析构维护的活场景表,
  只读 `storage(id)->size()`)。单类型注销(`UnregisterComponent`)保留 T2b 语义,不做活实例门。
- 模板:PLUG-AUTH 探针里 `probe-component` 生成物可编译、`plugin.list` 状态为 `loaded`
  (与 T2b 同一条断言,不退化)。

单测 `World.Plugins`(用例 T2c)覆盖:注册产物(`Storage != nullptr`、`Size` = 声明值、
保留段 id、`FindByComponentId` 自洽)、按 id 挂到实体 + blob 初始清零、
`FieldSchema::Set/Get` 按插件 `offsetof` 逐字节成立、`DuplicateEntity` / `CopyScene`
两条复制通路、`.wd` 往返一致、插件缺失时未知组件 YAML 片段保留、移除组件后类型仍在、
有活实例时 Unload 被拒 / 清实例后成功 + 槽位与绑定可复用、单类型注销后同模块其余类型仍可用、
以及 `Size = 0` 的旧式 schema-only 行为不变。


## 12. 打包:显式启用 + 依赖闭包 + 引用完整性硬门(T5,2026-09-30)

### 项目清单的 `plugins:` 区块

`project.we.yaml` 新增 `plugins:` 区块(cook 读写;缺块 = 全默认,**老清单行为逐字节不变**):

```yaml
plugins:
  enabled: [engine.hello-import, com.example.core]   # 显式启用(引擎插件需要在这里出现才随包)
  shipped: [com.example.core, engine.hello-import]   # cook 写入发行清单的随包插件(依赖拓扑序)
  tolerate_missing: [com.example.optional]           # 引用缺件的唯一例外(降级为 ERROR + 摘要计数)
```

- `enabled`:引擎插件必须在此出现才随包;**项目插件默认随包**,只有写在插件自己 `plugin.we.yaml`
  里的 `ship: never` 才能排除;显式启用了 `ship: never` 的插件 = 记 WARN 后跳过;
- `shipped`:只由 cook 写进**发布目录**的清单;Runtime 按它加载 `<exe>/bin/plugins/*.dll`;
- `tolerate_missing`:缺省空;列出的插件,其引用缺件降级为 ERROR 日志 + `missing-references` 计数;
  其余情况"引用落在闭包外"= **cook 失败**。

### 插件清单的 `contributes:`

引用硬门要回答"内容里的这个 id 是哪个插件提供的",索引来源是每个插件的**静态声明** ——
cook 因此**不加载插件 DLL**(打包是确定性、可审计的步骤,不执行插件代码):

```yaml
contributes:
  components: [com.example.health]              # .wd 里的组件类型 id(WeComponentDesc::Id)
  asset_types:                                  # 资产类型:内容里以扩展名出现
    - id: health
      extensions: [.whealth]
  importers:                                    # 导入器:内容里的源扩展名
    - id: health.whealth
      extensions: [.whealth]
  script_namespaces: [health]                   # Lua 函数库命名空间(health.ping)
```

- 扩展名规范化:小写 + 前导点(`WHEALTH` → `.whealth`);未写扩展名的面"扫不到",该面按 0 计;
- 未知面 / 在组件面写扩展名 / 空 id = 清单被**干净拒绝**(不猜语义);
- **声明必须与实际注册一致**:插件加载成功后 `PluginManager::WarnContributionDrift` 逐条比对
  声明与运行时注册的 id,不一致记 WARN(声明漏了 = 打包门禁漏报,不能静默)。

### 闭包与产物

- 随包闭包 = `enabled` ∪ **项目插件(`ship != never`)** ∪ 闭包内插件的 `depends` 传递闭包,
  输出依赖拓扑序;
- 闭包内插件的 DLL → `<publish>/bin/plugins/<name>.dll`;引擎插件产物从
  `<repo>/build/x64-<cfg>/bin/<cfg>/plugins/<cfg>/`(或包自带 `bin/`)解析,项目插件从
  `<项目根>/build/x64-<cfg>/bin/<cfg>/plugins/<cfg>/`(或包自带 `bin/`)解析;
- cook 摘要固定格式(探针断言点):
  `[plugins] shipped=<id,id|none> skipped=<id,id|none> missing-references=<n>`;
- 逐面命中数另记一行 INFO:
  `[plugins] references: components=… asset-types=… importers=… scripts=… render-hooks=0
  (face not provided by the plugin ABI; skipped)` —— ⑤ 渲染钩子当前没有注册面,显式跳过并记 0。

### 引用完整性硬门(扫不到该面就跳过并记 0)

| 面 | 扫描口径 | 引用落在闭包外 |
| --- | --- | --- |
| ① 组件 | `.wd` 实体里出现的组件类型键 | cook 失败 |
| ② 资产类型 | 内容里匹配声明扩展名的文件 | cook 失败 |
| ③ 导入器 | 内容里匹配导入器声明扩展名的源文件 | cook 失败 |
| ④ 脚本命名空间 | `.luau` / `.lua` 里的 `<命名空间>.` 调用(整行 `--` 注释跳过) | cook 失败 |
| ⑤ 渲染钩子 | 没有注册面 ⇒ 恒 0 | — |

诊断逐条给出**引用者 → 引用面 → 建议**:

```
assets/scenes/main.wd (entity 0) → component 'com.example.health' → enable plugin
'com.example.health-plugin' (project.we.yaml plugins.enabled) or declare it under plugins.tolerate_missing
```

被 `tolerate_missing` 覆盖的条目同样逐条记 ERROR,只是不让 cook 失败。
**已知边界**:没有被任何插件声明的 id(拼错、未安装)不在索引里,硬门无法判断 —— 它由内容/场景
各自的加载诊断负责;本机 `local/plugins.json`(编辑器偏好)不是打包输入,打包只认项目清单。

### Runtime 装载(发行形态)

- Runtime 读发行清单的 `plugins.shipped`(依赖拓扑序),加载 `<exe>/bin/plugins/*.dll`:
  每个 DLL 先 `WePluginQuery` 读出 id,再走与发现式加载**同一套**契约校验(ABI / StructSize /
  id 一致 / `Register` / 回滚);
- 清单列了但目录里没有 ⇒ ERROR + 报告失败(单条失败不影响其余);目录里多余(清单没列)⇒
  WARN 后忽略;
- **失败策略 = 可观测 + 不阻断启动**:缺失 / ABI 不符 / 注册失败记 ERROR 后继续启动(缺件在 cook
  的硬门里已经是致命错误);Runtime 不接线编辑器扩展 —— 只提供 `editor.panel` / `editor.command`
  的插件在这里按 T3b 契约干净拒绝,要随游戏发布且运行时可用,插件必须至少有一个运行期能力;
- 插件在场景 / GameHost 之前加载(schema 组件、资产类型、脚本函数必须先就位),装载结论强制
  刷一次日志(默认 INFO 不落盘,强杀进程会丢)。

### 验证

- 单测 `World.Plugins`(T5①–④):闭包(显式 + `depends` 传递 + `ship: never`)、四类引用面命中与
  "闭包外 ⇒ 失败"、`tolerate_missing` 降级、产物拷贝 + `LoadPackaged`(缺件 / 多余 DLL)、
  `plugins:` 块与 `contributes:` 的清单契约(含老清单向后兼容);
- 端到端探针 `tools/agents/scratch/PLUG-T5/verify-plugin-packaging.py`:
  cook 干净包(引擎 + 项目插件都随包、摘要一致)→ 引用缺件 cook 失败(带引用者诊断)→
  `tolerate_missing` 降级(计数保留 + ERROR)→ Runtime 按发行清单加载 2 个插件,**20 PASS**。

## 13. 热重载与泄漏审计(T6,2026-09-30)

### 两段式(与 `module.reload` 同款;Windows 会锁住已加载 DLL)

```
plugin.reload <id>   # ① loaded → 第一段:快照实例 + 卸载(释放文件锁,plugin.list = unloaded)
<外部重编该插件 DLL>  #    项目构建 / 引擎构建,产物路径与清单解析一致
plugin.reload <id>   # ② 未加载 + pending → 第二段:重读清单 + 载入新 DLL + 写回快照
```

- **第一段**(`PluginManager::UnloadForReload`):按"实体 UUID + 组件 id + 字段 id"快照**目标场景**里
  该插件的 blob 组件实例(字段值走 schema 访问器,卸载前完成)→ 从场景移除 → 卸载(账本回收 +
  `Unregister` + `FreeLibrary`)。卸载前把当前 DLL 拷成同目录 `<名>.rollback-<插件ABI>.dll`(只留最近一份)。
  同一 WorldContext 的**其它活场景**仍有实例,或场景处于 Play/Simulate(结构写门禁)= 干净拒绝,
  理由可读(与 T2c 的 `Unload` 活实例门同口径;普通 `plugin.unload` 不偷数据,仍拒绝)。
- **第二段**(`PluginManager::LoadForReload`):重读 `plugin.we.yaml`(拾取版本 / `contributes` 变化;
  id 变化 = 干净拒绝并要求重新发现)→ 载入新 DLL → 按**字段 id**(字段名 FNV-1a,回退字段名)把快照
  写回;实体消失 / 字段被删 / 字段 kind 变化 = 逐条诊断 + 计数(`instancesSkipped`),不写错值。
  任一步失败 ⇒ 载入回滚副本(旧 DLL)并用旧 schema 写回原状态:`rolledBack=true` + 可读诊断。
- 快照在管理器内存里:**没完成第二段就退出编辑器 = 那些实例数据丢失**;管理器析构 / `UnloadAll` /
  重新 `Discover` 会为未完成的 pending 记 ERROR(不静默)。
- `plugin.reload` 在"未加载且没有 pending"时等价于一次普通 `Load`(与 `module.reload` 的未加载分支一致)。

### AI 命令

| 命令 | 语义 |
| --- | --- |
| `plugin.unload <id>` | 卸载单个插件(T2c:有活组件实例 = 干净拒绝 + 可读原因);成功后关掉它的面板 |
| `plugin.reload <id>` | 两段式(上表);返回 JSON:`phase`(unloaded/loaded/rolled-back)+ `rolledBack` + `instancesSnapshotted/restored/skipped` + `message` + `diagnostics[]` |
| `plugin.info <id>` | 新增 `ledgers`(schema 类型 / 资产类型 / 导入器 / 编辑器命令 / 编辑器面板 / 脚本函数计数 + `total`)、`liveInstances`、`hasPendingReload`、`loadedLibrary` |

### 泄漏审计口径

宿主侧注册账本 = 资产类型 + 导入器 + 组件 schema 类型 + 编辑器命令 + 编辑器面板 + 脚本函数。
**卸载路径跑完必须全部归零**(`PluginManager::LedgerTotals()`);插件没自己注销的项由兜底回收
强删并记 WARN,`plugin.info` 的 `ledgers` 是可读证据。`World.Plugins` 的 T6 用例还做"连续 5 轮
load/unload 账本零增长 + 条目数不增长"。

### 验证

- 单测 `World.Plugins`(T6):快照/写回(5 个字段全等)、坏 DLL 回滚(旧 DLL + 原字段值)、
  有活实例时 `Unload` 与**单类型 `UnregisterComponent`** 都被拒、清实例后卸载成功、5 轮账本零增长;
- 端到端探针 `tools/agents/scratch/PLUG-T6/verify-plugin-reload.py`:真项目插件 + 真项目构建
  (`v1` → `v2` 标记证明加载的是新 DLL)+ 真场景字段值跨重载保留 + 坏 DLL 回滚 + 账本归零 + 活实例卸载被拒。

## PLUG-CLEAN-1 契约补充(2026-09-30)

### 引擎版本:`engine: ">=X.Y"` 的事实源与比较口径

- **宿主引擎版本的事实源 = 根 `CMakeLists.txt` 的 `project(World VERSION …)`**(当前 `2.0.0`)。
  该值经编译定义 `WLD_ENGINE_VERSION` 传给 `World`(`World/Plugins/PluginManifest.h` 的
  `HostEngineVersion()` 是唯一读取点);不在插件层复制第二份版本号。
- 清单 `engine:` 只接受 `">=X.Y"` 形态(或省略 = 不限制)。语义比较 = **宿主 `major.minor` ≥ 声明值**;
  `plugin.info` 输出宿主 `hostEngineVersion`,每个条目输出 `engineSatisfied`(空清单字段 = `true`)。
- 自带清单与事实源已对齐:`plugins/hello-import` 与 6 个 `templates/plugin-*/plugin.we.yaml` 声明 `>=2.0`,
  宿主事实源也是 `2.0.0`;约束不满足(例:`>=99.0`)⇒ **干净拒绝**并给可读诊断(见下一段)。
- **PLUG-CLEAN-2 更新(2026-09-30)**:宿主版本事实源已提升为
  `project(World VERSION 2.0.0)`(与自带清单的 `>=2.0` 对齐);`engine:` 约束不满足 ⇒ **硬拒绝**
  (`PluginState::Rejected` + `engine requirement '…' is not satisfied by host engine 2.0.0`,不进 loaded)。
- **加载失败必须干净拒绝,不能弹系统框**:`PluginManager::LoadRecord` 在 DLL 加载窗口内临时压制
  Windows 硬错误框(`SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX`),随后恢复进程原错误模式。
  坏产物 = 立即返回 `LoadLibraryA failed with Win32 error 193`(实测:进程默认错误模式为 0 时,
  `LoadLibraryA` 会阻塞在系统错误框上 —— ctest 子进程曾因此在 `World.Plugins` 卡死超时)。

### 插件组件显示名的本地化约定

- **约定键:`plugin.<pluginId>.<typeId>`**。`<typeId>` 先按组件类型**全名**(`WeComponentDesc::Id`,
  也是 `.wd` 里的类型键)查,未命中再按**短名**(最后一个 `.` 之后)查一次;都未命中 ⇒ 回落插件声明的
  `DisplayName`(英文 canonical)。
- 解析时机 = **插件加载 / 重载时**(组件注册进 schema 的同一生命周期),结果写入 `TypeSchema.DisplayName`:
  属性面板分区标题、Add Component 候选行(行 id = `prop.add.<DisplayName>`)与 `plugin.info` 共用同一份文案。
  因此**切换语言后需要 `plugin.reload`(面板「重新加载」按钮)或重启编辑器**才会重新解析插件组件的显示名;
  内置组件不受此限。
- 语言包分层(优先级从低到高):引擎层 `Engine/assets/localization/<lang>/**` → 编辑器层
  `Editor/assets/localization/<lang>/**` → 项目层 `<项目根>/assets/localization/<lang>/**`。
  插件作者/项目可以在任意一层提供该约定键的译文;缺条目回落声明的 `DisplayName`,不会出现裸键。

### 面板「重新加载」按钮与两段式热重载的会话内契约

- 插件管理器面板的动作 `plugins.action.reload` 与 AI `plugin.reload` 走**同一条实现**
  (`EditorLayer::ReloadPlugin` 两段式):面板只登记请求(`PluginManager::RequestReload`),
  宿主在**下一帧开头**执行 —— 面板绘制期间不卸载/装载 DLL(避免打断面板遍历)。
  详情行同时显示 `pendingReload` 与 `ledgers` 摘要(与 `plugin.info` 同一数据源)。
- **快照只活在本编辑器会话**:热重载快照在内存里,不落盘、不跨编辑器重启;没完成第二段就退出编辑器
  = 那些组件实例的数据丢失(管理器析构 / `UnloadAll` / 重新 `Discover` 会为未完成的 pending 记 ERROR)。
  面板按钮与 `plugin.reload` 的提示文案都写明这一点。
- 第一段(loaded → 快照 + 卸载)之后 DLL 文件锁已释放,可以外部重编;失败 ⇒ 自动回滚到
  `<名>.rollback-<插件ABI>.dll` 并用旧 schema 写回原状态(`rolledBack=true` + 可读诊断)。
