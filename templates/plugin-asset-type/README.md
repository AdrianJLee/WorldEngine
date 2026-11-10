# {{PluginName}}

{{PluginScope}} 插件,由编辑器「文件 ▸ 新建插件…」按 **Asset Type Plugin** 模板生成。

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

- **资产类型** `{{PluginDir}}`(扩展名 `.{{PluginDir}}`):内容浏览器里「新建」会写出
  一份模板文件 `new.{{PluginDir}}`;类型描述与「新建」回调见 `src/plugin.cpp`。
- **导入器** `{{PluginDir}}.asset`:`Matches` 按 `.{{PluginDir}}` 扩展名接手,
  `Import` 给源文件加一行 `WEASSET1` 头作为单产物写进宿主 sink。

改这几处就能变成自己的资产类型:`kAssetTypeId` / `kImporterId` / `kExtension`,
以及 `CreateAsset` 与 `ImportSource` 两个回调。资产类型是全局注册面:
**重复 id 会注册失败**(`RegisterAssetType` 返回 false,插件会整体回滚)。
