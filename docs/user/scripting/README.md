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
`WE_MODULE_ABI_VERSION`(当前 7)。

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

## C++ 脚本（原生脚本组件）

C++ 脚本编译进 `Game.dll`（模板在 `Game/src/Scripts/`），挂载方式与 Luau 一样：给实体的
`World::CppScriptComponent`选一个已注册脚本名（如 `Game::ExampleScript`）。属性用 `WE_FIELD`
写在脚本类里，由生成器 `schema-compiler` 生成注册与访问器；属性面板、场景存档与 Play 应用
共用同一份 schema（与 Luau 属性同一套行模型）。改动声明后要按下面的命令重跑生成物，
否则 `Game.SchemaDrift` 门禁会红灯。

完整可运行示例见 `Game/src/Scripts/ExampleScript.h`（每条声明都带注释：怎么声明、怎么用、
进不进存档）。最小形态：

```cpp
enum class ExampleMode : int32_t { None = 0, Patrol = 1, Chase = 2 };
WE_ENUM_SCHEMA(Game, ExampleMode, Int32)
    WE_ENUM_VALUE(None); WE_ENUM_VALUE(Patrol); WE_ENUM_VALUE(Chase);
WE_ENUM_END

struct ExampleStats               // 嵌套 struct：字段模型 + 默认值，可被 Object/容器复用
{
    float Health = 5.0f;
    int32_t Count = 1;
    WE_SCHEMA_BODY(Game, ExampleStats, Struct)
        WE_FIELD(Health, Float);
        WE_FIELD(Count, Int32);
    WE_SCHEMA_END
};

class ExampleScript : public ScriptableEntity
{
public:
    virtual void OnCreate() override;    // 生命周期：OnCreate/OnUpdate/OnDestroy
    float Health = 100.0f;
    ExampleMode Mode = ExampleMode::Patrol;
    Ref<Texture2D> Icon;
    ExampleStats Stats {};
    std::vector<float> Scores { 1.5f, 2.5f, 3.5f };
    std::map<std::string, float> Costs { { "gold", 3.0f } };

    WE_SCHEMA_BODY(Game, ExampleScript, Script)
        WE_FIELD(Health, Float, Default(100.0f), Range(0.0f, 1000.0f), Unit("hp"), Step(1.0f), Doc("…"));
        WE_FIELD(Mode, Enum, Of(ExampleMode));
        WE_FIELD(Icon, Asset, Of("Texture2D"));
        WE_FIELD(Stats, Object, Of(ExampleStats));
        WE_FIELD(Scores, Array, Of(Float));
        WE_FIELD(Costs, Map, Of(Float));
    WE_SCHEMA_END
};
```

### 标量属性

| 声明 | C++ 成员 | 面板与存档 |
| --- | --- | --- |
| `WE_FIELD(Health, Float, Default(100.0f), Range(0.0f, 1000.0f), Unit("hp"), Step(1.0f))` | `float Health = 100.0f;` | 带单位/范围的滑条；存档写 `Type: Float` + 改后的 `Value` |
| `WE_FIELD(Enabled, Bool, Default(true))` | `bool Enabled = true;` | 复选框 |
| `WE_FIELD(Label, String)` | `std::string Label;` | 文本行 |
| `WE_FIELD(Mode, Enum, Of(ExampleMode))` | `ExampleMode Mode;` | 下拉；存档写整数 + `TypeName: ExampleMode` |
| `WE_FIELD(Icon, Asset, Of("Texture2D"))` | `Ref<Texture2D> Icon;` | 可搜索资产下拉；存档写逻辑资产路径 |
| `WE_FIELD(GridCell, IVec3)` / `WE_FIELD(PreviewMatrix, Mat4)` | `glm::ivec3` / `glm::mat4` | **只读摘要**：面板一行只读文本，值不进存档，Play 保留成员初值 |

没有 `Default(...)` 的标量显示为"未设（脚本默认）"：Play 时未设字段不覆盖成员初值，
行尾 `↺` 也是回到"未设"。

