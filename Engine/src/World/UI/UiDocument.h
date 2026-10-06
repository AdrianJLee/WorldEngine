#pragma once

// 游戏 UI 框架 — `.wui` 文档模型与读写。
//
// 契约:`contract.ui-document-format`(单一 FormatVersion、稳定 Id、`@key` 本地化、
// 未知类型/属性必须报错、原子写盘、逐字节确定性)。
//
// 注意:本层只做"文档 ⇄ 文本",不做布局、不碰渲染、不依赖 Editor。

#include "World/Core/Export.h"
#include "World/UI/UiTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	inline constexpr int kUiDocumentFormatVersion = 1;
	inline constexpr const char* kUiDocumentExtension = ".wui";

	// 属性值 = 文本编码(与 WuiComponentProperty 的值编码协议一致:
	// 颜色 "#RRGGBB[AA]"、尺寸 "WxH"、布尔 "true"/"false"、枚举/文本直接字面量)。
	// 显示文本只写 `@key`(走 localization-catalog 的三层回退),不写死字符串。
	struct UiProp
	{
		std::string Name;
		std::string Value;
	};

	// `Bind: { Value: "player.health" }` —— 声明"节点属性 ← 数据源"。
	// 源协议见 `contract.ui-runtime` §6;本层只存字符串,不解析、不取值。
	struct UiBindingDecl
	{
		std::string Target;
		std::string Source;
	};

	// `On: { Click: "ui.close" }` —— UI 发出的命令/事件(不是直接改数据)。
	struct UiCommandDecl
	{
		std::string Event;
		std::string Command;
	};

	struct UiNode
	{
		std::string Id;      // 稳定身份:文档内唯一;AI/绑定/测试/动画引用它
		std::string Type;    // 类型名:必须在 UiNodeRegistry 登记
		std::vector<UiProp> Props;
		UiAnchor Anchor;
		UiLayoutSpec Layout;
		UiWorldAnchor World;   // 世界空间锚点(M8);Enabled=false = 纯屏幕空间
		std::vector<UiBindingDecl> Bind;
		std::vector<UiCommandDecl> On;
		std::vector<UiNode> Children;
		// true = 解析时 Id 缺失、由 MakeStableId 生成(保存时写回,保证确定性)。
		bool IdWasGenerated = false;

		const UiProp* FindProp(std::string_view name) const;
	};

	struct UiStyleDef
	{
		std::string Id;
		std::vector<UiProp> Props;
	};

	struct UiParamDecl
	{
		std::string Name;
		std::string Type;
		std::string Default;
	};

	struct UiDocument
	{
		int FormatVersion = kUiDocumentFormatVersion;
		std::string Screen;
		UiDesign Design;
		UiSafeArea SafeArea;
		std::string Theme;
		std::vector<UiParamDecl> Parameters;
		std::vector<UiStyleDef> Styles;
		// M14:文档级主题令牌(与 `Styles` 同形;同名时 `Tokens` 优先,`Styles` 兼容回退)。
		// 值支持 `$tokenId` 多级引用;解析口径见 `UiTypes.h::ResolveUiTokenValue`。
		std::vector<UiStyleDef> Tokens;
		std::vector<UiNode> Nodes;   // 根节点(有序)

		// M14:换肤 —— 就地覆盖令牌值。命中的 Id 改写为单值令牌(`value`);
		// 未命中的 Id 追加新令牌。宿主可在 Build 前按 `--theme` 调用(本任务不接线宿主)。
		void ApplyTokenOverrides(const std::map<std::string, std::string>& values);

		void ForEachNode(const std::function<void(UiNode&, const std::string& parentPath)>& fn);
		void ForEachNode(const std::function<void(const UiNode&, const std::string& parentPath)>& fn) const;
		const UiNode* FindNode(std::string_view id) const;
		std::size_t NodeCount() const;
	};

	struct UiValidationIssue
	{
		std::string Path;      // 出问题的节点路径(如 "root.hp")
		std::string Message;
	};

	// 文档校验:Id 合法且唯一、Type 非空、属性名非空、Layout 合法、绑定/命令非空。
	// 返回 true = 无 issue。issues 允许为空指针。
	WLD_API bool ValidateUiDocument(const UiDocument& doc, std::vector<UiValidationIssue>* issues);

	// 行为等价比较(用于 round-trip 断言):逐字段比对,不比较 IdWasGenerated。
	WLD_API bool UiDocumentsEquivalent(const UiDocument& a, const UiDocument& b);

	// ---- 主题令牌(M14;仅追加)----
	//
	// 令牌表 = `Tokens`(优先)∪ `Styles`(回退);`$id` 在属性 `propName` 上取值时:
	// ① 精确命中同名属性;② 令牌只声明一个属性时用它作为该令牌的"单值";③ 否则 = 未定义。
	// 返回 nullptr = 未定义/不适用(调用方按"未设置"处理)。指针在 doc 存活期内有效。
	WLD_API const std::string* FindUiToken(const UiDocument& doc, std::string_view tokenId,
		std::string_view propName);
	// 在 doc 的令牌表 + 属性名上下文下解析单个属性值(非令牌值原样返回)。
	WLD_API UiTokenResult ResolveUiToken(const UiDocument& doc, std::string_view propName,
		std::string_view value);
	// 令牌表**结构**校验:空 Id / 重复 Id / 空 Props / 环。
	// 注意:不在 `ValidateUiDocument` 里调用 —— M14 是兼容追加,加载保持宽松;
	// "未定义令牌"是引用期问题,由解析期给可读错误并按"未设置"处理,不影响加载。
	// 返回 true = 无 issue;issues 允许为空指针。
	WLD_API bool ValidateUiTokens(const UiDocument& doc, std::vector<UiValidationIssue>* issues);

	class WLD_API UiDocumentIO
	{
	public:
		// 解析文本;版本不符/结构非法 → false + 可读 error(不猜、不迁移)。
		static bool Parse(std::string_view text, UiDocument& out, std::string* error = nullptr);
		static bool LoadFile(const std::filesystem::path& path, UiDocument& out, std::string* error = nullptr);
		// 确定性序列化:同一模型两次输出逐字节相同(键序稳定)。
		static std::string Serialize(const UiDocument& doc);
		// 原子写盘:.tmp → .bak → rename(与 SceneSerializer 同口径)。
		static bool SaveFile(const std::filesystem::path& path, const UiDocument& doc, std::string* error = nullptr);
	};
}
