# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Lua Library Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)

> **注册面未就绪**:插件侧脚本库注册面属于 **T4**,当前 ABI 还没有对应的注册函数。
> 本模板现在生成的是**可编译骨架**:清单声明 `script.library`,源码里用 TODO 标出将来
> 要换成真实注册的位置,不引用任何尚不存在的 API。T4 落地后把 TODO 换成
> `host.RegisterScriptLibrary(...)` 即可。

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。
