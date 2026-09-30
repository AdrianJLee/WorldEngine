# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Component Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)
- 组件类型:`{{PluginId}}.Health`(`float Health` + `int Charges` 示例布局 —— 换成你的字段)

## 注册面(T2b 起为真实注册;T2c 起能挂到场景实体上)

`src/plugin.cpp` 在 `Register` 里调用 `WeHostApi::RegisterComponent`,把组件布局(每个字段的
Kind / Offset / Size,用 `offsetof` / `sizeof` 填写)交给宿主;宿主据此在
`Schema::SchemaRegistry` 里生成一个 `TypeCategory::Component` 类型,并在 `Unregister` 里
成对注销(`UnregisterComponent`)。注册的类型可被注册表查询(`Find` / `ListByModule`)与
序列化 API 读写。

结构总大小与对齐用 `WeComponentDesc::Size` / `Alignment` 声明(T2c):

- `Size = sizeof(你的组件结构)`(> 0)⇒ 宿主为它合成固定尺寸 blob 存储,组件能挂到场景
  实体上:Add/Remove/Copy/`.wd` 序列化/属性面板全部走既有 schema 通路;
  最大档见 `docs/dev/plugin-framework.md` 的档位表(超过 = 注册被干净拒绝);
- `Alignment = alignof(你的组件结构)`(0 = 未声明;非 0 必须是 2 的幂且 ≤ 16);
- 每个字段必须落在 `Size` 之内(否则注册被拒绝 —— 宿主不会按越界的偏移读写);
- `Size = 0` = T2b 的旧式 schema-only 组件(注册表可见,但不能挂到实体上)。

边界:

- `WeComponentDesc::ComponentId` 必须为 0:组件存储 id 由**宿主**分配(插件不选 id);
- 字段类型支持固定大小 POD(布尔 / 整型 / 浮点 / 向量 / 四元数 / 矩阵);String / Enum /
  Asset / Object 与容器字段会被宿主干净拒绝(返回 false + 可读日志);
- 字段名就是 `.wd` / YAML 里的键,同一个组件内必须唯一;
- 组件类型有活实例(挂在活场景实体上)时插件**不会被卸载**(宿主干净拒绝并给可读原因)——
  先移除组件 / 销毁场景再卸载。

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。
