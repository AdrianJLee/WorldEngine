# 测试夹具:Game 模块的 schema(示例组件)

这是 `World.ScriptWorkflow` / `Game.SchemaDrift` 两个测试用的**最小 Game 模块**:

```
src/Components/SampleDataComponent.h      schema 组件示例(WE_SCHEMA_BODY)
src/Generated/Game.manifest               类型账本(schema-compiler 的输入)
src/Generated/Game/GameSchemaRegistration.*  生成物(schema-compiler 的输出)
```

## 为什么在夹具里而不是在 `Game/src`

PROJ-8/T2 起引擎仓库的 `Game/src` 只留**模块骨架**(`GameAPI` / `GameLayer`);示例 C++ 属于
游戏项目层,随 `templates/project-example/src/**` 分发、由项目自己的 `build.cmd` 编进该项目
的 `Game.dll`。测试因此改指这份夹具:它和示例模板同名同形(同样是 `Game::SampleDataComponent`),
既让 `World.ScriptWorkflow` 的存根漂移门禁
(`tests/fixtures/content/scripts/intermediate/WorldEngineAPI.luau`)保持逐字节可比,
又不再依赖模板目录或引擎 `Game/src` 的任何文件。

## 重新生成 / 漂移门禁

改了夹具里的 `WE_SCHEMA_*` 声明就要重跑生成器,否则 `Game.SchemaDrift` 红灯:

```powershell
& .\build\x64-Debug\Engine\generators\schema-compiler\Debug\schema-compiler.exe `
    --input tests/fixtures/game-module/src/Components/SampleDataComponent.h `
    --module Game `
    --reg-include "Components/SampleDataComponent.h" `
    --output tests/fixtures/game-module/src/Generated `
    --manifest tests/fixtures/game-module/src/Generated/Game.manifest
```

`game-schema` 的台账口径与 `Game/CMakeLists.txt` 完全一致:输入发现 =
`src/Components/*.h`(纯 ECS 起项目层 C++ 只有组件会进 schema 反射),
生成物回写 `src/Generated/**`(`copy_if_different`)。

> PURE-ECS(2026-10-02):`Category==Script` 与 `Schema::ScriptBinding` 已整体删除,
> 夹具里的示例脚本(`ExampleScript`)随之退场 —— 逻辑一律写成系统(C++ `World::ISystem`
> 或 `<内容根>/scripts/systems/*.luau`),不再有"可挂到实体上的脚本类型"。
> 项目层 C++ 布局随之收敛为 `src/Components/`(纯数据)+ `src/Systems/`(逻辑)+
> `src/GameProject.cpp`(系统挂载入口;可选文件),见 `docs/dev/project-standard.md`。
