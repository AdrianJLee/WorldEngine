# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **C++ Library Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。

## 这个模板注册什么

导出表里有一个示例函数 `{{PluginDir}}.ping`(版本 1),返回 `uint32_t`。

- 自己的导出函数写在 `src/plugin.cpp` 的导出表 `kExports` 里;
  名字建议用 `<插件短名>.<函数>` 形状,避免不同插件互相撞名。
- 调用方(宿主或其它插件)通过宿主能力表 `WeHostApi::LookupExport(userData,
  "<插件 id>", "<导出名>", <最低版本>)` 取得函数指针;查询未命中返回 `nullptr`。
- 插件 ABI 是 C 边界:导出表里只放函数指针,不要跨边界传 STL 对象。
