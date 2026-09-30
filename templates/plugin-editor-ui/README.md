# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Editor Extension Plugin** 模板生成。

- 插件 ID:`{{PluginId}}`
- 目录名 / 产物名:`{{PluginDir}}`
- 清单:`plugin.we.yaml`(字段口径见引擎的 `docs/dev/plugin-framework.md`)

> **T3b 起是真实注册**:`src/plugin.cpp` 在 Register 里经
> `WeHostApi::RegisterEditorCommand` / `RegisterEditorPanel` 注册一条命令与一个面板,
> Unregister 里成对注销。面板是**独立窗口形态**(默认附加到主窗口的标签切换,可拖出为
> 独立 OS 窗口),内容用宿主给的**小组件表**(`WeEditorUiApi`:Label / Button /
> Separator / Checkbox)绘制;面板里的按钮经 `WeEditorUiApi::InvokeCommand` 触发本插件
> 注册的命令 —— 与 AI 通道 `plugin.command.run` 同一条宿主命令路径。
>
> 注册面命名(宿主统一加命名空间,插件只需面板内唯一的短 id):
> - 面板:`plugin.panel.<插件 id>.<面板 id>`(窗口菜单 / `ui.open` / a11y 根 id)
> - 命令:`plugin.command.<插件 id>.<命令 id>`(AI `plugin.command.run` 触发)

## 构建

- **engine** 形态(目录在 `<引擎根>/plugins/{{PluginDir}}`):重跑一次引擎 CMake configure,
  再构建 `ALL_BUILD` → `<引擎根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- **project** 形态(目录在 `<项目根>/plugins/{{PluginDir}}`):用项目自带的 `build.cmd`
  (或 CMake)构建 → `<项目根>/build/x64-<配置>/bin/<配置>/plugins/<配置>/{{PluginDir}}.dll`。
- 编辑器的插件管理器(「窗口 ▸ 插件」)在**下次启动**时发现并加载它。
