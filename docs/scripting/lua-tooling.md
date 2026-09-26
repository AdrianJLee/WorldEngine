# 脚本与代码提示

引擎内嵌的脚本运行时是 **Luau**。工程里用**语言服务器**提供补全:根 `.luarc.json` 给 LuaLS
(`sumneko.lua`)用,`Game/.vscode/settings.json` 与 `Game/.luau-lsp/config.json` 给 Luau LSP 用,
两者指向同一份声明文件。项目不会替你改编辑器设置,也不会自动装扩展。

## 工作区

| 打开方式 | 生效的配置 | 补全来源 |
| --- | --- | --- |
| 用 VS Code 打开仓库根 | 根 `.luarc.json` | `projects/default/assets/scripts/intermediate/WorldEngineAPI.luau` |
| 用 VS Code 打开 `Game/` | `Game/.vscode/settings.json` + `Game/.luau-lsp/config.json` | 同上(路径按 `Game/` 相对) |

`.luarc.json` 里的 `runtime.version` 是 LuaLS 自身的设置项(LuaLS 没有 Luau 运行时档位);
实际执行脚本的是引擎里的 Luau 版本,两者不是同一个东西。

## 写一个脚本

从 `projects/default/assets/scripts/templates/WorldScript.lua` 复制新脚本,修改类名和字段。用
`---@class YourScript : WorldScript`、`---@field`、`---@param` 描述自己新增的类型。脚本必须返回一个 table。

```lua
---@class PlayerScript : WorldScript
---@field Speed number 移动速度
local PlayerScript = { Speed = 5.0 }

---@param dt number 距离上一帧的秒数
function PlayerScript:OnUpdate(dt)
    local direction = vec3.new(1.0, 0.0, 0.0)
    print(self.entity:GetID(), direction:length(), self.Speed * dt)
end

return PlayerScript
```

在 `self.entity:` 后使用补全;输入 `vec3.new(` 后看参数提示;悬停 `direction`、`dt`、`self.Speed` 查看类型。
`WorldScript` 只描述脚本形状,不能调用 `WorldScript.new()`;构造数学对象用实际绑定的 `.new(...)`。

`projects/default/assets/scripts/intermediate/WorldEngineAPI.luau` 由编辑器按**实际注册的绑定**生成,仅供语言服务器读取,
**不要 require 或运行它**。仓库保留生成基线;编辑器注册完 API 会自动刷新(内容不变就不改写,失败保留原文件),
也可以从菜单手动刷新。新增 C++ 侧脚本 API 后要重新构建并启动编辑器才会出现在声明里;Runtime 不生成提示文件。

目前 `Entity:GetComponent(name)` 返回不透明的 `userdata|nil`,没有通用组件字段代理,因此不承诺 Transform
等组件字段的补全。`GetID()` 返回当前场景的运行句柄整数,不是持久 UUID。`self.__Entity` / `self.__EntityID`
保留旧脚本兼容,新脚本用 `self.entity`。

## 生命周期和结构修改

场景拥有并在同一线程调用 `OnCreate`、`OnUpdate(dt)` 和 `OnDestroy`。创建回调进入后,销毁至多尝试一次;
回调出错后清理实例并保留 Faulted 诊断,下次明确 Play 才重试,其他脚本继续运行。停止场景会取消未执行的创建命令并清理运行引用。

Entity 句柄会校验场景寿命与实体代数。脚本可以用 `self.entity:Destroy()`、`RemoveComponent(name)` 请求删除:
当前回调先返回,随后统一提交 ;待删除的对象不会再被更新。`OnDestroy` 里仍可读取本实体现有的组件。
失效句柄的调用返回可定位的错误,跨帧保留底层组件指针是不允许的。

原生代码里需要创建并配置实体时,用显式命令并按值捕获参数或 Entity 句柄:

```cpp
GetEntity().GetScene()->DeferStructuralChange([position](World::Scene& scene) {
    auto entity = World::Entity::CreateEntity(&scene, "Spawned");
    entity.AddComponent<World::TransformComponent>().SetLocation(position);
    entity.AddComponent<World::LuauScriptComponent>("scripts/Test.lua");
});
```

命令在安全点按批次提交,来源实体 / 脚本失效时取消。不要捕获裸 `this`、组件引用或局部变量引用。
旧式同步 Create/Add(返回对象或引用)不能在回调里使用,会在写入前被拒绝。已有运行中、待创建或失败的脚本
不能直接替换:先移除并完成清理,再在后续阶段添加。

运行中 Inspector 禁用脚本重绑、解绑与自动重载;缓存的起始字段在停止后编辑。改完脚本要 **Stop 再 Play** 才生效,
刷新声明不等于热重载。暂停时仍会提交结构请求,但不执行 `OnUpdate`。可以删除已有物理 body 或移除刚体;
运行中新增 / 替换刚体与 collider、以及破坏 Transform 依赖的操作会被拒绝。

## 排查

| 症状 | 先查什么 |
| --- | --- |
| 没有补全 | 打开的是仓库根或 `Game/`(配置见上表);语言服务器索引是否完成;声明基线是否存在 |
| 自定义字段没有类型 | 给脚本的 class / field 和回调参数补注解;类名是否唯一 |
| API 变更没出现在补全里 | 重新构建并启动编辑器,看生成是否报错;不要手改生成文件 |
| 生命周期报错 | 看 Inspector 状态与引擎日志里的文件、实体、阶段和 traceback,修好后重新 Play |

`tests/World/lua/CompletionProbe.lua` 有意包含错误,用于语言服务器的验收;日常工作区已把它排除在索引之外。
