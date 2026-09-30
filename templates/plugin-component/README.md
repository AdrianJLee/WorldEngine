# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Component Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)
- 组件类型:`{{PluginId}}.Health`(`float Health` + `int Charges` 示例布局 —— 换成你的字段)

## 注册面(T2b 起为真实注册)

`src/plugin.cpp` 在 `Register` 里调用 `WeHostApi::RegisterComponent`,把组件布局(每个字段的
Kind / Offset / Size,用 `offsetof` / `sizeof` 填写)交给宿主;宿主据此在
`Schema::SchemaRegistry` 里生成一个 `TypeCategory::Component` 类型,并在 `Unregister` 里
成对注销(`UnregisterComponent`)。注册的类型可被注册表查询(`Find` / `ListByModule`)与
序列化 API 读写。

边界:

- 注册产物是**组件 schema**;**存储桥(entt)尚未提供** —— `WeComponentDesc::ComponentId`
  必须为 0,组件暂时不能挂到场景实体上(挂载能力在后续 ABI 尾部追加存储回调后启用);
- 字段类型支持固定大小 POD(布尔 / 整型 / 浮点 / 向量 / 四元数 / 矩阵);String / Enum /
  Asset / Object 与容器字段会被宿主干净拒绝(返回 false + 可读日志);
- 字段名就是 `.wd` / YAML 里的键,同一个组件内必须唯一。

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。
