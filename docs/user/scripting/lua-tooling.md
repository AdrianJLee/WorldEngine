# 脚本与代码提示

引擎内嵌的脚本运行时是 **Luau**。工程里用**语言服务器**提供补全:`<项目根>/.vscode/settings.json` 与
`<项目根>/.luau-lsp/config.json`(编辑器新建项目时生成)给 Luau LSP 用;仓库根的 `.luarc.json` 给 LuaLS(`sumneko.lua`)用,
指向引擎内的入库存根样本。项目不会替你改编辑器设置,也不会自动装扩展。

## 工作区

| 打开方式 | 生效的配置 | 补全来源 |
| --- | --- | --- |
| 用 VS Code 打开**项目根** | `<项目根>/.vscode/settings.json` + `<项目根>/.luau-lsp/config.json` | `<项目根>/assets/scripts/intermediate/WorldEngineAPI.luau`(编辑器生成) |
| 用 VS Code 打开引擎仓库根 | 根 `.luarc.json` | `tests/fixtures/content/scripts/intermediate/WorldEngineAPI.luau`(入库存根样本) |

`.luarc.json` 里的 `runtime.version` 是 LuaLS 自身的设置项(LuaLS 没有 Luau 运行时档位);
实际执行脚本的是引擎里的 Luau 版本,两者不是同一个东西。

## 写一个系统脚本

系统脚本放在 `<项目根>/assets/scripts/systems/`。它不是"类",是**直接执行的脚本**:
用 `ecs:Query` 建查询,用 `ecs:AddSystem` 把系统挂进帧管线。

```lua
-- assets/scripts/systems/player_movement.luau
local movement = ecs:Query({ "TransformComponent", "VelocityComponent" })

-- 规范签名 = (名字, 函数 [, 阶段]);阶段省略 = "Update"。
ecs:AddSystem("PlayerMovementSystem", function(dt)
    movement:Each(function(entity, transform, velocity)
        local direction = vec3.new(1.0, 0.0, 0.0)
        transform.Location.x = transform.Location.x + velocity.Linear.x * dt
        print(entity:GetID(), direction:length())
    end)
end, "Update")
```

在 `movement:Each(` 的回调里,`transform` / `velocity` 是**组件代理**,字段带补全;
输入 `vec3.new(` 后看参数提示;悬停 `dt`、`transform.Location` 查看类型。
构造数学对象用实际绑定的 `.new(...)`。

`<项目根>/assets/scripts/intermediate/WorldEngineAPI.luau` 由编辑器按**实际注册的绑定**生成,仅供语言服务器读取,
**不要 require 或运行它**。编辑器注册完 API 会自动刷新(内容不变就不改写,失败保留原文件),
也可以从菜单手动刷新。Runtime 不生成提示文件。

需要 Lua 视野里的"结构"（例如一张怪物配置表）时,在 `scripts/lib/` 里用 `---@class` / `---@field`
声明注解表(放在 `scripts/lib/` 下)—— 那只是类型注解,不参与存储与序列化。
注意:沙箱里 `require` 是被禁用的,`scripts/lib/` 的文件**不会**在运行时被装载,
它是"放可复用源码 + 注解"的地方,用的时候复制进系统脚本。

## 结构修改

Entity 句柄会校验场景寿命与实体代数。脚本可以用 `ecs:CreateEntity(name)`、
`ecs:DestroyEntity(entity)`、`entity:RemoveComponent(name)` 请求结构变更:
**当前回调先返回,随后在安全点统一提交**;待删除的对象不会再被更新。

失效句柄的调用返回可定位的错误;跨帧保留底层组件指针是不允许的。

原生代码里需要创建并配置实体时,用显式命令并按值捕获参数或 Entity 句柄:

```cpp
GetEntity().GetScene()->DeferStructuralChange([position](World::Scene& scene) {
    auto entity = World::Entity::CreateEntity(&scene, "Spawned");
    entity.AddComponent<World::TransformComponent>().SetLocation(position);
});
```

命令在安全点按批次提交,来源实体失效时取消。不要捕获裸 `this`、组件引用或局部变量引用。

## 热重载

- 改 `<内容根>/scripts/systems/` 下的脚本,**保存即生效**:编辑器在帧边界轮询,内容变化后 150ms 消抖,
  然后**整份重跑**该系统脚本（先撤销它上次注册的系统,再重新执行）。
- 内容改回原值不算变化;只动 mtime 不算变化（内容哈希优先）。
- 宿主手写 `ecs:AddSystem`（不在系统脚本加载作用域内）的系统**不参与**脚本热重载。
- 发行形态（`Runtime.exe` + 打包内容）没有监听器;详见 [热重载生效矩阵](../../dev/hot-reload.md)。

## 排查

| 症状 | 先查什么 |
| --- | --- |
| 没有补全 | 打开的是仓库根或 `Game/`(配置见上表);语言服务器索引是否完成;声明基线是否存在 |
| `ecs` 标红未定义 | 声明基线是否包含 `ecs` 块;重新构建并启动编辑器看生成是否报错;不要手改生成文件 |
| 系统不生效 | 文件是否在 `<内容根>/scripts/systems/` 下;`ecs:AddSystem` 的名字是否与别处重名（同名会替换） |
| `ui.*` 没反应 | 是不是**没在 `ui.onDraw` 回调里**调?UI 只在 UI 阶段可用 —— 在系统脚本顶层直接 `ui.panel(...)` 会抛 "can only be called during the UI phase" |

`tests/World/lua/CompletionProbe.lua` 有意包含错误,用于语言服务器的验收;日常工作区已把它排除在索引之外。
