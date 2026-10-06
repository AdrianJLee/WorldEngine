#include "wldpch.h"
#include "WUI/Panels/UiDesignerPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Utils/Paths.h"
#include "World/UI/UiNodeRegistry.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <system_error>
#include <utility>

namespace World
{
	namespace
	{
		// ---- M16:工具栏排队 ----
		// 一排 26px 控件;宽度够时只有一排(工具栏高 34),排不下就把 New/Add/Delete/Duplicate
		// 换到第二排(高 62)。换行阈值 = 单排所需最小宽度,由下面的部件宽度算出:
		// 左右边距 12 + 撤销/重做 2×30 + 路径框 120 + Open/Reload/Save/New 4×62 + Add 132
		// + Delete 70 + Duplicate 84 + 9 个 6px 间隙 = 780。
		constexpr float kToolbarRowHeight = 26.0f;
		constexpr float kToolbarPadY = 4.0f;
		constexpr float kToolbarRowGap = 2.0f;
		constexpr float kToolbarHeightSingle = 34.0f;
		constexpr float kToolbarHeightDouble = 62.0f;
		// M19:工具栏又多了 Fit(和即将到来的 Refresh),单排阈值同步上调,否则会挤在一起。
		constexpr float kToolbarSingleRowMinWidth = 860.0f;

		float ToolbarNeededHeight(float panelWidth)
		{
			return panelWidth >= kToolbarSingleRowMinWidth ? kToolbarHeightSingle : kToolbarHeightDouble;
		}

		constexpr float kSplitterWidth = 5.0f;
		constexpr float kColumnPad = 6.0f;
		constexpr float kStatusHeight = 20.0f;
		constexpr float kTreeRowHeight = 22.0f;
		constexpr float kMinColumnWidth = 120.0f;
		constexpr float kMinCanvasWidth = 140.0f;

		// 属性行的"元数据":一行 = 本地化键 + 英文回退 + 目标字段。字段用枚举寻址,
		// 面板按表驱动地生成 Anchor / Layout 的数值行(不手写 16 段重复代码)。
		enum class FloatField
		{
			AnchorMinX, AnchorMinY, AnchorMaxX, AnchorMaxY,
			AnchorPivotX, AnchorPivotY, AnchorOffsetX, AnchorOffsetY,
			AnchorSizeW, AnchorSizeH,
			LayoutGap, LayoutPadL, LayoutPadT, LayoutPadR, LayoutPadB,
		};

		struct FloatRowDef
		{
			const char* Key;
			const char* Label;
			const char* Term;
			FloatField Field;
		};

		const FloatRowDef kAnchorFloatRows[] =
		{
			{ "ui_designer.anchor.min_x", "Min X", "Min.X", FloatField::AnchorMinX },
			{ "ui_designer.anchor.min_y", "Min Y", "Min.Y", FloatField::AnchorMinY },
			{ "ui_designer.anchor.max_x", "Max X", "Max.X", FloatField::AnchorMaxX },
			{ "ui_designer.anchor.max_y", "Max Y", "Max.Y", FloatField::AnchorMaxY },
			{ "ui_designer.anchor.pivot_x", "Pivot X", "Pivot.X", FloatField::AnchorPivotX },
			{ "ui_designer.anchor.pivot_y", "Pivot Y", "Pivot.Y", FloatField::AnchorPivotY },
			{ "ui_designer.anchor.offset_x", "Offset X", "Offset.X", FloatField::AnchorOffsetX },
			{ "ui_designer.anchor.offset_y", "Offset Y", "Offset.Y", FloatField::AnchorOffsetY },
			{ "ui_designer.anchor.size_w", "Size W", "Size.W", FloatField::AnchorSizeW },
			{ "ui_designer.anchor.size_h", "Size H", "Size.H", FloatField::AnchorSizeH },
		};

		const FloatRowDef kLayoutFloatRows[] =
		{
			{ "ui_designer.layout.gap", "Gap", "Gap", FloatField::LayoutGap },
			{ "ui_designer.layout.pad_left", "Padding Left", "Left", FloatField::LayoutPadL },
			{ "ui_designer.layout.pad_top", "Padding Top", "Top", FloatField::LayoutPadT },
			{ "ui_designer.layout.pad_right", "Padding Right", "Right", FloatField::LayoutPadR },
			{ "ui_designer.layout.pad_bottom", "Padding Bottom", "Bottom", FloatField::LayoutPadB },
		};

		const UI::UiLayoutKind kLayoutKinds[] =
		{
			UI::UiLayoutKind::Absolute,
			UI::UiLayoutKind::Column,
			UI::UiLayoutKind::Row,
			UI::UiLayoutKind::Overlay,
			UI::UiLayoutKind::Grid,
			UI::UiLayoutKind::Flex,
		};

		float* FloatFieldPtr(UI::UiNode& node, FloatField field)
		{
			switch (field)
			{
			case FloatField::AnchorMinX: return &node.Anchor.Min.x;
			case FloatField::AnchorMinY: return &node.Anchor.Min.y;
			case FloatField::AnchorMaxX: return &node.Anchor.Max.x;
			case FloatField::AnchorMaxY: return &node.Anchor.Max.y;
			case FloatField::AnchorPivotX: return &node.Anchor.Pivot.x;
			case FloatField::AnchorPivotY: return &node.Anchor.Pivot.y;
			case FloatField::AnchorOffsetX: return &node.Anchor.Offset.x;
			case FloatField::AnchorOffsetY: return &node.Anchor.Offset.y;
			case FloatField::AnchorSizeW: return &node.Anchor.Size.x;
			case FloatField::AnchorSizeH: return &node.Anchor.Size.y;
			case FloatField::LayoutGap: return &node.Layout.Gap;
			case FloatField::LayoutPadL: return &node.Layout.Padding[0];
			case FloatField::LayoutPadT: return &node.Layout.Padding[1];
			case FloatField::LayoutPadR: return &node.Layout.Padding[2];
			case FloatField::LayoutPadB: return &node.Layout.Padding[3];
			}
			return nullptr;
		}

		std::string TrimCopy(const std::string& text)
		{
			std::size_t begin = 0;
			while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
				++begin;
			std::size_t end = text.size();
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
				--end;
			return text.substr(begin, end - begin);
		}

		std::string FormatFloat(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
			return buffer;
		}

		void DashedSegment(Wui::WuiContext& ctx, const glm::vec2& from, const glm::vec2& to,
			const Wui::WuiColor& color, float thickness)
		{
			const glm::vec2 delta = to - from;
			const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			if (!(length > 0.5f))
				return;
			const glm::vec2 direction = delta / length;
			constexpr float dash = 6.0f;
			constexpr float gap = 4.0f;
			for (float offset = 0.0f; offset < length; offset += dash + gap)
			{
				const float end = std::min(offset + dash, length);
				Wui::LineSegment(ctx, from + direction * offset, from + direction * end, color, thickness);
			}
		}

		void DashedRectOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const Wui::WuiColor& color, float thickness)
		{
			if (!(rect.W > 0.0f) || !(rect.H > 0.0f))
				return;
			const glm::vec2 topLeft { rect.X, rect.Y };
			const glm::vec2 topRight { rect.X + rect.W, rect.Y };
			const glm::vec2 bottomRight { rect.X + rect.W, rect.Y + rect.H };
			const glm::vec2 bottomLeft { rect.X, rect.Y + rect.H };
			DashedSegment(ctx, topLeft, topRight, color, thickness);
			DashedSegment(ctx, topRight, bottomRight, color, thickness);
			DashedSegment(ctx, bottomRight, bottomLeft, color, thickness);
			DashedSegment(ctx, bottomLeft, topLeft, color, thickness);
		}

		void MarkerCross(Wui::WuiContext& ctx, const glm::vec2& center, const Wui::WuiColor& color,
			float half = 5.0f)
		{
			Wui::LineSegment(ctx, glm::vec2 { center.x - half, center.y },
				glm::vec2 { center.x + half, center.y }, color, 1.0f);
			Wui::LineSegment(ctx, glm::vec2 { center.x, center.y - half },
				glm::vec2 { center.x, center.y + half }, color, 1.0f);
		}

		// ---- M12:8 个缩放手柄 ----
		// 每个手柄由两条边构成(位掩码);AnchorX/Y = 手柄中心在节点矩形内的归一化位置。
		enum HandleBits
		{
			HandleNone = 0,
			HandleLeft = 1,
			HandleRight = 2,
			HandleTop = 4,
			HandleBottom = 8,
		};

		struct HandleDef
		{
			int Bits;
			float AnchorX;
			float AnchorY;
		};

		constexpr float kHandleSize = 8.0f;

		const HandleDef kHandles[] =
		{
			{ HandleLeft | HandleTop,     0.0f, 0.0f },
			{ HandleRight | HandleTop,    1.0f, 0.0f },
			{ HandleRight | HandleBottom, 1.0f, 1.0f },
			{ HandleLeft | HandleBottom,  0.0f, 1.0f },
			{ HandleLeft,                 0.0f, 0.5f },
			{ HandleRight,                1.0f, 0.5f },
			{ HandleTop,                  0.5f, 0.0f },
			{ HandleBottom,               0.5f, 1.0f },
		};

		// 画布上节点框的物理矩形:零面积节点(点锚 + Size=0)给一个最小可视盒,否则画布上
		// 完全看不见、8 个手柄也没有可抓的位置。
		Wui::WuiRect NodeBoxPhysical(const UI::UiViewport& viewport, const Wui::WuiRect& rect)
		{
			Wui::WuiRect box = viewport.DesignRectToPhysical(rect);
			if (box.W < 3.0f || box.H < 3.0f)
				box = Wui::WuiRect { box.X, box.Y, 6.0f, 6.0f };
			return box;
		}

		Wui::WuiRect HandlePhysicalRect(const Wui::WuiRect& box, const HandleDef& handle)
		{
			const float half = kHandleSize * 0.5f;
			return Wui::WuiRect { box.X + handle.AnchorX * box.W - half,
				box.Y + handle.AnchorY * box.H - half, kHandleSize, kHandleSize };
		}