### 容器属性（vector / map）

容器是**形状**（不是新的 Schema 类型）：写 `Array` / `Map` + `Of(元素类型)`，C++ 成员类型随之确定，
面板复用 Luau 的集合行模型（每元素一行、`+` 增、`-` 删、`↺` 复位）。

| 声明 | C++ 成员 | 面板与存档 |
| --- | --- | --- |
| `WE_FIELD(Scores, Array, Of(Float))` | `std::vector<float>` | 元素按下标成行；存档 `Type: Array` + `ElementType: Float` + `Value: [1, 2.5, 7]` |
| `WE_FIELD(Path, Array, Of(Vec3))` | `std::vector<glm::vec3>` | 每个元素一个向量控件 |
| `WE_FIELD(Squad, Array, Of(ExampleStats))` | `std::vector<ExampleStats>` | 元素是命名 struct：展开子字段逐项编辑 |
| `WE_FIELD(Modes, Array, Of(ExampleMode))` | `std::vector<ExampleMode>` | 每个元素是枚举下拉 |
| `WE_FIELD(Costs, Map, Of(Float))` | `std::map<std::string, float>` | 按键成行，`+` 先输入键名；存档 `Type: Map` + `KeyType: String` + `ValueType: Float` |
| `WE_FIELD(Units, Map, Of(ExampleStats))` | `std::map<std::string, ExampleStats>` | 键 → 命名 struct，展开子字段 |

- **元素类型**：叶子 kind（`Float`/`Vec3`/`String`/`Bool`/…）、枚举（`Of(枚举名)`）、资产（`Of("资产类型")`）、
  或已注册的命名 struct（`Of(MyStruct)`）。
- **Map 的键固定是 `std::string`**；`Array, Of(单个标量/枚举/资产)` 同样支持。
- **嵌套容器**：匿名嵌套（`Array, Of(Array(...))`）不支持 —— 先用 `WE_SCHEMA_BODY(..., Struct)` 定义
  命名 struct，再 `Of(ThatStruct)`。
- **默认形状与存档**：成员初始化就是默认形状；面板里加过/改过的元素才随场景保存，重开场景保持形状与值。
  容器字段不支持 `Default(...)`（成员初值即默认），`↺` 复位 = 回到"未设"（重新按成员初值显示）。
- **Play**：容器值在 Play 时按场景行重建到脚本实例成员上（与 Luau 集合同一条应用路径）。

重跑生成物（在仓库根执行；输入自动发现 = `Game/src/Components/*.h` + `Game/src/Scripts/*.h`，
新增脚本无需改任何清单）：

```powershell
# 只重跑生成物(内容不变时不动 mtime)
cmake --build build/x64-Debug --config Debug --target GameSchema

# 或直接重建 Game:GameSchema 是 Game 的构建依赖,会自动重跑
cmake --build build/x64-Debug --config Debug --target Game
```

生成物与注解不一致时 `Game.SchemaDrift` 会红灯；用
`…\schema-compiler.exe --check`（或直接跑该 ctest）可只比较不改写。重建 `Game.dll` 后在编辑器里
File ▸ Reload C++ Module 生效；新建脚本走 `File ▸ New C++ Script…`（创建后会提示重建）。

> **类型账本**：`Game/src/Generated/Game.manifest` 是双向清单——声明了必须登记，登记了必须存在
> （防止类型被误删）。用编辑器入口创建的脚本会自动补 `struct World::<Name>`、`struct World::<Name>Data`
> 与 `enum World::<Name>Mode`（模板声明的全部类型）；**手写**新脚本时需要自己往 manifest 加对应行，
> 否则 `GameSchema`/`Game.SchemaDrift` 会以可读错误拒绝。

## 调试

- 脚本错误会带上文件名、实体、回调阶段与 traceback;先看编辑器日志(标准输出)里的这一段。
- 语法检查 / 声明刷新通过 ≠ 运行过:真正验证要 Play 一次,看行为与日志。
