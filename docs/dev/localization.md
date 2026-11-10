# 本地化(语言包结构 / 加载 / 工具链)

面向改引擎的人;做游戏的人只需要 `docs/user/**` 里的用法说明。设计记录见
`tools/agents/tasks/20260924-1200-localization-layers/plan.md`(L2 本地过程层)。

## 1. 原则

- **英文 = 源码内联文案**,永远是默认与回退:`Wui::Tr("menu.file", "File")`。
  目录文件只提供**其它语言的覆盖**;缺翻译永远显示可读英文,不出现裸 key。
- 语言包是**数据**,归属由文件自己声明,不靠目录名猜。

## 2. 目录结构(层 = 模块,目录 = 域,文件 = 域内件)

```
Engine/assets/localization/<lang>/schema/schema.json      # 层 0:引擎自带件(随库发运)
Engine/assets/localization/<lang>/wui/wui.json
Editor/assets/localization/<lang>/shell/*.json            # 层 1:编辑器 UI
Editor/assets/localization/<lang>/panels/<面板>.json
projects/<名>/assets/localization/<lang>/**               # 层 2:项目(只存覆盖 + 项目文案)
```

- 文件头四个元数据键:`$format`(`wld-localization/1`)、`$layer`、`$language`、`$owns`。
- `$owns` 声明本文件拥有的键**前缀**(有尾点 = 前缀;无尾点 = 精确键,如面板标题)。
  精确键与子前缀互不相交(`panel.hierarchy` vs `panel.hierarchy.` 不冲突)。
- 新增面板/域 = 放一个文件(或一个子目录)进对应层,不改任何中心表。

## 3. 条目

```json
"menu.file": "文件",
"panel.save.status.loaded_components": {
  "text": "已应用 {count} 个组件",
  "context": "Save 面板 · 读取存档后的统计",
  "status": "reviewed",
  "maxLength": 32,
  "plural": { "one": "已应用 {count} 个组件", "other": "已应用 {count} 个组件" }
}
```

- 允许字段:`text`(必需)/`context`/`status`(`draft|reviewed|machine|stale`)/`maxLength`/`plural`。
  未知字段忽略(前向兼容)。
- **占位符**:`{name}`;`{{` `}}` 转义。API:`TrFormat(key, fallback, {{"name", value}})`。
- **复数**:`plural` 按语言类别选择(en: one/other;ru: one/few/many/other;zh/ja/ko: other;
  其它语言回退 other)。API:`TrPlural(key, fallback, count)`。
  **中文也需要 plural 条目**(类别恒为 `other`),这样以后加语言不用改数据。

## 4. 加载与覆盖

- 层按 `priority` 升序解析,高优先级覆盖低优先级;跨层同键是正常覆盖,层内重复才是错误
  (先出现者生效并记入 `LocalizationConflicts()`)。
- 语言回退链:`zh-CN` → `zh` → 内联英文(`en`/空不读盘)。
- 目录**递归**扫描 `<layer>/<lang>/**/*.json`(按相对路径排序);
  `<layer>/<lang>/catalog.json` 存在时该层该语言**只读产物**(见 §6)。
- 编辑器在运行时轮询 `LocalizationFilesChanged()`(文件 mtime/大小),变化即 `ReloadLocalization()`
  —— 改翻译不用重启。

## 5. 工具链(`tools/agents/skills/worldengine-dev/scripts/`)

| 工具 | 作用 |
| --- | --- |
| `audit-localization.py` | 门禁:层内跨文件重复 / `$owns` 归属与重叠 / 结构字段 / 占位符一致 / 缺键(默认硬失败);报告死键、覆盖、术语表计数 |
| `localization-compile.py` | 把分层语言包合并成**单文件产物** `catalog.json`(`--check` 只校验),供打包/CI/Mod 分发 |
| `localization-po.py` | PO 导出/导入(对外翻译交换);export → import 往返**逐字节不变** |

术语表:`<layer>/glossary/<lang>.json`(`[{term, translation, note?, doNotTranslate?}]`),
供审计与 PO 导出参考。示例见 `Editor/assets/localization/glossary/zh-CN.json`。

## 6. 打包

- 编辑器 cook 时按**语言子集**把 engine 层与 project 层拷进发行目录:
  `<publish>/localization/<layer>/<lang>/**`(游戏包不含 editor 层)。
  语言子集来自 `CookOptions.Languages`,CLI 形如 `--languages=zh-CN`(空 = 扫描到的全部)。
- Runtime 从 `<exe 目录>/localization/{engine,project}` 注册两层;目录不存在时退回开发树默认
  (`WLD_PROJECT_DIR/assets/localization`)。
- 需要单文件分发(Mod / 手工包)时,用 `localization-compile.py` 产出 `catalog.json` 放进
  `<lang>/` 即可,加载器会优先读它。