		// 同父内定位节点(上移/下移用)。返回所属兄弟列表与下标。
		bool LocateSibling(std::vector<UI::UiNode>& nodes, const std::string& id,
			std::vector<UI::UiNode>** outList, std::size_t* outIndex)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					*outList = &nodes;
					*outIndex = index;
					return true;
				}
				if (LocateSibling(nodes[index].Children, id, outList, outIndex))
					return true;
			}
			return false;
		}

		bool LocateSiblingIndex(const std::vector<UI::UiNode>& nodes, const std::string& id,
			std::size_t* outIndex, std::size_t* outCount)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					*outIndex = index;
					*outCount = nodes.size();
					return true;
				}
				if (LocateSiblingIndex(nodes[index].Children, id, outIndex, outCount))
					return true;
			}
			return false;
		}

		UI::UiNode* FindNodeMutable(std::vector<UI::UiNode>& nodes, const std::string& id)
		{
			for (UI::UiNode& node : nodes)
			{
				if (node.Id == id)
					return &node;
				if (UI::UiNode* found = FindNodeMutable(node.Children, id))
					return found;
			}
			return nullptr;
		}

		// ---- M16:节点定位 / 唯一 Id / 深拷贝重发 Id ----

		// 节点定位:所属 Children 列表(根 = &document.Nodes)、下标与**父路径**。
		// 父路径只在这里沿树累积 —— Id 里可以含 '.'(MakeStableId 的产物),
		// 不能靠字符串反推父路径。
		struct NodeLocation
		{
			std::vector<UI::UiNode>* List = nullptr;
			std::size_t Index = 0;
			std::string ParentPath;
		};

		bool LocateNode(std::vector<UI::UiNode>& nodes, const std::string& id,
			const std::string& parentPath, NodeLocation& out)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					out.List = &nodes;
					out.Index = index;
					out.ParentPath = parentPath;
					return true;
				}
				const std::string path = parentPath.empty()
					? nodes[index].Id : (parentPath + "." + nodes[index].Id);
				if (LocateNode(nodes[index].Children, id, path, out))
					return true;
			}
			return false;
		}

		// 该 Id 是否是**根列表**里的节点(只扫第一层;节点 Id 文档内唯一,无需递归)。
		bool LocateRootIndex(const std::vector<UI::UiNode>& nodes, const std::string& id,
			std::size_t* outIndex)
		{
			for (std::size_t index = 0; index < nodes.size(); ++index)
			{
				if (nodes[index].Id == id)
				{
					if (outIndex != nullptr)
						*outIndex = index;
					return true;
				}
			}
			return false;
		}

		// 稳定 Id 生成(MakeStableId 口径);撞上文档已有 Id 或本轮已分配的 Id 时序号 +1 重试。
		std::string MakeUniqueNodeId(const UI::UiDocument& document,
			const std::set<std::string>& reserved, const std::string& parentPath,
			const std::string& type, std::size_t index)
		{
			std::size_t probe = index;
			std::string id = UI::MakeStableId(parentPath, type, probe);
			while (document.FindNode(id) != nullptr || reserved.find(id) != reserved.end())
			{
				++probe;
				id = UI::MakeStableId(parentPath, type, probe);
			}
			return id;
		}

		// 复制子树:整棵子树重新发 Id(唯一,文档内不重复),父路径逐层累积。
		void RegenerateSubtreeIds(const UI::UiDocument& document, std::set<std::string>& reserved,
			UI::UiNode& node, const std::string& parentPath, std::size_t index)
		{
			node.Id = MakeUniqueNodeId(document, reserved, parentPath, node.Type, index);
			node.IdWasGenerated = false;
			reserved.insert(node.Id);
			const std::string path = parentPath.empty() ? node.Id : (parentPath + "." + node.Id);
			for (std::size_t child = 0; child < node.Children.size(); ++child)
				RegenerateSubtreeIds(document, reserved, node.Children[child], path, child);
		}

		// 节点在文档里的路径(空 = 没找到)。
		std::string FindNodePath(const UI::UiDocument& document, const std::string& id)
		{
			std::string path;
			document.ForEachNode([&](const UI::UiNode& node, const std::string& nodePath)
				{
					if (path.empty() && node.Id == id)
						path = nodePath;
				});
			return path;
		}

		// M7a:内容浏览器双击 `.wui` → 本面板的"按路径打开"待办(见头文件 RequestOpenPath)。
		// 进程内单槽(UI 单线程);登记后由面板下一次渲染取走一次。
		std::string& PendingOpenPath()
		{
			static std::string pending;
			return pending;
		}
	}

	// ---- 文档生命周期 ----

	void UiDesignerPanel::RequestOpenPath(const std::string& logicalPath)
	{
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		PendingOpenPath() = normalized;
	}

	void UiDesignerPanel::ConsumeOpenRequest()
	{
		std::string pending = PendingOpenPath();
		if (pending.empty())
			return;
		PendingOpenPath().clear();
		// 逻辑路径按内容根解析(与工具栏 Open 同一条 ResolveInputPath);解析后 LoadFrom
		// 负责读盘/报错/更新状态行。载入失败时同样清掉待办(不每帧重试同一条坏路径)。
		LoadFrom(ResolveInputPath(pending));
	}

	const std::filesystem::path& UiDesignerPanel::ContentRoot()
	{
		if (!m_ContentRootResolved)
		{
			m_ContentRootResolved = true;
			// 内容根 = **编辑器当前打开的项目**的内容根,与引擎其余部分同一口径:
			// `Paths::AssetRoot()` 已经处理了 `--project` / `WLD_PROJECT_DIR` / `projects/<名>`
			// 与"standard 清单的 content_root"四种来源。
			//
			// 为什么不能从 `current_path()` 反查清单(实测踩过):编辑器从仓库根启动 + 用
			// `WLD_PROJECT_DIR` 指定项目时,`current_path()` 是**仓库根**,那里没有清单 ——
			// 于是内容根解析失败,面板把 `assets/ui/untitled.wui` 落到了**引擎仓库**里
			// (用户的 .wui 会被写进 engine 仓库,而不是他的项目)。
			m_ContentRoot = World::Paths::AssetRoot();
			if (m_ContentRoot.empty())
			{
				std::error_code error;
				m_ContentRoot = std::filesystem::current_path(error);
			}
		}
		return m_ContentRoot;
	}

	std::filesystem::path UiDesignerPanel::ResolveInputPath(const std::string& text)
	{
		const std::string trimmed = TrimCopy(text);
		if (trimmed.empty())
			return {};
		std::filesystem::path path(trimmed);
		if (path.is_relative())
		{
			const std::filesystem::path& root = ContentRoot();
			std::error_code error;
			path = (root.empty() ? std::filesystem::current_path(error) : root) / path;
		}
		return path;
	}

	bool UiDesignerPanel::LoadFrom(const std::filesystem::path& path)
	{
		std::error_code error;
		if (!std::filesystem::exists(path, error))
		{
			m_Status = Wui::Tr("panel.ui_designer.load_missing", "File not found: ") + path.string();
			return false;
		}

		UI::UiDocument document;
		std::string parseError;
		if (!UI::UiDocumentIO::LoadFile(path, document, &parseError))
		{
			m_Status = Wui::Tr("panel.ui_designer.load_failed", "Load failed: ") + parseError;
			return false;
		}

		m_Document = std::move(document);
		m_Path = path;
		m_PathBuffer = path.string();
		m_SelectedId.clear();
		m_BufferNodeId.clear();
		// M12:换文档 = 撤销历史作废(跨文档撤销没有意义),进行中的拖动/编辑也一并收口。
		m_Undo.Clear();
		CancelDrag();
		CancelNodeEdit();
		m_Collapsed.clear();
		m_OutlineScroll = 0.0f;
		m_PropertyScroll = 0.0f;
		m_RecentScroll = 0.0f;
		m_HasDocument = true;
		m_ScreenDirty = true;
		m_Dirty = false;
		RebuildScreen();
		m_Status = Wui::Tr("panel.ui_designer.loaded", "Loaded ") + path.filename().string()
			+ " (" + std::to_string(m_Document.NodeCount()) + " nodes)";
		return true;
	}

	bool UiDesignerPanel::SaveTo(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			return false;
		}
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		std::string saveError;
		if (!UI::UiDocumentIO::SaveFile(path, m_Document, &saveError))
		{
			m_Status = Wui::Tr("panel.ui_designer.save_failed", "Save failed: ") + saveError;
			return false;
		}
		m_Path = path;
		m_PathBuffer = path.string();
		m_Dirty = false;
		m_Status = Wui::Tr("panel.ui_designer.saved", "Saved ") + path.filename().string();
		return true;
	}

	void UiDesignerPanel::RebuildScreen()
	{
		m_ScreenDirty = false;
		std::string error;
		if (!m_Screen.Build(m_Document, &error))
			m_Status = Wui::Tr("panel.ui_designer.build_failed", "Cannot build screen: ") + error;
	}

	UI::UiNode* UiDesignerPanel::MutableSelectedNode()
	{
		if (m_SelectedId.empty())
			return nullptr;
		UI::UiNode* found = nullptr;
		m_Document.ForEachNode([&](UI::UiNode& node, const std::string&)
		{
			if (found == nullptr && node.Id == m_SelectedId)
				found = &node;
		});
		return found;
	}

	const UI::UiNode* UiDesignerPanel::SelectedNode() const
	{
		return m_SelectedId.empty() ? nullptr : m_Document.FindNode(m_SelectedId);
	}

	bool UiDesignerPanel::IsCollapsed(const std::string& nodeId) const
	{
		const auto found = m_Collapsed.find(nodeId);
		return found != m_Collapsed.end() && found->second;
	}

	// ---- 三栏渲染 ----

	void UiDesignerPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (!(rect.W > 1.0f) || !(rect.H > 1.0f))
			return;

		// M7a:先消费内容浏览器排队的"按路径打开"(取走后 LoadFrom),再画工具栏/画布 ——
		// 同一帧就能看到新文档,而不是等下一帧。
		ConsumeOpenRequest();

		// M12:面板级快捷键(Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z)。走 ctx 的输入快照 —— 真实键盘与
		// AI 注入(ui.key)是同一条路径;文本焦点活跃时让位给文本编辑(与 EditorShell W9-2 同口径)。
		HandleShortcuts(ctx);

		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.WindowBg, 0.0f);

		// M16:工具栏高度随宽度变化(够宽一排,排不下换两排);画布/状态行按同一结果切分。
		const float toolbarHeight = std::min(ToolbarNeededHeight(rect.W), rect.H);
		const float statusHeight = rect.H > 120.0f ? kStatusHeight : 0.0f;
		RenderToolbar(ctx, Wui::WuiRect { rect.X, rect.Y, rect.W, toolbarHeight }, host);

		const Wui::WuiRect body { rect.X, rect.Y + toolbarHeight, rect.W,
			std::max(0.0f, rect.H - toolbarHeight - statusHeight) };

		const float innerWidth = std::max(0.0f, body.W - kColumnPad * 2.0f - kSplitterWidth * 2.0f);
		if (innerWidth < kMinColumnWidth * 2.0f + kMinCanvasWidth || body.H < 48.0f)
		{
			// 面板太窄/太矮:三栏挤不出可读宽度,退化为只画画布(不画不可读的窄条)。
			RenderCanvas(ctx, body, host);
		}
		else
		{
			const float maxLeft = std::max(kMinColumnWidth, innerWidth - kMinCanvasWidth - kMinColumnWidth);
			const float leftWidth = std::clamp(m_LeftWidth, kMinColumnWidth, maxLeft);
			const float maxRight = std::max(kMinColumnWidth, innerWidth - kMinCanvasWidth - leftWidth);
			const float rightWidth = std::clamp(m_RightWidth, kMinColumnWidth, maxRight);
			const float canvasWidth = std::max(kMinCanvasWidth, innerWidth - leftWidth - rightWidth);

			const float columnTop = body.Y + kColumnPad;
			const float columnHeight = std::max(0.0f, body.H - kColumnPad * 2.0f);
			float x = body.X + kColumnPad;
			const Wui::WuiRect outlineRect { x, columnTop, leftWidth, columnHeight };
			x += leftWidth;
			const Wui::WuiRect leftSplitter { x, columnTop, kSplitterWidth, columnHeight };
			x += kSplitterWidth;
			const Wui::WuiRect canvasRect { x, columnTop, canvasWidth, columnHeight };
			x += canvasWidth;
			const Wui::WuiRect rightSplitter { x, columnTop, kSplitterWidth, columnHeight };
			x += kSplitterWidth;
			const Wui::WuiRect propertiesRect { x, columnTop, rightWidth, columnHeight };

			RenderOutline(ctx, outlineRect, host);
			RenderCanvas(ctx, canvasRect, host);
			RenderProperties(ctx, propertiesRect, host);

			// 分隔条:拖动直接改像素宽度,越界由上面的 clamp 在每个帧收口。
			Wui::Splitter(ctx, Wui::HashId("ui_designer.split.left"), leftSplitter, true,
				m_LeftWidth, kMinColumnWidth, maxLeft, theme);
			Wui::Splitter(ctx, Wui::HashId("ui_designer.split.right"), rightSplitter, true,
				m_RightWidth, kMinColumnWidth, maxRight, theme);
		}

		if (statusHeight > 0.0f)
		{
			const Wui::WuiRect status { rect.X, rect.Y + rect.H - statusHeight, rect.W, statusHeight };
			Wui::PanelBackground(ctx, status, theme.PanelHeader, 0.0f);
			std::string text = m_Status;
			if (text.empty())
			{
				text = m_HasDocument
					? (m_Dirty ? std::string("* ") : std::string()) + m_Path.filename().string()
						+ "  ·  " + std::to_string(m_Document.NodeCount()) + " nodes"
					: Wui::Tr("panel.ui_designer.status_idle", "No document open");
			}
			Wui::Label(ctx, glm::vec2 { status.X + 8.0f, status.Y + 4.0f }, text,
				theme.TextMuted, theme.FontSizeSmall);
		}

		// M12:属性行编辑在鼠标抬起后落一条撤销记录(文本提交时鼠标本就抬起 → 同帧提交)。
		if (m_PendingEditValid && !ctx.Input().MouseDown[0])
			CommitNodeEdit();
		// M12:文本焦点快照 —— 下一帧的 Ctrl+Z/Y 判定要用"上一帧结束时"的状态(W9-2 同口径)。
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}

	void UiDesignerPanel::RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelHeader, 0.0f);

		constexpr float iconWidth = 30.0f;
		constexpr float buttonWidth = 62.0f;
		constexpr float addWidth = 132.0f;
		constexpr float deleteWidth = 70.0f;
		constexpr float duplicateWidth = 84.0f;
		constexpr float gap = 6.0f;

		const float height = kToolbarRowHeight;
		const float left = rect.X + 6.0f;
		const float right = rect.X + rect.W - 6.0f;
		const float row1Y = rect.Y + kToolbarPadY;
		// M16:排不下就把 New/Add/Delete/Duplicate 换到第二排(工具栏高度由
		// ToolbarNeededHeight 按同一阈值先算好,两边不会错位)。
		const bool twoRows = rect.W < kToolbarSingleRowMinWidth;
		const float row2Y = row1Y + (twoRows ? height + kToolbarRowGap : 0.0f);

		float x = left;

		// M12:撤销/重做(与 Ctrl+Z / Ctrl+Y 同一条面板本地栈)。
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.undo"), Wui::WuiRect { x, row1Y, iconWidth, height },
			Wui::Tr("panel.ui_designer.undo", "Undo"), theme, m_Undo.CanUndo(), false,
			Wui::Tr("panel.ui_designer.undo.tip", "Undo the last designer edit (Ctrl+Z)")))
		{
			UndoDocument();
		}
		x += iconWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.redo"), Wui::WuiRect { x, row1Y, iconWidth, height },
			Wui::Tr("panel.ui_designer.redo", "Redo"), theme, m_Undo.CanRedo(), false,
			Wui::Tr("panel.ui_designer.redo.tip", "Redo the last undone edit (Ctrl+Y)")))
		{
			RedoDocument();
		}
		x += iconWidth + gap;

		// 行 1 尾部固定项:Open / Reload / Save / Fit(+ 单排时的 New / Add / Delete / Duplicate)。
		float tailWidth = gap + buttonWidth * 4.0f + gap * 3.0f;
		if (!twoRows)
			tailWidth += gap + buttonWidth + gap + addWidth + gap + deleteWidth + gap + duplicateWidth;
		// 路径框拿走剩余宽度;面板极窄时钳到 80(后面的部件被画到边界外 = 截断,不互相叠)。
		const float fieldWidth = std::max(80.0f, right - x - tailWidth);

		Wui::TextFieldA11y pathA11y;
		pathA11y.Label = Wui::Tr("panel.ui_designer.path", "WUI file path");
		pathA11y.Placeholder = "assets/ui/hud.wui";
		Wui::TextField(ctx, Wui::HashId("ui_designer.path.field"),
			Wui::WuiRect { x, row1Y, fieldWidth, height }, m_PathBuffer, theme, nullptr, &pathA11y);
		x += fieldWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.open"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.open", "Open"), theme, true, false,
			Wui::Tr("panel.ui_designer.open.tip", "Load the .wui document at this path")))
		{
			const std::filesystem::path path = ResolveInputPath(m_PathBuffer);
			if (path.empty())
				m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			else
				LoadFrom(path);
		}
		x += buttonWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.reload"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.reload", "Reload"), theme, true, false,
			Wui::Tr("panel.ui_designer.reload.tip", "Discard in-memory edits and re-read the file from disk")))
		{
			if (m_Path.empty())
				m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			else
				LoadFrom(m_Path);
		}
		x += buttonWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.save"), Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.save", "Save"), theme, m_HasDocument, true,
			Wui::Tr("panel.ui_designer.save.tip", "Write the document back to disk (atomic)")))
		{
			const std::filesystem::path path = m_Path.empty() ? ResolveInputPath(m_PathBuffer) : m_Path;
			SaveTo(path);
		}
		x += buttonWidth + gap;

		// M19:画布视图复位(F 键同路径)—— 放大看细节后一键回到"全貌适配"。
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.fit"),
			Wui::WuiRect { x, row1Y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.fit", "Fit"), theme, m_HasDocument, false,
			Wui::Tr("panel.ui_designer.fit.tip",
				"Fit the whole design into the canvas (F); wheel zooms, middle/space drag pans")))
		{
			m_CanvasZoom = 1.0f;
			m_CanvasPan = glm::vec2 { 0.0f, 0.0f };
			m_Status = Wui::Tr("panel.ui_designer.view_reset", "Canvas view reset");
		}
		x += buttonWidth + gap;

		// ---- M16:New / Add / Delete / Duplicate(单排接在 Save 后;排不下换到第二排)----
		float editX = twoRows ? left : x;
		const float editY = twoRows ? row2Y : row1Y;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.new"),
			Wui::WuiRect { editX, editY, buttonWidth, height },
			Wui::Tr("panel.ui_designer.new", "New"), theme, true, false,
			Wui::Tr("panel.ui_designer.new.tip",
				"Create a minimal document (Panel + Label); uses the path box, else <content-root>/ui/untitled.wui")))
		{
			CreateNewDocument();
		}
		editX += buttonWidth + gap;

		// 类型来自 UI::UiNodeRegistry::All()(唯一事实源:新登记的类型自动出现在下拉里)。
		std::vector<std::string> typeOptions;
		for (const UI::UiNodeTypeDesc& desc : UI::UiNodeRegistry::All())
			typeOptions.push_back(desc.Type);
		if (!typeOptions.empty())
		{
			if (m_AddTypeIndex < 0 || m_AddTypeIndex >= static_cast<int>(typeOptions.size()))
				m_AddTypeIndex = 0;
			int picked = m_AddTypeIndex;
			const std::string addLabel = Wui::Tr("panel.ui_designer.add", "Add");
			// M19:触发器固定显示 "Add"(M19a 给 Combo 加的 displayText 尾参)——
			// 否则它只会画上次选的类型名,用户看不出这是个"添加节点"入口。
			if (Wui::Combo(ctx, Wui::HashId("ui_designer.add"),
				Wui::WuiRect { editX, editY, addWidth, height }, addLabel, typeOptions, picked, theme,
				addLabel))
			{
				m_AddTypeIndex = std::clamp(picked, 0, static_cast<int>(typeOptions.size()) - 1);
				AddNode(typeOptions[static_cast<std::size_t>(m_AddTypeIndex)]);
			}
		}
		editX += addWidth + gap;

		const bool canDelete = CanDeleteSelected();
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.delete"),
			Wui::WuiRect { editX, editY, deleteWidth, height },
			Wui::Tr("panel.ui_designer.delete", "Delete"), theme, canDelete, false,
			canDelete
				? Wui::Tr("panel.ui_designer.delete.tip", "Remove the selected node (and its children)")
				: Wui::Tr("panel.ui_designer.delete.disabled",
					"Select a node first; the last root node cannot be deleted")))
		{
			DeleteSelectedNode();
		}
		editX += deleteWidth + gap;

		const bool canDuplicate = CanDuplicateSelected();
		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.duplicate"),
			Wui::WuiRect { editX, editY, duplicateWidth, height },
			Wui::Tr("panel.ui_designer.duplicate", "Duplicate"), theme, canDuplicate, false,
			canDuplicate
				? Wui::Tr("panel.ui_designer.duplicate.tip", "Deep-copy the selected subtree and select the copy")
				: Wui::Tr("panel.ui_designer.duplicate.disabled", "Select a node first")))
		{
			DuplicateSelectedNode();
		}
	}

	void UiDesignerPanel::RenderOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);
		Wui::Label(ctx, glm::vec2 { rect.X + 6.0f, rect.Y + 4.0f },
			Wui::Tr("panel.ui_designer.outline", "Outline"), theme.TextMuted, theme.FontSizeCaption);

		// M12:同父内上移/下移(改 Children 顺序 = 改绘制顺序;保存后文档顺序真的变了)。
		{
			constexpr float headerHeight = 16.0f;
			constexpr float moveWidth = 24.0f;
			const float headerY = rect.Y + 1.0f;
			float buttonX = rect.X + rect.W - 4.0f - moveWidth;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.down"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x93", theme, CanMoveSelected(1), false,
				Wui::Tr("panel.ui_designer.outline.down.tip", "Move node down (later in draw order)")))
			{
				MoveSelectedNode(1);
			}
			buttonX -= moveWidth + 3.0f;
			if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.outline.up"),
				Wui::WuiRect { buttonX, headerY, moveWidth, headerHeight },
				"\xE2\x86\x91", theme, CanMoveSelected(-1), false,
				Wui::Tr("panel.ui_designer.outline.up.tip", "Move node up (earlier in draw order)")))
			{
				MoveSelectedNode(-1);
			}
		}

		const Wui::WuiRect area { rect.X + 2.0f, rect.Y + 18.0f,
			std::max(0.0f, rect.W - 4.0f), std::max(0.0f, rect.H - 20.0f) };

		m_OutlineIds.clear();
		std::vector<Wui::TreeViewItem> items;
		if (m_HasDocument)
			AppendOutlineItems(m_Document.Nodes, 0, items);

		const Wui::TreeViewResult result = Wui::TreeView(ctx, area, items, kTreeRowHeight,
			m_OutlineScroll, theme, Wui::HashId("ui_designer.outline.tree"));

		const std::string selectionBefore = m_SelectedId;
		const auto nodeIdAt = [&](int index) -> const std::string*
		{
			if (index < 0 || static_cast<std::size_t>(index) >= m_OutlineIds.size())
				return nullptr;
			return &m_OutlineIds[static_cast<std::size_t>(index)];
		};
		if (const std::string* id = nodeIdAt(result.ClickedArrow))
			m_Collapsed[*id] = !IsCollapsed(*id);
		if (const std::string* id = nodeIdAt(result.KeyToggleExpand))
			m_Collapsed[*id] = !IsCollapsed(*id);
		if (const std::string* id = nodeIdAt(result.Clicked))
			m_SelectedId = *id;
		if (const std::string* id = nodeIdAt(result.KeyActivate))
			m_SelectedId = *id;
		if (const std::string* id = nodeIdAt(result.KeyMoveTo))
			m_SelectedId = *id;
		if (m_SelectedId != selectionBefore)
			CommitNodeEdit();   // 选中变化前把上一节点的属性编辑落账
	}

	void UiDesignerPanel::AppendOutlineItems(const std::vector<UI::UiNode>& nodes, int depth,
		std::vector<Wui::TreeViewItem>& out)
	{
		for (const UI::UiNode& node : nodes)
		{
			const bool hasChildren = !node.Children.empty();
			const bool collapsed = IsCollapsed(node.Id);
			Wui::TreeViewItem item;
			item.Id = Wui::HashId(("ui_designer.tree." + node.Id).c_str());
			item.Label = node.Type + "  " + node.Id;
			item.Depth = depth;
			item.HasChildren = hasChildren;
			item.Expanded = hasChildren && !collapsed;
			item.Selected = (node.Id == m_SelectedId);
			out.push_back(item);
			m_OutlineIds.push_back(node.Id);
			if (hasChildren && !collapsed)
				AppendOutlineItems(node.Children, depth + 1, out);
		}
	}

	float UiDesignerPanel::PropertyContentHeight(const UI::UiNode* node) const
	{
		const float rowHeight = Wui::PropertyRowHeight();
		const float propertyRows = static_cast<float>(node != nullptr && node->Props.empty() == false
			? node->Props.size() : 1);
		float rows = 4.0f;                                  // 四个分组头
		if (m_ShowNodeSection) rows += 2.0f;                // Id / Type
		if (m_ShowPropsSection) rows += propertyRows;       // 每条属性一行
		if (m_ShowAnchorSection) rows += 11.0f;             // 10 个数值 + RelativeToSafeArea
		if (m_ShowLayoutSection) rows += 8.0f;              // Kind + Gap + 4×Padding + Columns + RowMajor
		return rowHeight * rows + 8.0f;
	}

	void UiDesignerPanel::RenderProperties(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);

		UI::UiNode* node = MutableSelectedNode();
		// 选中节点变化(含重新加载):清掉上一份属性文本编辑缓冲,避免 TextField 回显旧值。
		if (node == nullptr)
		{
			m_BufferNodeId.clear();
		}
		else if (node->Id != m_BufferNodeId)
		{
			m_BufferNodeId = node->Id;
			for (const UI::UiProp& prop : node->Props)
			{
				ctx.ErasePersist(Wui::HashId(
					("ui_designer.prop." + node->Id + "." + prop.Name + ".buf").c_str()));
			}
		}

		// M12:没有待提交编辑时,每帧抓一份"本帧起点"节点快照 —— 属性行首次改动时拿它当
		// 撤销前像。拖动期间只在第一帧抓一次,所以"一次拖动 = 一条记录"。
		if (node != nullptr && !m_PendingEditValid)
			m_FrameNodeBefore = *node;

		const float rowHeight = Wui::PropertyRowHeight();
		const float innerWidth = std::max(0.0f, rect.W - 12.0f);
		const Wui::WuiRect inner { rect.X + 6.0f, rect.Y + 6.0f, innerWidth, std::max(0.0f, rect.H - 12.0f) };
		const float labelWidth = Wui::PropertyRowLabelWidth(Wui::WuiRect { 0.0f, 0.0f, innerWidth, rowHeight });

		Wui::BeginScrollArea(ctx, inner, PropertyContentHeight(node), m_PropertyScroll, theme,
			Wui::HashId("ui_designer.props.scroll"));

		float y = inner.Y - m_PropertyScroll;
		const auto nextRow = [&](float height) -> Wui::WuiRect
		{
			const Wui::WuiRect row { inner.X, y, inner.W, height };
			y += height;
			return row;
		};
		const auto groupHeader = [&](const char* key, const char* fallback, const std::string& trailing, bool& open)
		{
			Wui::PropertyGroupHeaderDesc desc;
			desc.Label = Wui::Tr(key, fallback);
			desc.A11yLabel = desc.Label;
			desc.Trailing = trailing;
			desc.Open = open;
			const Wui::PropertyGroupHeaderResult result =
				Wui::PropertyGroupHeader(ctx, Wui::HashId(key), nextRow(rowHeight), desc, theme);
			if (result.Toggled)
				open = !open;
		};
		const auto floatRow = [&](const FloatRowDef& def, UI::UiNode& target)
		{
			float* value = FloatFieldPtr(target, def.Field);
			if (value == nullptr)
				return;
			Wui::PropertyRowDesc desc;
			desc.Label = Wui::Tr(def.Key, def.Label);
			desc.Term = def.Term;
			desc.A11yLabel = desc.Label;
			desc.A11yValue = FormatFloat(*value);
			desc.LabelWidth = labelWidth;
			const Wui::PropertyRowResult result =
				Wui::PropertyRow(ctx, Wui::HashId(def.Key), nextRow(rowHeight), desc, theme);
			if (Wui::DragFloat(ctx, Wui::HashId((std::string(def.Key) + ".field").c_str()),
				result.FieldRect, *value, 0.25f, -1000000.0f, 1000000.0f, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(def.Label);
			}
		};

		if (node == nullptr)
		{
			Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
				Wui::Tr("panel.ui_designer.no_selection", "Select a node in the outline to edit it."),
				theme.TextMuted, theme.FontSizeSmall);
			Wui::EndScrollArea(ctx);
			return;
		}

		// ---- Node ----
		groupHeader("ui_designer.section.node", "Node", std::string(), m_ShowNodeSection);
		if (m_ShowNodeSection)
		{
			const auto inlineRow = [&](const char* key, const char* fallback, const std::string& value)
			{
				Wui::PropertyRowDesc desc;
				desc.Label = Wui::Tr(key, fallback);
				desc.A11yLabel = desc.Label;
				desc.InlineValue = true;
				desc.InlineValueText = value;
				desc.LabelWidth = labelWidth;
				Wui::PropertyRow(ctx, Wui::HashId(key), nextRow(rowHeight), desc, theme);
			};
			inlineRow("ui_designer.node.id", "Id", node->Id);
			inlineRow("ui_designer.node.type", "Type", node->Type);
		}

		// ---- Props(每条属性一行;值文本回车提交,失焦提交)----
		groupHeader("ui_designer.section.props", "Props",
			"(" + std::to_string(node->Props.size()) + ")", m_ShowPropsSection);
		if (m_ShowPropsSection)
		{
			if (node->Props.empty())
			{
				Wui::Label(ctx, glm::vec2 { inner.X + 2.0f, y + 2.0f },
					Wui::Tr("panel.ui_designer.no_props", "This node has no properties."),
					theme.TextMuted, theme.FontSizeSmall);
				y += rowHeight;
			}
			for (UI::UiProp& prop : node->Props)
			{
				const std::string base = "ui_designer.prop." + node->Id + "." + prop.Name;
				Wui::PropertyRowDesc desc;
				desc.Label = prop.Name;
				desc.A11yLabel = prop.Name;
				desc.A11yEnabled = false;
				desc.LabelWidth = labelWidth;
				const Wui::PropertyRowResult rowResult = Wui::PropertyRow(ctx,
					Wui::HashId((base + ".row").c_str()), nextRow(rowHeight), desc, theme);

				std::string& buffer = ctx.Persist<std::string>(Wui::HashId((base + ".buf").c_str()), prop.Value);
				Wui::TextFieldA11y a11y;
				a11y.Label = prop.Name;
				const Wui::WuiId fieldId = Wui::HashId((base + ".field").c_str());
				bool cancelled = false;
				const bool committed = Wui::TextField(ctx, fieldId, rowResult.FieldRect, buffer, theme, &cancelled, &a11y);
				if (cancelled)
				{
					buffer = prop.Value;
				}
				else if (committed || (ctx.Focus() != fieldId && buffer != prop.Value))
				{
					if (prop.Value != buffer)
					{
						prop.Value = buffer;
						m_ScreenDirty = true;
						NoteNodeEdit(prop.Name);
					}
				}
			}
		}

		// ---- Anchor ----
		groupHeader("ui_designer.section.anchor", "Anchor", std::string(), m_ShowAnchorSection);
		if (m_ShowAnchorSection)
		{
			for (const FloatRowDef& def : kAnchorFloatRows)
				floatRow(def, *node);

			Wui::PropertyRowDesc desc;
			desc.Label = Wui::Tr("ui_designer.anchor.relative_safe", "Relative To Safe Area");
			desc.Term = "RelativeToSafeArea";
			desc.A11yLabel = desc.Label;
			desc.LabelWidth = labelWidth;
			const Wui::PropertyRowResult result = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.anchor.relative_safe"), nextRow(rowHeight), desc, theme);
			if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.anchor.relative_safe.field"), result.FieldRect,
				std::string(), node->Anchor.RelativeToSafeArea, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(desc.Label);
			}
		}

		// ---- Layout ----
		groupHeader("ui_designer.section.layout", "Layout", std::string(), m_ShowLayoutSection);
		if (m_ShowLayoutSection)
		{
			std::vector<std::string> options;
			options.reserve(sizeof(kLayoutKinds) / sizeof(kLayoutKinds[0]));
			for (const UI::UiLayoutKind kind : kLayoutKinds)
				options.push_back(UI::UiLayoutKindName(kind));

			int selected = 0;
			for (std::size_t i = 0; i < options.size(); ++i)
			{
				if (kLayoutKinds[i] == node->Layout.Kind)
				{
					selected = static_cast<int>(i);
					break;
				}
			}

			Wui::PropertyRowDesc kindDesc;
			kindDesc.Label = Wui::Tr("ui_designer.layout.kind", "Kind");
			kindDesc.Term = "Kind";
			kindDesc.A11yLabel = kindDesc.Label;
			kindDesc.A11yValue = options.empty() ? std::string() : options[static_cast<std::size_t>(selected)];
			kindDesc.LabelWidth = labelWidth;
			const Wui::PropertyRowResult kindRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.kind.row"), nextRow(rowHeight), kindDesc, theme);
			if (Wui::Combo(ctx, Wui::HashId("ui_designer.layout.kind.field"), kindRow.FieldRect,
				kindDesc.Label, options, selected, theme))
			{
				node->Layout.Kind = kLayoutKinds[static_cast<std::size_t>(selected)];
				m_ScreenDirty = true;
				NoteNodeEdit(kindDesc.Label);
			}

			for (const FloatRowDef& def : kLayoutFloatRows)
				floatRow(def, *node);

			Wui::PropertyRowDesc columnsDesc;
			columnsDesc.Label = Wui::Tr("ui_designer.layout.columns", "Columns");
			columnsDesc.Term = "Columns";
			columnsDesc.A11yLabel = columnsDesc.Label;
			columnsDesc.A11yValue = std::to_string(node->Layout.Columns);
			columnsDesc.LabelWidth = labelWidth;
			const Wui::PropertyRowResult columnsRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.columns.row"), nextRow(rowHeight), columnsDesc, theme);
			int64_t columns = node->Layout.Columns;
			if (Wui::DragInt(ctx, Wui::HashId("ui_designer.layout.columns.field"), columnsRow.FieldRect,
				columns, 1, 64, theme))
			{
				node->Layout.Columns = static_cast<int>(columns);
				m_ScreenDirty = true;
				NoteNodeEdit(columnsDesc.Label);
			}

			Wui::PropertyRowDesc rowMajorDesc;
			rowMajorDesc.Label = Wui::Tr("ui_designer.layout.row_major", "Row Major");
			rowMajorDesc.Term = "RowMajor";
			rowMajorDesc.A11yLabel = rowMajorDesc.Label;
			rowMajorDesc.LabelWidth = labelWidth;
			const Wui::PropertyRowResult rowMajorRow = Wui::PropertyRow(ctx,
				Wui::HashId("ui_designer.layout.row_major.row"), nextRow(rowHeight), rowMajorDesc, theme);
			if (Wui::Checkbox(ctx, Wui::HashId("ui_designer.layout.row_major.field"), rowMajorRow.FieldRect,
				std::string(), node->Layout.RowMajor, theme))
			{
				m_ScreenDirty = true;
				NoteNodeEdit(rowMajorDesc.Label);
			}
		}

		Wui::EndScrollArea(ctx);
	}

	// ---- 画布 ----

	void UiDesignerPanel::RenderCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.WindowBg, 2.0f);

		if (!m_HasDocument)
		{
			// M16:空态不再是"去工具栏打字"这一条死路 —— 直接列出内容根里扫到的 .wui
			// (每条一个按钮,点一条即 LoadFrom);一条都没有就提示用工具栏 New 新建。
			RenderDocumentBrowser(ctx, rect, host);
			return;
		}

		if (m_ScreenDirty)
			RebuildScreen();

		LayoutCanvas(rect);
		HandleCanvasInput(ctx, rect);
		{
			Wui::ClipScope clip(ctx, rect);
			DrawCanvas(ctx, rect, host);
		}
	}

	void UiDesignerPanel::LayoutCanvas(const Wui::WuiRect& rect)
	{
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
		const float fit = CanvasFitScale(rect);

		// 用"物理面 = 设计分辨率"(比例 = 1)调 ComputeUiViewport:安全区内缩在设计单位下
		// 换算逐字段正确(ContentRect = 设计矩形扣安全区);再把 Scale 换成画布适配比、
		// 把原点挪到画布中心 —— 设计器是设计视图,只做等比适配、不裁切。
		UI::UiViewport viewport = UI::ComputeUiViewport(m_Document.Design, m_Document.SafeArea,
			UI::UiSurface { glm::vec2 { designWidth, designHeight }, 1.0f });
		// M19:适配比 × 用户缩放(滚轮),再加平移量 —— 大设计稿也能放大看细节。
		// 视图中心仍然对齐画布中心,所以平移是"相对适配位置"的像素偏移,与缩放无关。
		viewport.Scale = std::max(0.02f, fit * m_CanvasZoom);
		viewport.PhysicalSize = glm::vec2 { rect.W, rect.H };
		viewport.PhysicalOrigin = glm::vec2 {
			rect.X + (rect.W - designWidth * viewport.Scale) * 0.5f + m_CanvasPan.x,
			rect.Y + (rect.H - designHeight * viewport.Scale) * 0.5f + m_CanvasPan.y };
		m_Viewport = viewport;
		m_Screen.Layout(viewport);
	}

	// M19:适配比 = 把整个设计分辨率塞进画布矩形(留 14px 边距)的等比缩放。
	// 单独抽出来,是因为"以光标为中心缩放"要按**同一个**适配比反推平移量。
	float UiDesignerPanel::CanvasFitScale(const Wui::WuiRect& rect) const
	{
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
		constexpr float pad = 14.0f;
		const float fitWidth = std::max(1.0f, rect.W - pad * 2.0f) / designWidth;
		const float fitHeight = std::max(1.0f, rect.H - pad * 2.0f) / designHeight;
		return std::max(0.01f, std::min(fitWidth, fitHeight));
	}

	void UiDesignerPanel::HandleCanvasInput(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const Wui::WuiInputState& input = ctx.Input();

		// ---- M19:画布视图操作(缩放 / 平移)----
		// 滚轮落在画布上 = 以光标为中心缩放(与主流编辑器一致:放大时光标下的那个点不动)。
		if (ctx.IsHovered(rect) && input.Wheel != 0.0f)
		{
			const glm::vec2 anchorDesign = m_Viewport.PhysicalToDesign(input.MousePos);
			m_CanvasZoom = std::clamp(m_CanvasZoom * (input.Wheel > 0.0f ? 1.1f : (1.0f / 1.1f)), 0.25f, 8.0f);
			// 解 "PhysicalOrigin + anchorDesign*scale == mousePos":
			//   pan = mousePos - anchorDesign*scale - centerTerm
			// 其中 centerTerm 是"无平移时"的适配原点(与 LayoutCanvas 同一公式)。
			const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
			const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);
			const float fit = CanvasFitScale(rect);
			const float scale = std::max(0.02f, fit * m_CanvasZoom);
			const glm::vec2 center {
				rect.X + (rect.W - designWidth * scale) * 0.5f,
				rect.Y + (rect.H - designHeight * scale) * 0.5f };
			m_CanvasPan = input.MousePos - anchorDesign * scale - center;
			m_Status = Wui::Tr("panel.ui_designer.zoom", "Zoom ")
				+ std::to_string(static_cast<int>(m_CanvasZoom * 100.0f)) + "%";
			// 滚轮被画布吃掉:不要让下面的滚动区再滚一次。
			ctx.ConsumePointerClick();
		}

		// 中键拖动 / 空格+左键拖动 = 平移画布。
		const bool panButton = input.MouseDown[2] || (input.MouseDown[0] && input.KeyDown.end() !=
			std::find(input.KeyDown.begin(), input.KeyDown.end(), static_cast<uint32_t>(KeyCodes::Space)));
		if (!m_CanvasPanning && ctx.IsHovered(rect) && panButton)
		{
			m_CanvasPanning = true;
			m_CanvasPanStart = m_CanvasPan;
			m_CanvasPanMouse = input.MousePos;
			CancelDrag();
		}
		if (m_CanvasPanning)
		{
			m_CanvasPan = m_CanvasPanStart + (input.MousePos - m_CanvasPanMouse);
			if (!input.MouseDown[2] && !input.MouseDown[0])
				m_CanvasPanning = false;
			return;   // 平移期间不参与选中/拖动,避免误改文档
		}

		// M12:选中节点的 8 个手柄悬停态(仅反馈用;拖动中不重算)。
		m_HoverHandle = HandleNone;
		Wui::WuiRect selectedBox;
		const bool hasSelectedBox = SelectedNodeBox(selectedBox);
		if (hasSelectedBox && m_Drag == CanvasDrag::None)
		{
			for (const HandleDef& handle : kHandles)
			{
				if (ctx.IsHovered(HandlePhysicalRect(selectedBox, handle)))
				{
					m_HoverHandle = handle.Bits;
					break;
				}
			}
		}

		if (m_Drag == CanvasDrag::None && ctx.IsHovered(rect) && ctx.IsClicked(rect, 0))
		{
			// ① 手柄优先:命中 8 手柄之一 → 缩放当前选中节点(不改变选中)。
			UI::UiNode* node = m_HoverHandle != HandleNone ? MutableSelectedNode() : nullptr;
			if (node != nullptr)
			{
				m_Drag = CanvasDrag::Resize;
				m_DragHandle = m_HoverHandle;
				m_DragNodeId = node->Id;
				m_DragStartDesign = m_Viewport.PhysicalToDesign(input.MousePos);
				m_DragStartOffset = node->Anchor.Offset;
				m_DragStartSize = node->Anchor.Size;
				m_DragBefore = m_Document;
				m_DragBeforeValid = true;
			}
			else
			{
				// ② 否则节点命中 = 选中 + 拖动 Offset;命中空白 = 取消选中。
				const UI::UiNodeInstance* hit = m_Screen.HitTest(input.MousePos);
				if (hit != nullptr)
				{
					CommitNodeEdit();
					m_SelectedId = hit->Id;
					if (const UI::UiNode* source = m_Document.FindNode(hit->Id))
					{
						m_Drag = CanvasDrag::Move;
						m_DragHandle = HandleNone;
						m_DragNodeId = hit->Id;
						m_DragStartDesign = m_Viewport.PhysicalToDesign(input.MousePos);
						m_DragStartOffset = source->Anchor.Offset;
						m_DragStartSize = source->Anchor.Size;
						m_DragBefore = m_Document;
						m_DragBeforeValid = true;
					}
				}
				else
				{
					CommitNodeEdit();
					m_SelectedId.clear();
					CancelDrag();
				}
			}
		}

		if (m_Drag != CanvasDrag::None && input.MouseDown[0] && !m_DragNodeId.empty())
		{
			UI::UiNode* node = FindNodeMutable(m_Document.Nodes, m_DragNodeId);
			if (node != nullptr)
			{
				const glm::vec2 design = m_Viewport.PhysicalToDesign(input.MousePos);
				const glm::vec2 delta = design - m_DragStartDesign;
				if (m_Drag == CanvasDrag::Move)
				{
					// M19:吸附到整数设计单位(Alt = 临时关掉,做像素级微调)。
					glm::vec2 next { m_DragStartOffset.x + delta.x, m_DragStartOffset.y + delta.y };
					if (!input.Alt)
						next = SnapDesign(next);
					if (next.x != node->Anchor.Offset.x || next.y != node->Anchor.Offset.y)
					{
						node->Anchor.Offset = next;
						m_ScreenDirty = true;
						m_Status = Wui::Tr("panel.ui_designer.drag_offset", "Offset ")
							+ FormatFloat(next.x) + ", " + FormatFloat(next.y);
					}
				}
				else
				{
					// 手柄语义:拖哪条边就动哪条边,**对边钉住不动**(与主流 UI 编辑器一致)。
					//
					// 两种锚定模式下手柄必须做的事不同(这是 M19 修的真 bug:此前只改 Size,
					// 于是拖左边/上边时对边跟着跑,表现为"整框在移动而不是在缩放"):
					//
					//   点锚定(Min == Max):left = Offset.x - Pivot.x*W,right = Offset.x + (1-Pivot.x)*W
					//     * 拖右边:W += delta(左边天然不动)⇒ 只改 Size
					//     * 拖左边:W -= delta,再要 left 动 delta ⇒ Offset.x += delta*(1-Pivot.x)
					//   拉伸(Min != Max):left = Offset.x(offsetMin),W = 锚框跨度 + Size
					//     * 拖右边:Size += delta
					//     * 拖左边:Offset.x += delta 且 Size -= delta(两边一起动,右边缘才不动)
					//
					// Y 轴同理(屏幕 Y 向下,"上边"= Y 变小)。
					const bool pointX = node->Anchor.Min.x == node->Anchor.Max.x;
					const bool pointY = node->Anchor.Min.y == node->Anchor.Max.y;
					glm::vec2 size = m_DragStartSize;
					glm::vec2 offset = m_DragStartOffset;
					if ((m_DragHandle & HandleRight) != 0)
						size.x = m_DragStartSize.x + delta.x;
					if ((m_DragHandle & HandleLeft) != 0)
						size.x = m_DragStartSize.x - delta.x;
					if ((m_DragHandle & HandleBottom) != 0)
						size.y = m_DragStartSize.y + delta.y;
					if ((m_DragHandle & HandleTop) != 0)
						size.y = m_DragStartSize.y - delta.y;

					const bool corner = (m_DragHandle & (HandleLeft | HandleRight)) != 0
						&& (m_DragHandle & (HandleTop | HandleBottom)) != 0;
					if (corner && input.Shift && m_DragStartSize.x != 0.0f && m_DragStartSize.y != 0.0f)
					{
						// Shift = 等比:以变化更大的轴为准缩放两轴。
						const float sx = size.x / m_DragStartSize.x;
						const float sy = size.y / m_DragStartSize.y;
						const float scale = std::abs(sx) > std::abs(sy) ? sx : sy;
						size.x = m_DragStartSize.x * scale;
						size.y = m_DragStartSize.y * scale;
					}
					// 点锚定时 Size 就是实际尺寸 ⇒ 钳到 >= 0;拉伸时 Size 是尺寸增量,允许为负。
					if (pointX && size.x < 0.0f)
						size.x = 0.0f;
					if (pointY && size.y < 0.0f)
						size.y = 0.0f;

					// M19:尺寸也吸附到整数设计单位(Alt 关掉)。
					if (!input.Alt)
						size = SnapDesign(size);

					// 拖左边/上边时反推 Offset,使**对边**在任意钳位之后仍然钉住:
					//   点锚定:right = Offset.x + (1-Pivot.x)*W ⇒ Offset.x = right0 - (1-Pivot.x)*W
					//   拉伸  :right = Offset.x + 跨度 + Size ⇒ Offset.x = right0 - 跨度 - Size
					// (right0 = 按下那一刻的右边缘;钳位改了 W,这里按钳后的 W 重算,所以缩到 0 时
					//  左边缘正好停在右边缘上,而不是把整框推走。)
					if ((m_DragHandle & HandleLeft) != 0)
					{
						const float right0 = pointX
							? m_DragStartOffset.x + (1.0f - node->Anchor.Pivot.x) * m_DragStartSize.x
							: m_DragStartOffset.x + m_DragStartSize.x;
						offset.x = pointX
							? right0 - (1.0f - node->Anchor.Pivot.x) * size.x
							: right0 - size.x;
					}
					if ((m_DragHandle & HandleTop) != 0)
					{
						const float bottom0 = pointY
							? m_DragStartOffset.y + (1.0f - node->Anchor.Pivot.y) * m_DragStartSize.y
							: m_DragStartOffset.y + m_DragStartSize.y;
						offset.y = pointY
							? bottom0 - (1.0f - node->Anchor.Pivot.y) * size.y
							: bottom0 - size.y;
					}

					if (size.x != node->Anchor.Size.x || size.y != node->Anchor.Size.y ||
						offset.x != node->Anchor.Offset.x || offset.y != node->Anchor.Offset.y)
					{
						node->Anchor.Size = size;
						node->Anchor.Offset = offset;
						m_ScreenDirty = true;
						m_Status = Wui::Tr("panel.ui_designer.drag_size", "Size ")
							+ FormatFloat(size.x) + ", " + FormatFloat(size.y);
					}
				}
			}
		}

		if (m_Drag != CanvasDrag::None && !input.MouseDown[0])
		{
			// 一次拖动 = 一条记录:只有真的改动了文档才入栈(单击选中不入栈)。
			if (m_DragBeforeValid && !UI::UiDocumentsEquivalent(m_DragBefore, m_Document))
			{
				const bool resize = m_Drag == CanvasDrag::Resize;
				PushDocumentUndo(resize
					? Wui::Tr("panel.ui_designer.resize", "Resize")
					: Wui::Tr("panel.ui_designer.move", "Move"),
					m_DragBefore);
				m_Dirty = true;
			}
			CancelDrag();
		}
	}

	void UiDesignerPanel::DrawCanvas(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float designWidth = std::max(m_Document.Design.Resolution.x, 1.0f);
		const float designHeight = std::max(m_Document.Design.Resolution.y, 1.0f);

		const Wui::WuiRect designRect = m_Viewport.DesignRectToPhysical(
			Wui::WuiRect { 0.0f, 0.0f, designWidth, designHeight });
		Wui::PanelBackground(ctx, designRect, theme.ContentBg, 0.0f);
		Wui::HighlightOutline(ctx, designRect, theme.Border, 0.0f, 1.0f);

		// 节点框 + 类型标签(线框视图;不画真实控件外观)。
		// M12:拖动中(移动/缩放)选中框换成警示色,给"正在拖动"的反馈。
		const bool dragging = m_Drag != CanvasDrag::None;
		for (const UI::UiNodeInstance& node : m_Screen.Nodes())
		{
			const Wui::WuiRect box = NodeBoxPhysical(m_Viewport, node.Rect);
			const bool selected = (node.Id == m_SelectedId);
			if (selected)
				Wui::PanelBackground(ctx, box, theme.Selection, 0.0f);
			const Wui::WuiColor outline = selected
				? (dragging ? theme.Warning : theme.Accent) : theme.BorderStrong;
			Wui::HighlightOutline(ctx, box, outline, 0.0f, selected ? 2.0f : 1.0f);
			Wui::Label(ctx, glm::vec2 { box.X + 3.0f, box.Y + 2.0f }, node.Type + "  " + node.Id,
				selected ? theme.Text : theme.TextMuted, theme.FontSizeCaption);
		}

		// M12:选中节点的 8 个缩放手柄(拖角改两轴、拖边改单轴;拖动手柄描边用强调色)。
		Wui::WuiRect selectedBox;
		if (SelectedNodeBox(selectedBox))
			DrawResizeHandles(ctx, theme, selectedBox);

		// M19:拖动/缩放中点亮对齐参考线(选中框的边或中心与"父框 / 视口内容矩形"的对应位置对齐时)。
		if (dragging && SelectedNodeBox(selectedBox))
			DrawAlignmentGuides(ctx, theme, selectedBox);

		// 安全区(虚线)与设计面区分。
		const Wui::WuiRect safeRect = m_Viewport.DesignRectToPhysical(m_Viewport.ContentRect);
		DashedRectOutline(ctx, safeRect, theme.Warning, 1.0f);
		Wui::Label(ctx, glm::vec2 { safeRect.X + 4.0f, safeRect.Y + 3.0f },
			Wui::Tr("panel.ui_designer.safe_area", "Safe Area"), theme.Warning, theme.FontSizeCaption);

		// 锚点标记:选中节点优先;没有选中时给根节点画,让"锚点在父矩形上的位置"始终看得见。
		if (const UI::UiNodeInstance* selected = m_Screen.Find(m_SelectedId))
		{
			DrawAnchorMarkers(ctx, theme, *selected);
		}
		else
		{
			for (const UI::UiNodeInstance& node : m_Screen.Nodes())
			{
				if (node.Parent == -1)
					DrawAnchorMarkers(ctx, theme, node);
			}
		}

		Wui::HighlightOutline(ctx, rect, theme.Border, 2.0f, 1.0f);
	}

	void UiDesignerPanel::DrawAnchorMarkers(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const UI::UiNodeInstance& node)
	{
		if (node.Source == nullptr)
			return;

		const Wui::WuiRect parentRect = node.Parent >= 0
			? m_Screen.Nodes()[static_cast<std::size_t>(node.Parent)].Rect
			: m_Viewport.ContentRect;
		const UI::UiAnchor& anchor = node.Source->Anchor;
		const glm::vec2 parentMin { parentRect.X, parentRect.Y };
		const glm::vec2 parentSize { parentRect.W, parentRect.H };

		const glm::vec2 minCanvas = m_Viewport.DesignToPhysical(parentMin + anchor.Min * parentSize);
		const glm::vec2 maxCanvas = m_Viewport.DesignToPhysical(parentMin + anchor.Max * parentSize);

		Wui::LineSegment(ctx, minCanvas, maxCanvas, theme.BorderStrong, 1.0f);
		MarkerCross(ctx, minCanvas, theme.Success);
		MarkerCross(ctx, maxCanvas, theme.Warning);
		Wui::PanelBackground(ctx, Wui::WuiRect { minCanvas.x - 2.0f, minCanvas.y - 2.0f, 4.0f, 4.0f },
			theme.Success, 0.0f);
		Wui::PanelBackground(ctx, Wui::WuiRect { maxCanvas.x - 2.0f, maxCanvas.y - 2.0f, 4.0f, 4.0f },
			theme.Warning, 0.0f);
	}

	// ---- M12:撤销/重做(面板本地 WuiUndoStack)----

	void UiDesignerPanel::HandleShortcuts(Wui::WuiContext& ctx)
	{
		if (!m_HasDocument || m_TextFocusLatched)
			return;
		const Wui::WuiInputState& input = ctx.Input();
		if (!input.Ctrl)
		{
			// ---- M19:不带修饰键的常用操作(只在没有文本焦点时生效)----
			// 方向键微调选中节点(Shift = ×10):改的是 Anchor.Offset,与鼠标拖动同一条模型路径。
			if (!m_SelectedId.empty() && m_Drag == CanvasDrag::None)
			{
				const float step = input.Shift ? 10.0f : 1.0f;
				if (ctx.WasKeyTriggered(KeyCodes::Left)) NudgeSelected(0, -step);
				else if (ctx.WasKeyTriggered(KeyCodes::Right)) NudgeSelected(0, step);
				else if (ctx.WasKeyTriggered(KeyCodes::Up)) NudgeSelected(1, -step);
				else if (ctx.WasKeyTriggered(KeyCodes::Down)) NudgeSelected(1, step);
			}
			// Delete/Backspace = 删除选中节点(与工具栏 Delete 同一条守卫)。
			if (ctx.WasKeyTriggered(KeyCodes::Delete) || ctx.WasKeyTriggered(KeyCodes::Backspace))
				DeleteSelectedNode();
			// F = 复位画布缩放/平移(看清全貌)。
			if (ctx.WasKeyTriggered(KeyCodes::F))
			{
				m_CanvasZoom = 1.0f;
				m_CanvasPan = glm::vec2 { 0.0f, 0.0f };
				m_Status = Wui::Tr("panel.ui_designer.view_reset", "Canvas view reset");
			}
			return;
		}
		// Ctrl+Z = 撤销;Ctrl+Y / Ctrl+Shift+Z = 重做(与编辑器其它面板同口径)。
		if (!input.Shift && ctx.WasKeyTriggered(KeyCodes::Z))
			UndoDocument();
		else if (ctx.WasKeyTriggered(KeyCodes::Y) || (input.Shift && ctx.WasKeyTriggered(KeyCodes::Z)))
			RedoDocument();
		// M19:Ctrl+D 复制 / Ctrl+S 保存(与工具栏按钮同一条路径)。
		else if (ctx.WasKeyTriggered(KeyCodes::D))
			DuplicateSelectedNode();
		else if (ctx.WasKeyTriggered(KeyCodes::S))
			SaveTo(m_Path.empty() ? ResolveInputPath(m_PathBuffer) : m_Path);
	}

	// M19:方向键微调 —— 与鼠标拖动共用"整档前像 + 一条撤销记录"的口径(连续微调合并成一条,
	// 直到指针/按键停下来:这里用 NoteNodeEdit/CommitNodeEdit 的同一套合并机制)。
	void UiDesignerPanel::NudgeSelected(int axis, float amount)
	{
		UI::UiNode* node = MutableSelectedNode();
		if (node == nullptr)
			return;
		glm::vec2 next = node->Anchor.Offset;
		(axis == 0 ? next.x : next.y) += amount;
		if (next == node->Anchor.Offset)
			return;
		NoteNodeEdit(Wui::Tr("panel.ui_designer.nudge", "Nudge"));
		node->Anchor.Offset = next;
		m_ScreenDirty = true;
		m_Status = Wui::Tr("panel.ui_designer.drag_offset", "Offset ")
			+ FormatFloat(next.x) + ", " + FormatFloat(next.y);
	}

	// M19:把移动/缩放的结果吸附到**整数设计单位**(像素级对齐;Alt 或缩放很小时跳过)。
	// 为什么是 1 而不是 8:设计单位通常就是像素,整数对齐能保证描边落在像素网格上;
	// 需要粗网格时按住 Shift(×10 微调)或直接用属性页输入精确值。
	glm::vec2 UiDesignerPanel::SnapDesign(glm::vec2 value) const
	{
		if (m_CanvasZoom < 0.6f)
			return value;   // 缩得太小,1 单位的吸附感会变成"跟不上鼠标"
		return glm::vec2 { std::round(value.x), std::round(value.y) };
	}

	void UiDesignerPanel::UndoDocument()
	{
		if (!m_Undo.CanUndo())
		{
			m_Status = Wui::Tr("panel.ui_designer.undo_empty", "Nothing to undo");
			return;
		}
		const std::string name = m_Undo.UndoName();
		if (m_Undo.Undo())
			m_Status = Wui::Tr("panel.ui_designer.undone", "Undo: ") + name;
	}

	void UiDesignerPanel::RedoDocument()
	{
		if (!m_Undo.CanRedo())
		{
			m_Status = Wui::Tr("panel.ui_designer.redo_empty", "Nothing to redo");
			return;
		}
		const std::string name = m_Undo.RedoName();
		if (m_Undo.Redo())
			m_Status = Wui::Tr("panel.ui_designer.redone", "Redo: ") + name;
	}

	void UiDesignerPanel::PushDocumentUndo(const std::string& name, const UI::UiDocument& before)
	{
		// 一条记录 = 前像 + 记录时的后像(整档)。面板本地,不往 WuiContext 加全局状态。
		UI::UiDocument after = m_Document;
		m_Undo.Push(name,
			[this, before]() { RestoreDocument(before); },
			[this, after]() { RestoreDocument(after); });
	}

	void UiDesignerPanel::RestoreDocument(const UI::UiDocument& document)
	{
		CancelDrag();
		CancelNodeEdit();
		m_Document = document;
		m_ScreenDirty = true;
		m_Dirty = true;
		m_BufferNodeId.clear();   // 属性文本缓冲按新模型重建
		if (!m_SelectedId.empty() && m_Document.FindNode(m_SelectedId) == nullptr)
			m_SelectedId.clear();
	}

	void UiDesignerPanel::NoteNodeEdit(const std::string& name)
	{
		if (m_PendingEditValid)
			return;   // 一次连续编辑只记第一条(拖动 = 一条记录)
		// 前像必须是"本次改动之前"的同一节点快照;对不上(不该发生)宁可不记,
		// 也不能拿一份空/别的节点当撤销前像(会把节点写坏)。
		if (m_FrameNodeBefore.Id != m_SelectedId)
			return;
		m_PendingEditValid = true;
		m_PendingEditNodeId = m_SelectedId;
		m_PendingEditBefore = m_FrameNodeBefore;
		m_PendingEditName = name;
	}

	void UiDesignerPanel::CommitNodeEdit()
	{
		if (!m_PendingEditValid)
			return;
		const std::string nodeId = m_PendingEditNodeId;
		const std::string name = m_PendingEditName;
		const UI::UiNode before = m_PendingEditBefore;
		CancelNodeEdit();
		if (nodeId.empty())
			return;
		const UI::UiNode* current = m_Document.FindNode(nodeId);
		if (current == nullptr)
			return;
		// 后像 = 当前文档(节点就是 current);前像 = 当前文档把该节点换回 before。
		// 拖回原值 / 未真正改动时不落空记录。
		const UI::UiDocument after = m_Document;
		UI::UiDocument beforeDoc = m_Document;
		if (UI::UiNode* slot = FindNodeMutable(beforeDoc.Nodes, nodeId))
			*slot = before;
		if (UI::UiDocumentsEquivalent(beforeDoc, after))
			return;
		PushDocumentUndo(name.empty() ? Wui::Tr("panel.ui_designer.edit", "Edit") : name, beforeDoc);
		m_Dirty = true;
	}

	void UiDesignerPanel::CancelNodeEdit()
	{
		m_PendingEditValid = false;
		m_PendingEditNodeId.clear();
		m_PendingEditName.clear();
	}

	void UiDesignerPanel::CancelDrag()
	{
		m_Drag = CanvasDrag::None;
		m_DragHandle = HandleNone;
		m_HoverHandle = HandleNone;
		m_DragNodeId.clear();
		m_DragBeforeValid = false;
	}

	// ---- M12:同父内上移/下移(改 Children 顺序 = 改绘制顺序)----

	bool UiDesignerPanel::CanMoveSelected(int direction) const
	{
		std::size_t index = 0;
		std::size_t count = 0;
		if (m_SelectedId.empty() || !LocateSiblingIndex(m_Document.Nodes, m_SelectedId, &index, &count))
			return false;
		return direction < 0 ? index > 0 : index + 1 < count;
	}

	void UiDesignerPanel::MoveSelectedNode(int direction)
	{
		if (m_SelectedId.empty())
			return;
		std::vector<UI::UiNode>* list = nullptr;
		std::size_t index = 0;
		if (!LocateSibling(m_Document.Nodes, m_SelectedId, &list, &index) || list == nullptr)
			return;
		if (direction < 0 && index == 0)
			return;
		const std::size_t target = direction < 0 ? index - 1 : index + 1;
		if (target >= list->size())
			return;

		CommitNodeEdit();
		const UI::UiDocument before = m_Document;
		std::swap((*list)[index], (*list)[target]);
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(direction < 0
			? Wui::Tr("panel.ui_designer.move_up", "Move Up")
			: Wui::Tr("panel.ui_designer.move_down", "Move Down"), before);
		m_Status = direction < 0
			? Wui::Tr("panel.ui_designer.moved_up", "Moved node up (earlier in draw order)")
			: Wui::Tr("panel.ui_designer.moved_down", "Moved node down (later in draw order)");
	}

	// ---- M16:空态文档浏览 / 节点增删复制 ----

	void UiDesignerPanel::ScanRecentDocuments()
	{
		m_RecentScanned = true;
		m_RecentDocuments.clear();
		m_RecentScroll = 0.0f;

		std::error_code error;
		std::filesystem::path root = ContentRoot();
		if (root.empty())
		{
			root = std::filesystem::current_path(error);
			error.clear();
		}
		if (root.empty())
			return;

		// 优先扫 <内容根>/ui;这个目录不存在才退回 <内容根> 全树(带访问量上限,
		// 无内容根时不会把 build 目录整棵扫穿)。
		//
		// 口径:`ContentRoot()` = `Paths::AssetRoot()` = 项目的**内容根**(即 `assets/`),
		// 所以 UI 目录是 `<内容根>/ui`,不是 `<内容根>/assets/ui`(后者会拼成 assets/assets)。
		const std::filesystem::path uiDirectory = root / "ui";
		const std::filesystem::path scanRoot =
			std::filesystem::is_directory(uiDirectory, error) && !error ? uiDirectory : root;

		constexpr std::size_t kRecentLimit = 50;
		constexpr std::size_t kCollectLimit = 200;
		constexpr std::size_t kVisitLimit = 20000;

		std::vector<std::filesystem::path> found;
		std::size_t visited = 0;
		std::filesystem::recursive_directory_iterator iterator(scanRoot,
			std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator finish;
		while (iterator != finish && visited < kVisitLimit && found.size() < kCollectLimit)
		{
			++visited;
			std::error_code fileError;
			if (iterator->is_regular_file(fileError) && !fileError)
			{
				std::string extension = iterator->path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (extension == UI::kUiDocumentExtension)
					found.push_back(iterator->path());
			}
			iterator.increment(error);
			if (error)
				break;   // 遍历出错:拿到已扫到的就够(不抛异常)
		}

		std::sort(found.begin(), found.end());
		if (found.size() > kRecentLimit)
			found.resize(kRecentLimit);

		for (const std::filesystem::path& absolute : found)
		{
			std::error_code relativeError;
			const std::filesystem::path relative = std::filesystem::relative(absolute, root, relativeError);
			RecentDocument entry;
			entry.Path = absolute;
			entry.RelativePath = (relativeError ? absolute.filename() : relative).generic_string();
			m_RecentDocuments.push_back(std::move(entry));
		}
	}

	void UiDesignerPanel::RenderDocumentBrowser(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		if (!m_RecentScanned)
			ScanRecentDocuments();
		// M19:空态列表在面板打开期间也可能变(从内容浏览器新导出一份 .wui)——
		// 每 ~2 秒(120 帧)重扫一次,否则用户要关掉面板再打开才看得到。
		else if (ctx.Frame() >= m_RecentScanFrame + 120)
		{
			m_RecentScanFrame = ctx.Frame();
			ScanRecentDocuments();
		}

		const bool hasFiles = !m_RecentDocuments.empty();
		const std::string title = Wui::Tr("panel.ui_designer.no_document", "No UI document open");
		const std::string hint = hasFiles
			? Wui::Tr("panel.ui_designer.no_document_hint_browse",
				"Pick a .wui found in the content root below, or press New to start a blank one.")
			: Wui::Tr("panel.ui_designer.no_document_hint_none",
				"No .wui found under the content root - press New to create one.");

		const float headerHeight = std::min(std::max(96.0f, rect.H * 0.38f), rect.H);
		Wui::EmptyState(ctx, Wui::WuiRect { rect.X, rect.Y, rect.W, headerHeight }, std::string(),
			title, hint, std::string(), 0, theme);

		if (!hasFiles)
			return;

		const Wui::WuiRect listRect { rect.X + 10.0f, rect.Y + headerHeight + 4.0f,
			std::max(0.0f, rect.W - 20.0f), std::max(0.0f, rect.H - headerHeight - 14.0f) };
		if (!(listRect.H > 8.0f) || !(listRect.W > 40.0f))
			return;

		constexpr float rowHeight = 24.0f;
		constexpr float rowGap = 2.0f;
		const float contentHeight = static_cast<float>(m_RecentDocuments.size()) * (rowHeight + rowGap);
		Wui::BeginScrollArea(ctx, listRect, contentHeight, m_RecentScroll, theme,
			Wui::HashId("ui_designer.recent.scroll"));
		float rowY = listRect.Y - m_RecentScroll;
		for (std::size_t index = 0; index < m_RecentDocuments.size(); ++index)
		{
			const RecentDocument& entry = m_RecentDocuments[index];
			const Wui::WuiRect row { listRect.X, rowY, listRect.W, rowHeight };
			// a11y id 形如 ui_designer.recent.<n>(n = 按路径排序后的下标);按钮文案带相对路径。
			const Wui::WuiId id = Wui::HashId(("ui_designer.recent." + std::to_string(index)).c_str());
			const std::string label = Wui::Tr("panel.ui_designer.open", "Open") + " " + entry.RelativePath;
			if (Wui::ActionButton(ctx, id, row, label, theme, true,
				Wui::Tr("panel.ui_designer.recent.tip", "Load this .wui document")))
			{
				LoadFrom(entry.Path);
			}
			rowY += rowHeight + rowGap;
		}
		Wui::EndScrollArea(ctx);
	}

	bool UiDesignerPanel::CreateNewDocument()
	{
		std::error_code error;
		// M19(安全):`New` 只在"路径框为空 / 指向的文件不存在"时才用它。
		// 原实现无条件采用路径框现值 —— 先 Open 一份文档再点 New,就会以同一路径建出空文档,
		// 接着 Save 直接覆盖原文件(实测踩到的数据丢失路径)。
		std::filesystem::path target = ResolveInputPath(m_PathBuffer);
		if (!target.empty() && std::filesystem::exists(target, error))
		{
			m_Status = Wui::Tr("panel.ui_designer.new_refused",
				"New refuses to reuse an existing file — clear the path box or pick another name");
			return false;
		}
		if (target.empty())
		{
			// 路径框为空:落 <内容根>/ui/untitled.wui;已存在则加 -1、-2 后缀。
			// (`<内容根>` 已经是项目的 `assets/`;再加一层 `assets/` 会拼成 assets/assets。)
			std::filesystem::path root = ContentRoot();
			if (root.empty())
				root = std::filesystem::current_path(error);
			if (root.empty())
			{
				m_Status = Wui::Tr("panel.ui_designer.new_no_root",
					"Cannot resolve a content root for the new document");
				return false;
			}
			const std::filesystem::path directory = root / "ui";
			target = directory / "untitled.wui";
			for (int suffix = 1; suffix < 10000 && std::filesystem::exists(target, error); ++suffix)
				target = directory / ("untitled-" + std::to_string(suffix) + ".wui");
		}
		else if (!target.has_extension())
		{
			target += UI::kUiDocumentExtension;
		}

		// 最小可用文档:Panel 根 1920x1080 铺满 + 一个 Label 子节点。
		UI::UiDocument document;
		std::string screenName;
		for (const char c : target.stem().string())
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-')
				screenName.push_back(c);
		}
		document.Screen = screenName.empty() ? std::string("Screen") : screenName;
		document.Design.Resolution = glm::vec2 { 1920.0f, 1080.0f };

		UI::UiNode root;
		root.Id = "root";
		root.Type = "Panel";
		root.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		root.Anchor.Max = glm::vec2 { 1.0f, 1.0f };
		root.Anchor.Pivot = glm::vec2 { 0.5f, 0.5f };
		root.Anchor.Offset = glm::vec2 { 0.0f, 0.0f };
		root.Anchor.Size = glm::vec2 { 0.0f, 0.0f };

		UI::UiNode label;
		label.Type = "Label";
		label.Id = UI::MakeStableId(root.Id, label.Type, 0);
		label.Props.push_back(UI::UiProp { "text", "Label" });
		label.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Max = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Pivot = glm::vec2 { 0.0f, 0.0f };
		label.Anchor.Offset = glm::vec2 { 40.0f, 40.0f };
		label.Anchor.Size = glm::vec2 { 240.0f, 40.0f };
		root.Children.push_back(std::move(label));
		document.Nodes.push_back(std::move(root));

		m_Document = std::move(document);
		m_Path = target;
		m_PathBuffer = target.string();
		// 根节点默认选中:画布上立刻有选择框/手柄,属性栏也不再是空的。
		m_SelectedId = m_Document.Nodes.empty() ? std::string() : m_Document.Nodes.front().Id;
		m_BufferNodeId.clear();
		m_Undo.Clear();          // 换文档 = 撤销历史作废(与 LoadFrom 同口径)
		CancelDrag();
		CancelNodeEdit();
		m_Collapsed.clear();
		m_OutlineScroll = 0.0f;
		m_PropertyScroll = 0.0f;
		m_RecentScroll = 0.0f;
		m_HasDocument = true;
		m_ScreenDirty = true;
		m_Dirty = true;          // 还没落盘:状态行显示未保存,Save 按钮可用
		RebuildScreen();
		m_Status = Wui::Tr("panel.ui_designer.new_unsaved", "New (unsaved): ")
			+ target.filename().string()
			+ Wui::Tr("panel.ui_designer.new_unsaved_hint", " - press Save to write it");
		return true;
	}

	bool UiDesignerPanel::AddNode(const std::string& type)
	{
		if (type.empty())
			return false;
		if (!m_HasDocument)
		{
			m_Status = Wui::Tr("panel.ui_designer.need_document",
				"Open or create a document first (New)");
			return false;
		}
		if (UI::UiNodeRegistry::Find(type) == nullptr)
		{
			m_Status = Wui::Tr("panel.ui_designer.unknown_type", "Unknown node type: ") + type;
			return false;
		}
		CommitNodeEdit();

		// 选中节点存在 -> 挂到它的 Children 末尾;没有选中 -> 作为新的根节点。
		std::vector<UI::UiNode>* targetList = &m_Document.Nodes;
		std::string parentPath;
		if (!m_SelectedId.empty())
		{
			if (UI::UiNode* parent = FindNodeMutable(m_Document.Nodes, m_SelectedId))
			{
				targetList = &parent->Children;
				parentPath = FindNodePath(m_Document, m_SelectedId);
			}
		}

		const UI::UiDocument before = m_Document;
		std::set<std::string> reserved;
		UI::UiNode node;
		node.Type = type;
		node.Id = MakeUniqueNodeId(m_Document, reserved, parentPath, type, targetList->size());
		node.IdWasGenerated = false;
		// 默认锚点:父矩形左上角一个 240x48 的固定盒子(容器布局会覆盖,不影响)。
		node.Anchor.Min = glm::vec2 { 0.0f, 0.0f };
		node.Anchor.Max = glm::vec2 { 0.0f, 0.0f };
		node.Anchor.Pivot = glm::vec2 { 0.0f, 0.0f };
		node.Anchor.Offset = glm::vec2 { 24.0f, 24.0f };
		node.Anchor.Size = glm::vec2 { 240.0f, 48.0f };
		const std::string newId = node.Id;
		targetList->push_back(std::move(node));

		m_SelectedId = newId;
		m_BufferNodeId.clear();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.add_node", "Add Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.added", "Added ") + newId;
		return true;
	}

	void UiDesignerPanel::DeleteSelectedNode()
	{
		if (!CanDeleteSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.delete_blocked",
				"Select a node first; the last root node cannot be deleted");
			return;
		}
		CommitNodeEdit();

		NodeLocation location;
		if (!LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
			|| location.List == nullptr)
			return;
		const UI::UiDocument before = m_Document;
		const std::string removedId = m_SelectedId;
		location.List->erase(location.List->begin() + static_cast<std::ptrdiff_t>(location.Index));
		CancelDrag();
		CancelNodeEdit();
		m_SelectedId.clear();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.delete_node", "Delete Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.deleted", "Deleted ") + removedId;
	}

	// M19:对齐参考线 —— 拖动/缩放时,选中框的左/中/右、上/中/下与**父框**(没有父则视口内容矩形)
	// 的对应位置在 1 设计单位内对齐时,画一条横跨父框的虚线。只在拖动期间画,零常驻开销。
	// 与吸附的关系:吸附给"手感",参考线给"看得见的理由" —— 两者共用同一容差。
	void UiDesignerPanel::DrawAlignmentGuides(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& box)
	{
		const UI::UiNodeInstance* selected = m_Screen.Find(m_SelectedId);
		if (selected == nullptr)
			return;
		Wui::WuiRect reference = m_Viewport.ContentRect;
		if (selected->Parent >= 0 && selected->Parent < static_cast<int>(m_Screen.Nodes().size()))
			reference = m_Screen.Nodes()[static_cast<std::size_t>(selected->Parent)].Rect;

		constexpr float kTolerance = 1.0f;
		const float boxXs[3] = { box.X, box.X + box.W * 0.5f, box.X + box.W };
		const float boxYs[3] = { box.Y, box.Y + box.H * 0.5f, box.Y + box.H };
		const float refXs[3] = { reference.X, reference.X + reference.W * 0.5f, reference.X + reference.W };
		const float refYs[3] = { reference.Y, reference.Y + reference.H * 0.5f, reference.Y + reference.H };

		for (const float bx : boxXs)
		{
			for (const float rx : refXs)
			{
				if (std::abs(bx - rx) <= kTolerance)
				{
					Wui::LineSegment(ctx, glm::vec2 { rx, reference.Y }, glm::vec2 { rx, reference.Y + reference.H },
						theme.Accent, 1.0f);
					break;
				}
			}
		}
		for (const float by : boxYs)
		{
			for (const float ry : refYs)
			{
				if (std::abs(by - ry) <= kTolerance)
				{
					Wui::LineSegment(ctx, glm::vec2 { reference.X, ry }, glm::vec2 { reference.X + reference.W, ry },
						theme.Accent, 1.0f);
					break;
				}
			}
		}
	}

	void UiDesignerPanel::DuplicateSelectedNode()
	{
		if (!CanDuplicateSelected())
		{
			m_Status = Wui::Tr("panel.ui_designer.duplicate_blocked", "Select a node first");
			return;
		}
		CommitNodeEdit();

		NodeLocation location;
		if (!LocateNode(m_Document.Nodes, m_SelectedId, std::string(), location)
			|| location.List == nullptr)
			return;
		const UI::UiDocument before = m_Document;

		// 深拷贝整棵子树(值语义),再给子树里每个节点重发唯一 Id。
		UI::UiNode copy = (*location.List)[location.Index];
		std::set<std::string> reserved;
		RegenerateSubtreeIds(m_Document, reserved, copy, location.ParentPath, location.Index + 1);
		const std::string copyId = copy.Id;
		location.List->insert(
			location.List->begin() + static_cast<std::ptrdiff_t>(location.Index + 1), std::move(copy));

		m_SelectedId = copyId;
		m_BufferNodeId.clear();
		m_ScreenDirty = true;
		m_Dirty = true;
		PushDocumentUndo(Wui::Tr("panel.ui_designer.duplicate_node", "Duplicate Node"), before);
		m_Status = Wui::Tr("panel.ui_designer.duplicated", "Duplicated to ") + copyId;
	}

	bool UiDesignerPanel::CanDeleteSelected() const
	{
		if (!CanDuplicateSelected())
			return false;
		std::size_t rootIndex = 0;
		// 最后一个根节点删掉就是空文档:这一步禁用并给理由(tooltip)。
		return !(LocateRootIndex(m_Document.Nodes, m_SelectedId, &rootIndex)
			&& m_Document.Nodes.size() <= 1);
	}

	bool UiDesignerPanel::CanDuplicateSelected() const
	{
		return m_HasDocument && !m_SelectedId.empty()
			&& m_Document.FindNode(m_SelectedId) != nullptr;
	}

	// ---- M12:画布手柄 ----

	bool UiDesignerPanel::SelectedNodeBox(Wui::WuiRect& out) const
	{
		if (!m_HasDocument || m_SelectedId.empty())
			return false;
		const UI::UiNodeInstance* instance = m_Screen.Find(m_SelectedId);
		if (instance == nullptr)
			return false;
		out = NodeBoxPhysical(m_Viewport, instance->Rect);
		return true;
	}

	void UiDesignerPanel::DrawResizeHandles(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
		const Wui::WuiRect& box)
	{
		for (const HandleDef& handle : kHandles)
		{
			const Wui::WuiRect rect = HandlePhysicalRect(box, handle);
			const bool active = m_Drag == CanvasDrag::Resize && m_DragHandle == handle.Bits;
			const bool hovered = active || m_HoverHandle == handle.Bits;
			const Wui::WuiColor fill = active
				? theme.Accent : (hovered ? theme.ActiveBg : theme.PanelHeader);
			const Wui::WuiColor border = active || hovered ? theme.Accent : theme.BorderStrong;
			Wui::PanelBackground(ctx, rect, fill, 0.0f);
			Wui::HighlightOutline(ctx, rect, border, 0.0f, 1.0f);
		}
	}
}
