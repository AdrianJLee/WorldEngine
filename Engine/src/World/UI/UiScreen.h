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
#include <unordered_map>
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
		// M10:同时尊重滚动容器的裁剪链 —— 点落在某个滚动祖先的矩形外时,该祖先的后代
		// 一律不命中(与 `UiPainter` 压的裁剪矩形同一口径,"看到的 = 点到的")。
		const UiNodeInstance* HitTest(glm::vec2 physicalPoint) const;

		// ---- 滚动(工作包 M10;仅追加:偏移表 + 查询,不改既有签名语义)----
		//
		// 判定口径与 `UiInputRouter::IsScrollContainer` 完全一致(同一份规则,见
		// `UiTypes.h::IsUiScrollContainerProps`);滚动偏移是**运行态**,住在下表的
		// "稳定 Id → 设计空间偏移"里(不进 `.wui` 文档、不进 ECS),`Build` 会清空它。
		bool IsScrollContainer(std::string_view id) const;
		bool IsScrollContainer(const UiNodeInstance& node) const;

		// 当前滚动偏移(设计空间;非滚动容器或未设置 = (0,0))。已按内容/容器尺寸钳位。
		glm::vec2 ScrollOffset(std::string_view id) const;
		// 设置滚动偏移(设计空间);非滚动容器 = 忽略。内容不溢出 ⇒ 钳到 0。
		// 立即作用到后代矩形(保持绘制与命中同一映射);通常在 `Layout` 之后调用。
		void SetScrollOffset(std::string_view id, glm::vec2 offset);

		// 内容尺寸(设计空间):优先显式值,其次类型自报(`UiNodeTypeDesc::ContentSize`),
		// 否则用直接子节点的包围盒。`scrollable` 但内容不溢出时 maxOffset 为 0。
		void SetScrollContentSize(std::string_view id, glm::vec2 size);
		glm::vec2 ScrollContentSize(std::string_view id) const;
		// 该轴最大可滚动距离 = max(0, 内容 − 容器);轴 0 = X,1 = Y。
		float MaxScrollOffset(std::string_view id, int axis) const;

		// 把 target(必须是指定滚动容器的后代)滚进容器可视区;无改动返回 false。
		bool EnsureScrollVisible(std::string_view containerId, std::string_view targetId);

	private:
		void PlaceNode(int index, const Wui::WuiRect& rect);
		// 把每个滚动容器的当前偏移作用到其后代矩形(增量式,重复调用幂等)。
		void ApplyScrollOffsets();
		// 把 delta 加到 index 的全部后代(不含 index 自身)的矩形上。
		void TranslateDescendants(std::size_t index, glm::vec2 delta);
		// index 的可见性裁剪链:点在每个滚动祖先内才算命中。
		bool PointInsideScrollAncestors(int index, glm::vec2 designPoint) const;
		bool IsDescendantOf(int index, int ancestor) const;
		bool IsScrollContainerIndex(std::size_t index) const;

		UiDocument m_Document;
		std::vector<UiNodeInstance> m_Nodes;
		UiViewport m_Viewport;
		std::unordered_map<std::string, glm::vec2> m_ScrollOffsets;        // 稳定 Id → 请求偏移(设计空间)
		std::unordered_map<std::string, glm::vec2> m_ScrollContentSizes;  // 稳定 Id → 显式内容尺寸
		std::vector<glm::vec2> m_AppliedScrollOffsets;                    // 每节点当前已应用的位移(增量基准)
	};
}
