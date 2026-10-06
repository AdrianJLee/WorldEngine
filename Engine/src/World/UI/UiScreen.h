#pragma once

// 游戏 UI 框架 — 实例树与布局(设计空间)。
//
// `UiScreen` = `.wui` 文档的运行时实例:节点树 + 每个节点的设计空间矩形 + 命中。
// 契约:`contract.ui-runtime`(坐标/锚点/容器/命中一致)。
// 边界:`invariant.ui-presentation-only` —— 运行态住在 UI 子系统内,不进 ECS、不写场景。

#include "World/Core/Export.h"
#include "World/UI/UiDocument.h"
#include "World/UI/UiTypes.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace World::UI
{
	struct UiNodeInstance
	{
		std::string Id;            // = 文档节点的稳定 Id
		std::string Type;          // = 文档节点 Type
		const UiNode* Source = nullptr;   // 指向 UiScreen 自持文档中的节点(生命周期同 UiScreen)
		int Parent = -1;           // 父节点在 Nodes() 中的下标;-1 = 根
		int Depth = 0;
		std::vector<int> Children; // 子节点下标,顺序 = 绘制顺序
		Wui::WuiRect Rect { 0, 0, 0, 0 };  // 设计空间
	};

	class WLD_API UiScreen
	{
	public:
		// 从文档构建实例树。文档会被本对象持有(Source 指针依赖它)。失败时写可读 error。
		bool Build(const UiDocument& doc, std::string* error = nullptr);

		const UiDocument& Document() const { return m_Document; }

		// 按视口计算全部节点矩形(设计空间)。返回 false = 视口非法。
		bool Layout(const UiViewport& viewport);
		const UiViewport& Viewport() const { return m_Viewport; }

		std::size_t Count() const { return m_Nodes.size(); }
		const std::vector<UiNodeInstance>& Nodes() const { return m_Nodes; }

		int IndexOf(std::string_view id) const;
		const UiNodeInstance* Find(std::string_view id) const;
		Wui::WuiRect RectOf(std::string_view id) const;

		// 世界空间锚定(M8)用:`ApplyWorldAnchors` 覆盖个别节点的矩形。
		// 这是**表现态**的局部改写,不改变文档、不影响其它节点。
		void SetNodeRect(std::size_t index, const Wui::WuiRect& rect);

		// 命中:物理坐标 → 最上层命中节点(后序 = 绘制顺序最后者胜);未命中 → nullptr。
		const UiNodeInstance* HitTest(glm::vec2 physicalPoint) const;

	private:
		void PlaceNode(int index, const Wui::WuiRect& rect);

		UiDocument m_Document;
		std::vector<UiNodeInstance> m_Nodes;
		UiViewport m_Viewport;
	};
}
