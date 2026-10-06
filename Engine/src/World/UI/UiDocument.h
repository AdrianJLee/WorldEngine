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
		std::vector<UiNode> Nodes;   // 根节点(有序)

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
