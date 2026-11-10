# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Empty Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它;面板里能看到
  id / 状态 / 根路径,以及本轮加载失败的原因。

## 这个模板做什么

只导出 `WePluginQuery` + `Register` / `Unregister` 两个生命周期回调,不注册任何东西。

- `src/plugin.cpp` 里的 `Register` 是加自己注册面的地方;`Unregister` 必须对称地清掉。
- 插件只依赖公共 ABI 头 `World/Plugins/WePluginApi.h`,**不链接 World** ——
  这是第三方插件的边界,不能改成链接引擎运行时。
