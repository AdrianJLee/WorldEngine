# 游戏 UI(声明式 `.wui`)

游戏内的 HUD、菜单、背包、对话框都走这套:**用文档描述界面,用设计器可视化编辑,用 AI 无障碍操作**。
它和编辑器面板的 WUI 是同一个渲染/无障碍底座,但**游戏 UI 只做呈现**:不进 ECS、不写游戏数据、
不拥有逻辑状态 —— 逻辑变化通过 `Bind:` 单向流进界面,界面把用户操作变成命令事件交给项目脚本。

## 30 秒上手

1. 在项目内容根建 `assets/ui/hud.wui`(最小示例见下)。
2. 运行时:`Runtime` 默认加载内容根 `assets/ui/` 下**字典序第一个** `.wui`;也可以用
   `WLD_UI_DOC=<路径>` 显式指定(绝对路径,或相对内容根)。
3. 编辑器里开 **UI Designer** 面板(项目 ▸ 面板 ▸ UI Designer):路径框填 `.wui` → **Open**;
   画布会按**真实控件外观**画出这份文档(不是线框),选中节点后在右侧改属性,改完 **Save**。
4. AI/自动化:`ui.tree` 读节点树(每行有 `path`/`kind`/`role`/`actions`),`ui.invoke` 点它,
   `ui.type` 输入;`ctrl`/`shift` 可选(`ui.invoke` 的 `ctrl=1` = 按住 Ctrl 点,用于**多选**)。
   注入走的是和真人鼠标/键盘**同一条**输入路径,游戏 UI 真的会收到。

```yaml
FormatVersion: 1
Screen: Hud
Design: { Resolution: [1920, 1080], ScaleMode: MatchWidthOrHeight, Match: 0.5 }
SafeArea: { Left: 0, Top: 0, Right: 0, Bottom: 0 }
Nodes:
  - Id: root
    Type: Panel
    Anchor: { Min: [0, 0], Max: [1, 1], Pivot: [0, 0], Size: [0, 0] }
    Children:
      - Id: hpBar
        Type: Progress
        Props: { value: 0.62, bg: "#20242B", fill: "#E5484D" }
        Anchor: { Min: [0, 0], Max: [1, 0], Pivot: [0, 0], Offset: [32, 32], Size: [-64, 18] }
        Bind: { value: "ecs:Hero/Health/Ratio" }
      - Id: pauseBtn
        Type: Button
        Props: { label: "@hud.pause|暂停" }
        Anchor: { Min: [1, 0], Max: [1, 0], Pivot: [1, 0], Offset: [-32, 32], Size: [120, 44] }
        On: { Click: ui.close }
```

## 文档要点(逐条都有契约)

| 键 | 说明 |
| --- | --- |
| `FormatVersion` | 必须是 `1`;缺/不认 = 拒绝加载 |
| `Screen` | 屏幕名。无障碍节点的 `panel` 归属、日志里的界面名都用它 |
| `Design` | 设计分辨率 + 缩放策略(`ConstantPixelSize` / `MatchWidthOrHeight` / `Expand` / `Shrink`)+ DPI 口径 |
| `SafeArea` | 刘海/圆角/电视过扫描;根节点相对**内容矩形**(已扣安全区)布局;子节点可 `RelativeToSafeArea: true` |
| `Nodes[].Id` | **稳定 Id**。热重载、绑定、`ui.invoke`、`Path` 都按它定位 —— 改名等于换身份 |
| `Anchor` | Unity RectTransform 口径:`Min==Max` = 点锚定(`Size` 即尺寸,`Offset` 是枢轴相对锚点);`Min!=Max` = 拉伸(`Size` 是尺寸增量)。四边贴边靠拉伸 + `Size: [0,0]` |
| `Layout.Kind` | 容器行为只看它:`Absolute` / `Column` / `Row` / `Grid` / `Flex`(与 `Children` 顺序配合) |
| `Props` | 控件属性(每种控件有登记表;`text`/`label`/`bg`/`fontSize`/`padding`/…)。**属性名拼错会报未知属性** |
| `Bind` | 数据源 → 属性,单向只读:`const:` / `ecs:<实体|组件>/<组件>/<字段>` / `script:<路径>.<字段>` / `service:<名字>` |
| `On` | 事件 → 命令(`Click: ui.close`、`Click: inventory.equip`)。命令进 `EventBus`(`UiCommandEvent`),项目脚本/系统订阅它响应 |
| `World` | 世界锚点:`{ Target: "Mesh:Cube", Offset: [...] }` 让节点跟住场景里的实体;目标解析不到 ⇒ 整棵子树隐藏(不画在角落) |

