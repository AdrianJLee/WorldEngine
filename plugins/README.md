# plugins/

**放"随引擎分发/给编辑器加载"的插件包**:每个子目录一个插件
(`<引擎根>/plugins/<名>/{plugin.we.yaml, CMakeLists.txt, src/**}`)。

构建期由根 `CMakeLists.txt` 的 `plugins/*/CMakeLists.txt` 通配收集
(`WORLD_BUILD_ENGINE_PLUGINS`,产物落 `build/x64-<配置>/bin/<配置>/plugins/`);
编辑器启动时扫描这个目录并加载(可用编辑器偏好里的插件开关按 id 禁用,
清单写在 `local/plugins.json`)。

写插件**从这里开始**:用编辑器 `文件 ▸ 新建插件…`,选一个模板
(`templates/plugin-*/`:`plugin-empty` / `plugin-asset-type` / `plugin-component` /
`plugin-editor-ui` / `plugin-library` / `plugin-lua-lib`)。

## 这个目录里现在没有示例插件

以前这里躺着一个示例插件 `hello-import`(注册资产类型 `hello` / `.whello`)。
它会被编辑器每个会话都加载,于是**用户的右键「新建」菜单里会多出一条 "Hello File"** ——
对做游戏的人没有任何用处。示例插件的价值在于覆盖宿主侧的插件加载路径,所以
2026-10-02 把它搬去当测试夹具:`tests/fixtures/plugins/hello-import`
(由 `tests/CMakeLists.txt` 显式构建,`World.Plugins` 仍然做真 DLL 加载)。

想找"怎么写一个插件"的样板,看 `templates/plugin-*/` 与
`docs/dev/plugin-framework.md`。
