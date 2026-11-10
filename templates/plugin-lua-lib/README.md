# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Lua Library Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)

本模板**真实注册**一个全局 Luau 函数(经 `WeHostApi::RegisterScriptFunction`,
定义见引擎的 `Engine/src/World/Plugins/WePluginApi.h`):

- 命名空间 = 插件 id 的最后一段(非法字符替换为 `_`,数字开头加 `plugin_` 前缀);
- 函数名 = `<命名空间>.ping(name?: string): string`,脚本里直接调用:

```lua
local reply = {{PluginDir}}.ping("world")
```

> 提示:命名空间在运行时由插件 id 推导;两个插件用同一个最后一段会共享同一张全局表
> (函数名仍然全局唯一)。需要独占命名空间时,把插件 id 的最后一段起成独占的名字。

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。
- 加载后脚本即可调用注册的函数(存根 `WorldEngineAPI.luau` 会随加载的插件更新)。
