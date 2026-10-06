#include "wldpch.h"
#include "WUI/Panels/UiDesignerPanel.h"

#include "World/Asset/ProjectManifest.h"
#include "World/UI/UiTypes.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <system_error>

namespace World
{
	namespace
	{
		constexpr float kToolbarHeight = 34.0f;
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
	}

	// ---- 文档生命周期 ----

	const std::filesystem::path& UiDesignerPanel::ContentRoot()
	{
		if (!m_ContentRootResolved)
		{
			m_ContentRootResolved = true;
			std::error_code error;
			std::filesystem::path manifestPath;
			if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(error), &manifestPath))
			{
				World::Asset::ProjectManifest manifest;
				std::string loadError;
				if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &loadError))
					m_ContentRoot = manifest.ResolveContentRoot(manifestPath);
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
		m_Collapsed.clear();
		m_OutlineScroll = 0.0f;
		m_PropertyScroll = 0.0f;
		m_HasDocument = true;
		m_ScreenDirty = true;
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

		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.WindowBg, 0.0f);

		const float toolbarHeight = std::min(kToolbarHeight, rect.H);
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
					? m_Path.filename().string() + "  ·  " + std::to_string(m_Document.NodeCount()) + " nodes"
					: Wui::Tr("panel.ui_designer.status_idle", "No document open");
			}
			Wui::Label(ctx, glm::vec2 { status.X + 8.0f, status.Y + 4.0f }, text,
				theme.TextMuted, theme.FontSizeSmall);
		}
	}

	void UiDesignerPanel::RenderToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelHeader, 0.0f);

		const float height = std::max(18.0f, rect.H - 8.0f);
		const float y = rect.Y + 4.0f;
		constexpr float buttonWidth = 62.0f;
		constexpr float gap = 6.0f;
		const float buttons = buttonWidth * 3.0f + gap * 3.0f;
		const float fieldWidth = std::max(80.0f, rect.W - 12.0f - buttons);
		float x = rect.X + 6.0f;

		Wui::TextFieldA11y pathA11y;
		pathA11y.Label = Wui::Tr("panel.ui_designer.path", "WUI file path");
		pathA11y.Placeholder = "assets/ui/hud.wui";
		Wui::TextField(ctx, Wui::HashId("ui_designer.path.field"),
			Wui::WuiRect { x, y, fieldWidth, height }, m_PathBuffer, theme, nullptr, &pathA11y);
		x += fieldWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.open"), Wui::WuiRect { x, y, buttonWidth, height },
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

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.reload"), Wui::WuiRect { x, y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.reload", "Reload"), theme, true, false,
			Wui::Tr("panel.ui_designer.reload.tip", "Discard in-memory edits and re-read the file from disk")))
		{
			if (m_Path.empty())
				m_Status = Wui::Tr("panel.ui_designer.need_path", "Enter a .wui path first");
			else
				LoadFrom(m_Path);
		}
		x += buttonWidth + gap;

		if (Wui::ButtonEx(ctx, Wui::HashId("ui_designer.save"), Wui::WuiRect { x, y, buttonWidth, height },
			Wui::Tr("panel.ui_designer.save", "Save"), theme, m_HasDocument, true,
			Wui::Tr("panel.ui_designer.save.tip", "Write the document back to disk (atomic)")))
		{
			const std::filesystem::path path = m_Path.empty() ? ResolveInputPath(m_PathBuffer) : m_Path;
			SaveTo(path);
		}
	}

	void UiDesignerPanel::RenderOutline(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.PanelBg, 2.0f);
		Wui::Label(ctx, glm::vec2 { rect.X + 6.0f, rect.Y + 4.0f },
			Wui::Tr("panel.ui_designer.outline", "Outline"), theme.TextMuted, theme.FontSizeCaption);

		const Wui::WuiRect area { rect.X + 2.0f, rect.Y + 18.0f,
			std::max(0.0f, rect.W - 4.0f), std::max(0.0f, rect.H - 20.0f) };

		m_OutlineIds.clear();
		std::vector<Wui::TreeViewItem> items;
		if (m_HasDocument)
			AppendOutlineItems(m_Document.Nodes, 0, items);

		const Wui::TreeViewResult result = Wui::TreeView(ctx, area, items, kTreeRowHeight,
			m_OutlineScroll, theme, Wui::HashId("ui_designer.outline.tree"));

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
			Wui::EmptyState(ctx, rect, std::string(),
				Wui::Tr("panel.ui_designer.no_document", "No UI document open"),
				Wui::Tr("panel.ui_designer.no_document_hint",
					"Type a .wui path in the toolbar and press Open."),
				std::string(), 0, theme);
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
		constexpr float pad = 14.0f;
		const float fitWidth = std::max(1.0f, rect.W - pad * 2.0f) / designWidth;
		const float fitHeight = std::max(1.0f, rect.H - pad * 2.0f) / designHeight;
		const float fit = std::max(0.01f, std::min(fitWidth, fitHeight));

		// 用"物理面 = 设计分辨率"(比例 = 1)调 ComputeUiViewport:安全区内缩在设计单位下
		// 换算逐字段正确(ContentRect = 设计矩形扣安全区);再把 Scale 换成画布适配比、
		// 把原点挪到画布中心 —— 设计器是设计视图,只做等比适配、不裁切。
		UI::UiViewport viewport = UI::ComputeUiViewport(m_Document.Design, m_Document.SafeArea,
			UI::UiSurface { glm::vec2 { designWidth, designHeight }, 1.0f });
		viewport.Scale = fit;
		viewport.PhysicalSize = glm::vec2 { rect.W, rect.H };
		viewport.PhysicalOrigin = glm::vec2 {
			rect.X + (rect.W - designWidth * fit) * 0.5f,
			rect.Y + (rect.H - designHeight * fit) * 0.5f };
		m_Viewport = viewport;
		m_Screen.Layout(viewport);
	}

	void UiDesignerPanel::HandleCanvasInput(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const Wui::WuiInputState& input = ctx.Input();
		if (ctx.IsHovered(rect) && ctx.IsClicked(rect, 0))
		{
			const UI::UiNodeInstance* hit = m_Screen.HitTest(input.MousePos);
			if (hit != nullptr)
			{
				m_SelectedId = hit->Id;
				if (const UI::UiNode* source = m_Document.FindNode(hit->Id))
				{
					m_DraggingOffset = true;
					m_DragNodeId = hit->Id;
					m_DragStartDesign = m_Viewport.PhysicalToDesign(input.MousePos);
					m_DragStartOffset = source->Anchor.Offset;
				}
			}
			else
			{
				m_SelectedId.clear();
				m_DraggingOffset = false;
				m_DragNodeId.clear();
			}
		}

		if (m_DraggingOffset && input.MouseDown[0] && !m_DragNodeId.empty())
		{
			UI::UiNode* node = MutableSelectedNode();
			if (node != nullptr && node->Id == m_DragNodeId)
			{
				const glm::vec2 design = m_Viewport.PhysicalToDesign(input.MousePos);
				const glm::vec2 next { m_DragStartOffset.x + (design.x - m_DragStartDesign.x),
					m_DragStartOffset.y + (design.y - m_DragStartDesign.y) };
				if (next.x != node->Anchor.Offset.x || next.y != node->Anchor.Offset.y)
				{
					node->Anchor.Offset = next;
					m_ScreenDirty = true;
					m_Status = Wui::Tr("panel.ui_designer.drag_offset", "Offset ")
						+ FormatFloat(node->Anchor.Offset.x) + ", " + FormatFloat(node->Anchor.Offset.y);
				}
			}
		}

		if (m_DraggingOffset && !input.MouseDown[0])
		{
			m_DraggingOffset = false;
			m_DragNodeId.clear();
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
		for (const UI::UiNodeInstance& node : m_Screen.Nodes())
		{
			Wui::WuiRect box = m_Viewport.DesignRectToPhysical(node.Rect);
			const bool selected = (node.Id == m_SelectedId);
			if (box.W < 3.0f || box.H < 3.0f)
			{
				// 点锚定 + Size=0 的节点矩形是零面积:给一个小标记,否则画布上完全看不见。
				box = Wui::WuiRect { box.X, box.Y, 6.0f, 6.0f };
			}
			if (selected)
				Wui::PanelBackground(ctx, box, theme.Selection, 0.0f);
			Wui::HighlightOutline(ctx, box, selected ? theme.Accent : theme.BorderStrong, 0.0f,
				selected ? 2.0f : 1.0f);
			Wui::Label(ctx, glm::vec2 { box.X + 3.0f, box.Y + 2.0f }, node.Type + "  " + node.Id,
				selected ? theme.Text : theme.TextMuted, theme.FontSizeCaption);
		}

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
}