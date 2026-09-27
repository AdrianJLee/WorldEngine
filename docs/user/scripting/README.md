# 脚本

引擎内嵌 **Luau**。脚本以组件的形式挂在实体上,由引擎驱动生命周期回调。

## 快速开始

1. 复制模板 `projects/default/assets/scripts/templates/WorldScript.lua`,改类名与字段,放到内容树的脚本目录下。
2. 给实体添加脚本组件,填脚本路径(相对内容根,例如 `scripts/MyScript.lua`)。
3. `Play`:引擎为每个脚本组件创建实例,并调用 `OnCreate` / `OnUpdate(dt)` / `OnDestroy`。

```lua
---@class MyScript : WorldScript
---@field Speed number 速度
local MyScript = { Speed = 5.0 }

---@param dt number 距上一帧的秒数
function MyScript:OnUpdate(dt)
    print(self.entity:GetID(), self.Speed * dt)
end

return MyScript
```

- 脚本必须返回一个 table;`self.entity` 是当前实体句柄。
- 用 `---@class` / `---@field` / `---@param` 描述自己新增的类型,补全与悬停提示会跟着变好。

## 属性与类型（由脚本声明生成）

属性面板直接反映脚本里的声明,不需要在编辑器里另配一份:

- **基础与引擎类型**:`number` / `boolean` / `string`,以及 `vec2` / `vec3` / `vec4`
  (行控件与数值编辑器一致;未赋值保持"未设",不会变成 0)。
- **嵌套结构**:用 `---@class` 声明结构、`---@field Stats SomeClass` 引用它,会展开成可折叠子行;
  未注解的 `table` 只显示只读摘要。
- **数组与映射**:`{number}`、`{string: number}`(可嵌套)显示元素行并支持增删;元素形状由场景持有,存读往返保持。
- **类型推导**:没写 `---@field` 的字段也会出现 —— 能从初值推出类型的进入属性表,推不出的保留只读摘要。
- **提示与重置**:悬停显示注解说明,没有说明时回落显示类型名;行尾 `↺` 只把该值恢复为"未设"(回到脚本默认值),
  集合表头的重置会整表复原并先确认。
- 字段名按脚本里的声明原样显示(不本地化)。

脚本示例见 `projects/default/assets/scripts/examples/FeatureShowcase.lua`;属性模型变化会同步升
`WE_MODULE_ABI_VERSION`(当前 5)。

## 生命周期与结构修改

- 三个回调都由场景在**同一线程**调用;实例由场景拥有与释放。
- 运行中要改场景结构(销毁实体、增删组件)必须**请求**,由引擎在安全点按批次提交 —— 当前回调先返回。
  在回调里直接同步创建/删除的旧写法会被拒绝。
- `OnCreate` 出错时保留诊断信息,并清理该实例;下次再 Play 才重试。
- 运行中 Inspector 会禁用脚本重绑、解绑与自动重载;要让改动生效,**Stop 后重新 Play**。

## 代码提示

- 编辑器按实际注册的绑定生成 `projects/default/assets/scripts/intermediate/WorldEngineAPI.luau`;它是声明基线,
  **不要** `require` 或运行它。
- VS Code 打开仓库根即可(根 `.luarc.json` 生效);`projects/default/.vscode/settings.json` 与
  `projects/default/.luau-lsp/config.json` 给 Luau LSP 喂同一份声明。
- 细节(工作区配置、补全范围、已知限制)见 [Lua 脚本与代码提示](lua-tooling.md)。

## 调试

- 脚本错误会带上文件名、实体、回调阶段与 traceback;先看编辑器日志(标准输出)里的这一段。
- 语法检查 / 声明刷新通过 ≠ 运行过:真正验证要 Play 一次,看行为与日志。