### 本地化

- `Props` 里的文本属性写 `@key`(`@hud.pause`),语言包放 `assets/localization/<语言>/*.json`。
- 写 `@key|兜底文案`:语言包里**没有**这个键时显示兜底文案(星号、`==` 之类的标记都按字面处理);
  两侧都空 = 原样显示(设计器里也能看到兜底文案,所见即所得)。
- 语言用 `WLD_LANG` 或项目设置指定;缺键会在日志里记一条 warning(不再静默)。

### 页面栈 / 模态(商业级导航)

编辑器与运行时共享同一个导航器(四层:`Page` < `Modal` < `Overlay` < `Debug`):

- 层序绘制:先画栈底,后画栈顶;模态浮在页面上;**栈非空时不画"单屏"文档**。
  ⇒ 常驻 HUD 的写法是把它作为**栈底 Page** 压栈,背包/设置压在其上,模态浮在最上。
- `ui.close` = **有模态先关模态,否则出栈**;`ui.back` = 返回。
- C++ 宿主可直接用 `UiHost::Navigator()` 做 `Push/Pop/Replace`;**脚本侧的项目 API 还没导出**
  (见下面的"已知限制")。
- 开发/验证开关(不改项目就有栈可跑):`WLD_UI_PAGE=a.wui;b.wui`、`WLD_UI_MODAL=m.wui`。

### 热重载

改盘上的 `.wui` ⇒ 运行时与编辑器 Play 都会在下一帧重载:**滚动偏移与焦点按 `Id` 迁移**;
页面文档同样判脏、页面栈形状(栈深/顶层/页名)保持不变。失败**保留上一份可用版本**并给出可读原因
(绝不半加载)。节点 `Type` 拼错 = **整份拒绝**(列出前 3 个未登记的 Type 与节点路径)。

## 设计器提示

- 选中节点后,右侧分两块:**Type: <类型> (N)** 是该类型**独有**属性,下面 `Common` 是公共段
  (Anchor / Layout / World / Bind / On)。类型登记了多个状态(Button 有 `default/hover/pressed/...`)时
  会出现**状态选择器**:`bg.hover` 这类"按状态分槽"的属性只在选中对应状态时列出。
- **Ctrl+点击**大纲 = 多选;**同类型多选**时改一个属性会同时写进所有选中节点(撤销一次回退整批);
  混选只改主选中并给出提示。
- 取色器:点颜色行 ⇒ 弹层里有色板/色相/Alpha/hex/RGBA 滑杆/预设色块(全部进无障碍树,可 `n` 驱动)。
- 快捷键:滚轮缩放、中键/空格拖动平移、`F` 复位、方向键微调(Shift ×10)、`Alt` 关吸附、
  `Delete` 删、`Ctrl+D` 复制、`Ctrl+S` 保存、`Ctrl+Z/Y` 撤销重做。

## 已知限制(截至 2026-10-07)

- 页面栈的**脚本侧 API** 未导出(只有 C++ `UiHost::Navigator()` 与开发开关)。
- 页面/模态里的节点**不参与 `Bind:` 求值与世界锚点**(这两条目前只作用于单屏文档)。
- 热重载不迁移页面**内部**运行态(页面自己的滚动/焦点会重放生命周期)。
- 设计器画布**不模拟** hover/pressed 预览(状态选择器只过滤属性行)。
- `.wui` 的类型/属性由登记表决定:加新控件要同时补登记表与绘制(见 `check-ui-components.ps1`)。

## 排查顺序(用户报"UI 不对"时)

1. **有没有加载**:日志 `[ui] game UI loaded from '<路径>' (screen '…', N nodes)`;没有 ⇒ 路径/内容根不对,
   或文档被拒(下一行就是可读原因)。
2. **改属性没变化**:① 运行时是否重载(日志 `[ui] document reloaded:`);② 设计器画布有没有反馈;
   ③ 输入后**不按回车**直接点 Save 是否落盘。
3. **点不到**:`ui.tree` 里有没有该节点(`path`/`interactive`);`visible=false` 的节点按契约不可点。
4. **Type 拼错**:整份文档被拒 + 可读原因(含未登记 Type 的节点路径)。