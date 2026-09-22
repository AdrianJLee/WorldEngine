#include "wldpch.h"
#include "ContentBrowserPanel.h"
#include "EditorAssetCatalog.h"
#include "EditorAssetTypes.h"
#include "../../EditorResources.h"

#include "World/Core/KeyCodes.h"
#include "World/Core/Application.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "World/Renderer/Texture.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Scene/Components.h"
#include "World/Scene/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <shellapi.h>
#include <string_view>

#pragma comment(lib, "shell32.lib")

namespace World
{
	namespace
	{
		std::string FormatBytes(size_t bytes)
		{
			const char* units[] = { "B", "KB", "MB", "GB", "TB" };
			double value = static_cast<double>(bytes);
			int unit = 0;
			while (value >= 1024 && unit < 4)
			{
				value /= 1024;
				++unit;
			}
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
			return buffer;
		}

		// 判断 candidate 是否等于 root 或位于 root 的子树内。
		bool IsWithinOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& root)
		{
			if (candidate == root)
				return true;
			const std::filesystem::path relative = candidate.lexically_relative(root);
			const std::string text = relative.generic_string();
			return !text.empty() && text.rfind("..", 0) != 0;
		}

		// 仅按 ASCII 大小写比较:重命名到"仅大小写不同"的名字在 Windows 上是合法操作,
		// 不能把它当成"同目录已有同名文件"拦掉。
		// P4-UX15:重命名框默认只编辑主名(后缀藏起来),提交时如果用户没写后缀就补回原名后缀。
		std::string RenameNameWithExtension(const std::filesystem::path& target, const std::string& stemEdit)
		{
			if (target.extension().empty() || std::filesystem::path(stemEdit).has_extension())
				return stemEdit;
			return stemEdit + target.extension().string();
		}

		bool EqualsNoCaseAscii(const std::string& left, const std::string& right)
		{
			if (left.size() != right.size())
				return false;
			for (size_t index = 0; index < left.size(); ++index)
			{
				const auto lower = [](unsigned char value) -> unsigned char
				{
					return value >= 'A' && value <= 'Z'
						? static_cast<unsigned char>(value - 'A' + 'a') : value;
				};
				if (lower(static_cast<unsigned char>(left[index]))
					!= lower(static_cast<unsigned char>(right[index])))
					return false;
			}
			return true;
		}

		// U2d 重命名校验:空 / 非法字符 / 同目录重名 → 返回给用户看的具体原因(空字符串 = 可提交)。
		// 与目标同名(含仅大小写不同)合法:ApplyRename 对它是无操作或只改文件名大小写。
		std::string RenameErrorFor(const std::filesystem::path& target, const std::string& newName)
		{
			if (newName.empty())
				return Wui::Tr("panel.content_browser.rename.error.empty", "Name cannot be empty");
			for (const char character : newName)
			{
				if (character == '\\' || character == '/' || character == ':' || character == '*'
					|| character == '?' || character == '"' || character == '<' || character == '>'
					|| character == '|')
					return Wui::Tr("panel.content_browser.rename.error.illegal",
						"Name contains illegal characters (\\ / : * ? \" < > |)");
			}
			if (!EqualsNoCaseAscii(newName, target.filename().string()))
			{
				std::error_code existsError;
				if (std::filesystem::exists(target.parent_path() / newName, existsError))
					return Wui::Tr("panel.content_browser.rename.error.duplicate",
						"An item with this name already exists in this folder");
			}
			return {};
		}

		// ---- P4-UX14:内容区统一切片的几何与文本工具 ----
		// 间距/颜色/字号一律取 WuiTheme 令牌;这里只放派工确认的固定尺寸
		// (切片最小 96、列表行高与树一致的 22、行内图标 16、网格图标 = 切片的一半)。
		constexpr float kSliceMinSize = 96.0f;         // 网格切片最小边长
		constexpr float kSliceIconSize = 48.0f;        // 网格图标基准边长
		constexpr float kSliceHoverIconScale = 1.04f;  // 悬停图标放大(≤4%)
		constexpr float kSelectedHoverLighten = 0.25f; // 选中 + 悬停时描边向白提亮的比例
		constexpr float kListRowHeight = 22.0f;        // 列表数据行高(与树行一致)
		constexpr float kListIconSize = 16.0f;         // 列表行内图标
		constexpr float kTableHeaderHeight = 22.0f;    // 常驻表头行高
		constexpr float kListTypeColumnWidth = 96.0f;  // 类型列基准宽
		constexpr float kListSizeColumnWidth = 96.0f;  // 大小列基准宽
		constexpr float kListColumnShare = 0.24f;      // 窄面板时列宽占面板宽的上限

		Wui::WuiColor MixColor(const Wui::WuiColor& from, const Wui::WuiColor& to, float amount)
		{
			return { from.R + (to.R - from.R) * amount, from.G + (to.G - from.G) * amount,
				from.B + (to.B - from.B) * amount, from.A + (to.A - from.A) * amount };
		}

		std::string LowerAscii(std::string text)
		{
			for (char& character : text)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return text;
		}

		// 文本按像素宽度截断(超宽补 '…')。与 WuiWidgets.cpp 内部同名实现同语义
		// (那边是文件内匿名实现,不可跨 TU 复用):逐码点累加,候选宽度 + 12px 余量超过
		// maxWidth 就停;maxWidth <= 0 时不裁剪。放不下一个字符时返回空串。
		std::string EllipsizeToWidth(const Wui::WuiContext& ctx, std::string_view text, float maxWidth,
			float fontSize)
		{
			if (maxWidth <= 0.0f || text.empty())
				return std::string(text);
			if (ctx.MeasureTextWidth(text, fontSize) <= maxWidth)
				return std::string(text);
			std::string out;
			size_t index = 0;
			bool any = false;
			while (index < text.size())
			{
				const size_t begin = index;
				size_t length = 1;
				const unsigned char lead = static_cast<unsigned char>(text[index]);
				if ((lead & 0xE0) == 0xC0) length = 2;
				else if ((lead & 0xF0) == 0xE0) length = 3;
				else if ((lead & 0xF8) == 0xF0) length = 4;
				length = std::min(length, text.size() - index);
				index += length;
				std::string candidate = out;
				candidate.append(text.substr(begin, length));
				if (ctx.MeasureTextWidth(candidate, fontSize) + 12.0f > maxWidth)
					break;
				any = true;
				out = std::move(candidate);
			}
			if (!any)
				return std::string();
			out += "…";
			return out;
		}

		// P4-UX16:新建资产的默认名候选:"<base><ext>" → "<base> (1)<ext>" → …
		// (与既有"New Folder (1)" / "material (1).wmat"同一套命名,冲突时永不覆盖)。
		std::filesystem::path MakeUniqueAssetPath(const std::filesystem::path& dir,
			const std::string& base, const std::string& extension)
		{
			std::filesystem::path candidate = dir / (base + extension);
			int counter = 1;
			std::error_code existsError;
			while (std::filesystem::exists(candidate, existsError))
				candidate = dir / (base + " (" + std::to_string(counter++) + ")" + extension);
			return candidate;
		}

		// U25-M2:把任意文本(实体名等)变成合法的资产文件名:剥掉首尾空白、
		// Windows 非法字符替换成 '_';不做其它改写(空结果由调用方兜底)。
		std::string SanitizeAssetName(const std::string& raw)
		{
			const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
			std::string name = raw;
			name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
			name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
			for (char& character : name)
				if (character == '\\' || character == '/' || character == ':' || character == '*'
					|| character == '?' || character == '"' || character == '<' || character == '>'
					|| character == '|')
					character = '_';
			return name;
		}

		// 资产基础名:去首尾空白 + 剥掉用户多打的 .wmat 后缀(后缀在落点回显行里常显)。
		std::string AssetBaseName(const std::string& raw)
		{
			std::string name = SanitizeAssetName(raw);
			constexpr size_t kSuffixLength = 5;   // ".wmat"
			if (name.size() > kSuffixLength)
			{
				std::string tail = name.substr(name.size() - kSuffixLength);
				std::transform(tail.begin(), tail.end(), tail.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				if (tail == ".wmat")
					name = name.substr(0, name.size() - kSuffixLength);
			}
			return name;
		}

		// U25-M2:新建材质向导的模板表(唯一落点)。
		// 模板只映射**现有字段** —— 真正的着色模型(Unlit / Additive 混合)是 M3 的内核工作,
		// 这里不假装:Unlit-ish 与 Additive 都在文案里写明是**近似**。
		struct NewMaterialTemplate
		{
			const char* LabelKey;
			const char* LabelEn;
			const char* DocKey;
			const char* DocEn;
		};
		const NewMaterialTemplate kNewMaterialTemplates[] = {
			{ "panel.content_browser.new_material.tpl.standard", "Standard",
				"panel.content_browser.new_material.tpl.standard.doc",
				"Engine defaults: opaque, single sided, no emissive." },
			{ "panel.content_browser.new_material.tpl.unlit", "Unlit-ish",
				"panel.content_browser.new_material.tpl.unlit.doc",
				"Approximation: emissive = base colour and metallic/roughness = 0. A real unlit shading "
				"model is M3 work." },
			{ "panel.content_browser.new_material.tpl.transparent", "Transparent",
				"panel.content_browser.new_material.tpl.transparent.doc",
				"BlendMode = Transparent (alpha blend, no depth write) and double sided off." },
			{ "panel.content_browser.new_material.tpl.additive", "Additive",
				"panel.content_browser.new_material.tpl.additive.doc",
				"Approximation: the .wmat blend enum only has Opaque/Transparent, so this writes "
				"Transparent + emissive = base x 2. A real additive blend value is M3 work." },
		};
		constexpr int kNewMaterialTemplateCount =
			static_cast<int>(sizeof(kNewMaterialTemplates) / sizeof(kNewMaterialTemplates[0]));

		// 两行折行(说明文案长于一行时按空白切一刀,第二行超出部分省略)。
		std::pair<std::string, std::string> WrapTwoLines(const Wui::WuiContext& ctx,
			const std::string& text, float width, float fontSize)
		{
			if (text.empty() || ctx.MeasureTextWidth(text, fontSize) <= width)
				return { text, std::string() };
			size_t cut = std::string::npos;
			for (size_t index = 0; index < text.size(); ++index)
			{
				if (text[index] != ' ')
					continue;
				if (ctx.MeasureTextWidth(text.substr(0, index), fontSize) > width)
					break;
				cut = index;
			}
			if (cut == std::string::npos)
				return { EllipsizeToWidth(ctx, text, width, fontSize), std::string() };
			return { text.substr(0, cut), EllipsizeToWidth(ctx, text.substr(cut + 1), width, fontSize) };
		}

		// U25-M2:模态里的动作按钮(与 U13d 的 Create Prefab 同一套画法)。
		// 主按钮 accent 填充,次按钮常规;不可用时弱化并把"为什么不可用"同时写进
		// 无障碍节点 Tooltip 与悬停提示 —— 灰按钮不能没有理由。
		bool ModalActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect,
			const std::string& label, const std::string& tooltip, bool enabled, bool primary,
			const Wui::WuiTheme& theme)
		{
			const bool hovered = ctx.IsHovered(rect);
			const Wui::WuiColor fill = !enabled ? theme.PanelBg
				: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, fill, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect,
				enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f });
			const Wui::WuiColor textColor = !enabled ? theme.TextDisabled
				: (primary ? theme.WindowBg : theme.Text);
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
				{ rect.X + 9.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0.0f, 0.0f },
				textColor, 0.0f, 1.0f, label, 15.0f, false });
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			node.Value = tooltip;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			Wui::DrawFocusRing(ctx, rect, id, theme);
			ctx.RegisterFocusable(id, rect);
			if (hovered)
			{
				if (enabled)
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (!tooltip.empty())
					ctx.SetTooltip(tooltip);
			}
			return enabled && ctx.IsClicked(rect);
		}
	}

	ContentBrowserPanel::ContentBrowserPanel(PanelHost& host)
		: m_Host(host), m_StatePath(std::string(WLD_EDITOR_DIR) + "wui-browser.json")
	{
		m_Model.Current = m_Model.Root;
		LoadState();
		RegisterDefaultAssetTypes();
	}

	ContentBrowserPanel::~ContentBrowserPanel()
	{
		// 注册表里的 Create 回调捕获了 this:必须先反注册再让面板析构,否则回调悬空。
		UnregisterDefaultAssetTypes();
		SaveState();
	}

	void ContentBrowserPanel::UpdateSearch()
	{
		m_Model.SearchResults.clear();
		std::string query = m_Model.Search;
		std::transform(query.begin(), query.end(), query.begin(), ::tolower);
		std::error_code searchError;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_Model.Current, std::filesystem::directory_options::skip_permission_denied, searchError))
		{
			if (searchError)
				break;
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			if (name.find(query) != std::string::npos)
				m_Model.SearchResults.push_back(entry.path());
		}
	}

	void ContentBrowserPanel::RefreshTree(bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!force && !m_Model.DirTreeDirty && std::chrono::duration<double>(now - m_Model.LastTreeCheck).count() < 0.5)
			return;
		m_Model.LastTreeCheck = now;

		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Model.Root, stampError);
		if (!force && !m_Model.DirTreeDirty && !stampError && stamp == m_Model.TreeStamp)
			return;

		m_Model.DirTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_Model.Root, std::filesystem::directory_options::skip_permission_denied, scanError);
		const std::filesystem::recursive_directory_iterator scanEnd;
		for (; scanIt != scanEnd; scanIt.increment(scanError))
		{
			if (scanError)
				break;
			const auto& entry = *scanIt;
			std::error_code dirError;
			if (!entry.is_directory(dirError))
				continue;
			BrowserDirNode node;
			node.Path = entry.path();
			node.Depth = scanIt.depth() + 1;
			node.HasChildren = false;
			std::error_code childError;
			for (const auto& child : std::filesystem::directory_iterator(node.Path, std::filesystem::directory_options::skip_permission_denied, childError))
			{
				if (childError)
					break;
				std::error_code childDirError;
				if (child.is_directory(childDirError)) { node.HasChildren = true; break; }
			}
			m_Model.DirTree.push_back(std::move(node));
		}
		std::sort(m_Model.DirTree.begin(), m_Model.DirTree.end(), [](const BrowserDirNode& a, const BrowserDirNode& b) { return a.Path < b.Path; });
		// D10:树里**显示根文件夹**(内容根那一行,Depth 0)。以前树从内容根的子目录开始,
		// 用户看不到"这些目录挂在谁下面";根行同时是导航/拖放/右键的对象。
		{
			BrowserDirNode rootNode;
			rootNode.Path = m_Model.Root;
			rootNode.Depth = 0;
			// 内容根是**固定根**:不给折叠标识 —— 折叠它等于把整棵树藏起来,没有意义;
			// 之前给了箭头但 Depth 1 的行恒可见,于是箭头点了没反应(用户 2026-09-21 报的问题)。
			rootNode.HasChildren = false;
			m_Model.DirTree.insert(m_Model.DirTree.begin(), std::move(rootNode));
			// 根行**首次**默认展开(之后尊重用户手动折叠)。
			if (!m_RootRowSeeded)
			{
				m_Model.TreeOpen.insert(m_Model.Root);
				m_RootRowSeeded = true;
			}
		}
		m_Model.TreeStamp = stamp;
		m_Model.DirTreeDirty = false;
	}

	void ContentBrowserPanel::RefreshListing()
	{
		const auto now = std::chrono::steady_clock::now();
		if (!m_Model.ListingDirty && m_Model.ListingPath == m_Model.Current && std::chrono::duration<double>(now - m_Model.LastListingCheck).count() < 0.5)
			return;
		m_Model.LastListingCheck = now;

		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Model.Current, stampError);
		if (!m_Model.ListingDirty && m_Model.ListingPath == m_Model.Current && !stampError && stamp == m_Model.ListingStamp)
			return;

		m_Model.Listing.clear();
		std::error_code listError;
		for (const auto& entry : std::filesystem::directory_iterator(m_Model.Current, std::filesystem::directory_options::skip_permission_denied, listError))
		{
			if (listError)
				break;
			m_Model.Listing.push_back(entry.path());
		}
		std::sort(m_Model.Listing.begin(), m_Model.Listing.end());
		m_Model.ListingPath = m_Model.Current;
		m_Model.ListingStamp = stamp;
		m_Model.ListingDirty = false;
	}

	uintmax_t ContentBrowserPanel::FileSize(const std::filesystem::path& path)
	{
		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(path, stampError);
		const auto cached = m_Model.SizeCache.find(path);
		if (!stampError && cached != m_Model.SizeCache.end() && cached->second.first == stamp)
			return cached->second.second;
		std::error_code sizeError;
		const uintmax_t size = std::filesystem::file_size(path, sizeError);
		const uintmax_t result = sizeError ? 0 : size;
		m_Model.SizeCache[path] = { stamp, result };
		return result;
	}

	void ContentBrowserPanel::InvalidateContents()
	{
		m_Model.DirTreeDirty = true;
		m_Model.ListingDirty = true;
		m_Model.TreeStamp = {};
		m_Model.ListingStamp = {};
	}

	void ContentBrowserPanel::SaveState()
	{
		try
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			root.Object.push_back({ "listMode", Wui::JsonValue::MakeBool(m_Model.ListMode) });
			root.Object.push_back({ "current", Wui::JsonValue::MakeString(m_Model.Current.lexically_relative(m_Model.Root).generic_string()) });
			root.Object.push_back({ "treeScroll", Wui::JsonValue::MakeNumber(m_Model.TreeScroll) });
			root.Object.push_back({ "contentScroll", Wui::JsonValue::MakeNumber(m_Model.ContentScroll) });
			Wui::JsonValue open;
			open.type = Wui::JsonValue::Type::Array;
			for (const auto& path : m_Model.TreeOpen)
				open.Array.push_back(Wui::JsonValue::MakeString(path.lexically_relative(m_Model.Root).generic_string()));
			root.Object.push_back({ "treeOpen", std::move(open) });
			std::ofstream stream(m_StatePath, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << root.Dump();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to save content browser state: {0}", error.what());
		}
	}

	void ContentBrowserPanel::LoadState()
	{
		if (!std::filesystem::exists(m_StatePath))
			return;
		try
		{
			std::ifstream stream(m_StatePath, std::ios::binary);
			if (!stream)
				return;
			std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			std::string error;
			const auto parsed = Wui::JsonValue::Parse(text, &error);
			if (!parsed)
				return;
			if (const Wui::JsonValue* value = parsed->Find("listMode"))
				m_Model.ListMode = value->AsBool(false);
			if (const Wui::JsonValue* value = parsed->Find("current"))
			{
				const std::string relative = value->AsString("");
				if (!relative.empty())
				{
					const std::filesystem::path target = m_Model.Root / std::filesystem::path(relative);
					std::error_code dirError;
					if (std::filesystem::is_directory(target, dirError))
						m_Model.Current = target;
				}
			}
			if (const Wui::JsonValue* value = parsed->Find("treeScroll"))
				m_Model.TreeScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("contentScroll"))
				m_Model.ContentScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("treeOpen"))
			{
				m_Model.TreeOpen.clear();
				for (const auto& item : value->Array)
				{
					const std::string relative = item.AsString("");
					if (!relative.empty())
						m_Model.TreeOpen.insert(m_Model.Root / std::filesystem::path(relative));
				}
			}
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to load content browser state: {0}", error.what());
		}
	}

	void ContentBrowserPanel::Navigate(const std::filesystem::path& path)
	{
		if (m_Model.HistoryIndex >= 0 && m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) && m_Model.History[m_Model.HistoryIndex] == path)
			return;
		if (m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1)
			m_Model.History.resize(m_Model.HistoryIndex + 1);
		m_Model.History.push_back(path);
		m_Model.HistoryIndex = static_cast<int>(m_Model.History.size()) - 1;
		m_Model.Current = path;
		m_Model.Selected.clear();
		m_Model.LastSelected.clear();
		Reveal(path);
		m_Model.ListingDirty = true;
		m_Model.ListingStamp = {};
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::Reveal(const std::filesystem::path& path)
	{
		std::filesystem::path current = path;
		while (current != m_Model.Root && !current.empty() && current.has_parent_path())
		{
			m_Model.TreeOpen.insert(current);
			const std::filesystem::path parent = current.parent_path();
			if (parent == current)
				break;
			current = parent;
		}
	}

	void ContentBrowserPanel::GoBack()
	{
		if (m_Model.HistoryIndex > 0)
		{
			--m_Model.HistoryIndex;
			m_Model.Current = m_Model.History[m_Model.HistoryIndex];
			m_Model.Selected.clear();
			m_Model.LastSelected.clear();
			Reveal(m_Model.Current);
			m_Model.ListingDirty = true;
			m_Model.ListingStamp = {};
			SaveState();
			if (m_Model.Search[0])
				UpdateSearch();
		}
	}

	void ContentBrowserPanel::GoUp()
	{
		if (m_Model.Current != m_Model.Root)
			Navigate(m_Model.Current.parent_path());
	}

	void ContentBrowserPanel::OpenItem(const std::filesystem::path& path)
	{
		if (std::filesystem::is_directory(path))
		{
			Navigate(path);
			return;
		}
		if (path.extension() == ".wd")
			m_Host.OpenScene(path);
		else if (path.extension() == ".wmat")
		{
			// D3:材质资产双击 → 材质编辑器(独立窗口)载入。
			// 面板/渲染侧都按"相对 Game/assets"的路径引用,这里转成同一约定。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenMaterialEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (path.extension() == ".lua" || path.extension() == ".luau")
		{
			// W9-2:脚本双击 → 内置脚本编辑器(与双击材质同一条路:逻辑路径相对 Game/assets,
			// 默认附加到主窗口;解析失败时面板自身显示只读 + 错误文本)。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenScriptEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (path.extension() == ".wmodel")
		{
			// P1b D5:模型双击 → **打开只读预览**(不改场景);要放进场景在预览里点按钮。
			// 用户反馈"多次打开会多次叠加":打开动作不应有场景副作用。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			const std::string logical = ec ? path.generic_string() : relative.generic_string();
			m_Host.OpenModelPreview(logical);
			WLD_CORE_INFO("[model] preview opened: {0}", logical);
		}
		else if (path.extension() == ".wprefab")
		{
			// P4-U13c:双击 = 打开 prefab **资产窗口**(看/管理:实体树 + 组件摘要 + 引用资产 +
			// 场景实例),与 .wmodel 预览窗口同款、默认附加到主窗口。
			// 真正要改资产走窗口里的 `Edit Prefab`(文档会话,那里才有 3D 视口与 gizmo)。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			const std::string logical = ec ? path.generic_string() : relative.generic_string();
			m_Host.OpenPrefabWindow(logical);
			WLD_CORE_INFO("[prefab] asset window opened: {0}", logical);
		}
		else if (path.extension() == ".gltf" || path.extension() == ".glb")
		{
			// P1b D5:glTF 双击 → 导入(.wmodel/.wmat/贴图)并**打开模型预览**(不实例化)。
			// 导入后立刻刷新列表,新产出的 .wmodel/.wmat 马上可见。
			std::string message;
			std::string logicalModel;
			// D10:导入到**当前文件夹**(用户 Q1=方案 A:导入物落在当前目录里,
			// 材质/贴图在它的 materials//textures/ 子目录)。
			std::error_code destError;
			const std::filesystem::path destRelative =
				std::filesystem::relative(m_Model.Current, m_Model.Root, destError);
			const std::string destination = destError ? std::string() : destRelative.generic_string();
			if (!m_Host.ImportModelFileTo(path.string(), destination, &message, &logicalModel))
				WLD_CORE_WARN("[model] import '{0}' failed: {1}", path.string(), message);
			else
			{
				WLD_CORE_INFO("[model] {0}", message);
				InvalidateContents();
				if (m_Model.Search[0])
					UpdateSearch();
				if (!logicalModel.empty())
				{
					// P4-U10:导入完成后**选中产物**(.wmodel)—— 让用户一眼看到"真正可引用的是它",
					// 而不是继续盯着 .gltf 源文件。
					SelectCreated(m_Model.Root / logicalModel, "import-model");
					m_Host.OpenModelPreview(logicalModel);
				}
			}
		}
		else
		{
			const std::string cmd = "start \"\" \"" + std::filesystem::absolute(path).string() + "\"";
			system(cmd.c_str());
		}
	}

	void ContentBrowserPanel::PasteInto(const std::filesystem::path& destination)
	{
		for (const auto& path : m_Model.Clipboard)
		{
			if (!std::filesystem::exists(path))
				continue;
			std::filesystem::path destPath = destination / path.filename();
			if (m_Model.ClipboardCut)
			{
				if (path != destPath)
				{
					std::filesystem::rename(path, destPath);
					if (m_Ctx) m_Ctx->RecordOp("browser", "move", path.filename().string(), "-> " + destination.string());
				}
			}
			else
			{
				std::string stem = path.stem().string();
				std::string ext = path.extension().string();
				int counter = 1;
				while (std::filesystem::exists(destPath))
					destPath = destination / (stem + "-Copy(" + std::to_string(counter++) + ")" + ext);
				std::filesystem::copy(path, destPath, std::filesystem::copy_options::recursive);
				if (m_Ctx) m_Ctx->RecordOp("browser", "copy", path.filename().string(), "-> " + destPath.string());
			}
		}
		if (m_Model.ClipboardCut)
		{
			m_Model.Clipboard.clear();
			m_Model.ClipboardCut = false;
		}
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::DeleteSelection()
	{
		// 先快照目标:父目录删除时已覆盖其子项,跳过冗余 remove_all。
		const std::vector<std::filesystem::path> targets(m_Model.Selected.begin(), m_Model.Selected.end());
		const size_t count = targets.size();
		bool removedCurrent = false;
		for (const auto& path : targets)
		{
			if (m_Model.Current == path || IsWithinOrEqual(m_Model.Current, path))
				removedCurrent = true;
			const bool covered = std::any_of(targets.begin(), targets.end(),
				[&](const std::filesystem::path& other) { return other != path && IsWithinOrEqual(path, other); });
			if (covered)
				continue;
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
			if (ec)
				WLD_CORE_WARN("Content browser delete failed for '{0}': {1}", path.string(), ec.message());
		}

		// 清理指向已删除路径的模型状态,避免残留导航、展开与拖放目标。
		const auto referenced = [&](const std::filesystem::path& entry)
		{
			return std::any_of(targets.begin(), targets.end(),
				[&](const std::filesystem::path& target) { return IsWithinOrEqual(entry, target); });
		};
		for (auto it = m_Model.TreeOpen.begin(); it != m_Model.TreeOpen.end();)
		{
			if (referenced(*it)) it = m_Model.TreeOpen.erase(it);
			else ++it;
		}
		m_Model.History.erase(std::remove_if(m_Model.History.begin(), m_Model.History.end(), referenced), m_Model.History.end());
		m_Model.HistoryIndex = static_cast<int>(m_Model.History.size()) - 1;
		m_Model.Clipboard.erase(std::remove_if(m_Model.Clipboard.begin(), m_Model.Clipboard.end(), referenced), m_Model.Clipboard.end());
		if (!m_Model.RenameTarget.empty() && referenced(m_Model.RenameTarget))
		{
			m_Model.RenameTarget.clear();
			m_Model.RenameEdit.clear();
			m_Model.RenameActive = false;
			m_TreeRenameTarget.clear();
		}
		if (!m_Model.PendingDropDest.empty() && referenced(m_Model.PendingDropDest))
			m_Model.PendingDropDest.clear();

		m_Model.Selected.clear();
		m_Model.LastSelected.clear();
		if (removedCurrent)
			Navigate(m_Model.Root);
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
		if (m_Ctx) m_Ctx->RecordOp("browser", "delete", std::to_string(count), "");
	}

	void ContentBrowserPanel::CreateFolder(Wui::WuiContext& ctx)
	{
		CreateFolderIn(ctx, m_Model.Current);
	}

	std::filesystem::path ContentBrowserPanel::CreateFolderIn(Wui::WuiContext& ctx, const std::filesystem::path& parentDir)
	{
		std::filesystem::path newPath = parentDir / "New Folder";
		int counter = 1;
		while (std::filesystem::exists(newPath))
			newPath = parentDir / ("New Folder (" + std::to_string(counter++) + ")");
		try
		{
			std::filesystem::create_directory(newPath);
			m_Model.Selected.clear();
			m_Model.Selected.insert(newPath);
			m_Model.LastSelected = newPath;
			InvalidateContents();
			SaveState();
			if (m_Ctx) m_Ctx->RecordOp("browser", "mkdir", newPath.filename().string(), "");
			StartRename(ctx, newPath);
			return newPath;
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Could not create directory: {0}", error.what());
			return {};
		}
	}

	// ---- P4-UX16:"新建资产"注册表 ----
	// 用户 2026-09-20:「不能每加一个类似的资产就新增一个按钮」。
	// 这里把"有哪些资产类型、怎么创建"收敛成**一次注册**;菜单/右键菜单/快捷键只读注册表,
	// 以后加 Model / Texture 等类型 → 再 Register 一条,内容浏览器的 UI 代码不动。
	// 生命周期:Create 回调捕获 this,析构必须反注册(见 ~ContentBrowserPanel)。
	void ContentBrowserPanel::RegisterDefaultAssetTypes()
	{
		if (m_AssetTypesRegistered)
			return;
		m_AssetTypesRegistered = true;

		AssetTypeRegistry& registry = AssetTypeRegistry::Get();
		auto add = [&registry](const char* id, const char* label, const char* extension, int order,
			bool isFolder, std::function<bool(const std::filesystem::path&, std::string*)> create)
		{
			AssetTypeDesc desc;
			desc.Id = id;
			desc.Label = label;
			desc.Extension = extension;
			desc.SortOrder = order;
			desc.IsFolder = isFolder;
			desc.Create = std::move(create);
			registry.Register(std::move(desc));
		};

		// Folder:唯一"建完立刻改名"的类型(与资源管理器同款手感,沿用 D10-6 的既有路径)。
		add("folder", "Folder", "", 0, true,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				if (!m_Ctx)
				{
					if (error) *error = "content browser has no active UI context";
					return false;
				}
				if (CreateFolderIn(*m_Ctx, dir).empty())
				{
					if (error) *error = "could not create a folder under " + dir.string();
					return false;
				}
				return true;
			});
		// Material(U25-M2 起):走**新建向导** —— 模板 / 名称 / 目录 / 实时落点全部在确认前可见,
		// 创建成功后由向导自己选中新资产并在材质编辑器里打开它。这里只负责把向导排进下一帧
		// (注册表回调也可能从快捷键路径进来,那时没有 WuiContext)。
		add("material", "Material", ".wmat", 10, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				(void)error;
				m_NewMaterialPendingDir = dir;
				m_NewMaterialPendingOpen = true;
				m_NewMaterialPendingFromSelection = false;
				return true;
			});
		// Scene:只创建 + 选中,**不自动打开** —— 打开会直接替换当前文档
		// (EditorLayer::DoOpenScene 不拦未保存改动),"新建资产"不该顺带丢掉用户正在编辑的场景。
		add("scene", "Scene", ".wd", 20, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				std::filesystem::path created;
				return CreateSceneAsset(dir, error, &created);
			});
		// Script:从 templates/WorldScript.lua 复制(与 Scripts 面板"新建脚本"同一份模板),
		// 随后在脚本编辑器里打开 —— 脚本面板不动文档,没有上面那条顾虑。
		add("script", "Script", ".lua", 30, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				std::filesystem::path created;
				if (!CreateScriptAsset(dir, error, &created))
					return false;
				std::error_code relativeError;
				const std::filesystem::path relative =
					std::filesystem::relative(created, m_Model.Root, relativeError);
				m_Host.OpenScriptEditor(relativeError ? created.generic_string() : relative.generic_string());
				return true;
			});
	}

	void ContentBrowserPanel::UnregisterDefaultAssetTypes()
	{
		if (!m_AssetTypesRegistered)
			return;
		m_AssetTypesRegistered = false;
		AssetTypeRegistry& registry = AssetTypeRegistry::Get();
		// 只撤销本面板注册的 id:别的组件(宿主/插件/测试)注册的类型不受影响。
		for (const char* id : { "folder", "material", "scene", "script" })
			registry.Unregister(id);
	}

	bool ContentBrowserPanel::CreateAssetFromRegistry(const std::string& typeId, std::string* error)
	{
		const AssetTypeDesc* desc = AssetTypeRegistry::Get().Find(typeId);
		if (!desc || !desc->Create)
		{
			if (error) *error = "asset type is not registered: " + typeId;
			return false;
		}
		std::error_code dirError;
		if (!std::filesystem::is_directory(m_Model.Current, dirError))
		{
			if (error) *error = "target folder does not exist: " + m_Model.Current.string();
			return false;
		}
		std::string localError;
		if (!desc->Create(m_Model.Current, &localError))
		{
			if (error) *error = localError.empty() ? ("could not create " + typeId) : localError;
			return false;
		}
		return true;
	}

	void ContentBrowserPanel::SelectCreated(const std::filesystem::path& path, const char* op)
	{
		m_Model.Selected.clear();
		m_Model.Selected.insert(path);
		m_Model.LastSelected = path;
		InvalidateContents();
		SaveState();
		if (m_Ctx && op)
			m_Ctx->RecordOp("browser", op, path.filename().string(), "");
	}

	// P4-U13d:按逻辑路径选中一个已存在的资产(创建预制体成功后由宿主调用)。
	// 选中的路径存的是**绝对路径**(与 SelectCreated / 内容区切片一致);目标不在当前目录时
	// 先导航过去(导航会清选中,所以选中必须放在导航之后)。
	bool ContentBrowserPanel::SelectAsset(const std::string& logicalPath, const char* op)
	{
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return false;
		const std::filesystem::path absolute = m_Model.Root / std::filesystem::path(normalized);
		std::error_code ec;
		if (!std::filesystem::exists(absolute, ec))
			return false;
		if (absolute.parent_path() != m_Model.Current)
			Navigate(absolute.parent_path());
		SelectCreated(absolute, op);
		return true;
	}

	bool ContentBrowserPanel::CreateMaterialAsset(const std::filesystem::path& dir, std::string* error,
		std::filesystem::path* outPath)
	{
		// 写一份默认 .wmat 模板(重名自动编号)。
		// 关键:MateriaIO 的路径解析是 `<Game>/assets/<path>` 存在就用它,否则回退 `<Game>/<path>` ——
		// 相对逻辑路径**新建**时会落到 `Game/<path>`(实测:内容浏览器新建材质写进了 Game/_ux_probe)。
		// 绝对路径两种情况都原样命中,所以这里传绝对路径(旧 CreateMaterial 的同一个坑)。
		const std::filesystem::path target = MakeUniqueAssetPath(dir, "material", ".wmat");
		MaterialDesc desc;
		desc.Name = target.stem().string();
		std::string localError;
		if (!MaterialIO::WriteFileText(target.generic_string(), MaterialIO::Serialize(desc), &localError))
		{
			WLD_CORE_ERROR("Could not create material: {0}", localError);
			if (error) *error = localError;
			return false;
		}
		SelectCreated(target, "new-material");
		if (outPath) *outPath = target;
		return true;
	}

	bool ContentBrowserPanel::CreateSceneAsset(const std::filesystem::path& dir, std::string* error,
		std::filesystem::path* outPath)
	{
		// 用引擎自己的 SceneSerializer 写"空场景",而不是手写模板字符串:
		// 产物与编辑器另存出来的场景同格式(FormatVersion/Entities),格式演进时不会两处漂移。
		const Ref<Scene> active = m_Host.GetActiveScene();
		if (!active)
		{
			if (error) *error = "no active scene to derive a world context from";
			return false;
		}
		const std::filesystem::path target = MakeUniqueAssetPath(dir, "scene", ".wd");
		SceneSerializer serializer(CreateRef<Scene>(active->GetContext()));
		if (!serializer.Serialize(target.string()))
		{
			const std::string localError = serializer.GetLastError().empty()
				? ("could not write " + target.string()) : serializer.GetLastError();
			WLD_CORE_ERROR("Could not create scene: {0}", localError);
			if (error) *error = localError;
			return false;
		}
		SelectCreated(target, "new-scene");
		if (outPath) *outPath = target;
		return true;
	}

	bool ContentBrowserPanel::CreateScriptAsset(const std::filesystem::path& dir, std::string* error,
		std::filesystem::path* outPath)
	{
		// 与 Scripts 面板"从模板新建"同一份磁盘模板;模板缺失时退回最小骨架(不阻断新建)。
		const std::filesystem::path target = MakeUniqueAssetPath(dir, "script", ".lua");
		const std::filesystem::path templatePath = m_Model.Root / "scripts" / "templates" / "WorldScript.lua";
		std::error_code templateError;
		if (std::filesystem::is_regular_file(templatePath, templateError))
		{
			std::error_code copyError;
			std::filesystem::copy_file(templatePath, target, std::filesystem::copy_options::none, copyError);
			if (copyError)
			{
				WLD_CORE_ERROR("Could not create script: {0}", copyError.message());
				if (error) *error = copyError.message();
				return false;
			}
		}
		else
		{
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
			{
				if (error) *error = "could not write " + target.string();
				return false;
			}
			out << "---@class NewScript : WorldScript\nlocal NewScript = {}\n\n"
				"function NewScript:OnCreate()\nend\n\n"
				"function NewScript:OnUpdate(dt)\nend\n\n"
				"function NewScript:OnDestroy()\nend\n\nreturn NewScript\n";
		}
		SelectCreated(target, "new-script");
		if (outPath) *outPath = target;
		return true;
	}

	bool ContentBrowserPanel::RenderNewAssetRow(Wui::WuiContext& ctx, Wui::WuiId rowId,
		const Wui::WuiRect& row, const Wui::WuiTheme& theme)
	{
		// 子菜单指示三角用 ▶(U+25B6):字体子集里验证过的字形(树用 ▼/▶);
		// "▸"(U+25B8)与"⋯"(U+22EF)一样不在子集里,会画成乱码。
		const std::string label = Wui::Tr("panel.content_browser.menu.new", "New") + "  ▶";
		return Wui::MenuItem(ctx, rowId, row, label, true, theme);
	}

	Wui::WuiRect ContentBrowserPanel::RenderNewAssetItems(Wui::WuiContext& ctx, const char* idPrefix,
		const Wui::WuiRect& parentMenu, const Wui::WuiRect& clampArea, const Wui::WuiTheme& theme)
	{
		// 清单 = 资产类型注册表(排序在注册表里定:Folder 恒第一 → SortOrder → Id)。
		// 这里**没有任何按类型分支** —— 新类型注册进来就自动出现在菜单里。
		const std::vector<AssetTypeDesc> types = AssetTypeRegistry::Get().Sorted();
		if (types.empty())
			return {};

		const float itemH = 22.0f;
		Wui::WuiRect panel { parentMenu.X + parentMenu.W - 4.0f, parentMenu.Y + 4.0f, 236.0f,
			itemH * static_cast<float>(types.size()) + 8.0f };
		// 贴边翻转:右侧放不下就摆到父菜单左侧;再夹进面板可视区(子菜单永远不出画面)。
		if (panel.X + panel.W > clampArea.X + clampArea.W - 4.0f)
			panel.X = parentMenu.X - panel.W + 4.0f;
		panel.X = std::max(clampArea.X + 4.0f,
			std::min(panel.X, clampArea.X + clampArea.W - panel.W - 4.0f));
		panel.Y = std::max(clampArea.Y + 4.0f,
			std::min(panel.Y, clampArea.Y + clampArea.H - panel.H - 4.0f));
		Wui::DrawPanelSurface(ctx, panel, theme);
		// P4-U7:子菜单往父菜单右侧伸出,父菜单矩形的遮挡区盖不住它 —— 子菜单矩形必须
		// 自己登记(下一帧树/切片不会吃掉落在子菜单上的点击)。
		ctx.RegisterOverlayRect(panel);

		const std::string tooltip = Wui::Tr("panel.content_browser.new.tooltip", "Create in the current folder");
		for (size_t index = 0; index < types.size(); ++index)
		{
			const AssetTypeDesc& desc = types[index];
			const Wui::WuiRect row { panel.X + 4.0f, panel.Y + 4.0f + itemH * static_cast<float>(index),
				panel.W - 8.0f, itemH };
			const std::string label = Wui::Tr(("asset.type." + desc.Id).c_str(), desc.Label.c_str());
			const Wui::WuiId itemId = Wui::HashId((std::string(idPrefix) + desc.Id).c_str());
			const bool clicked = Wui::MenuItem(ctx, itemId, row, label, true, theme);
			// MenuItem 登记的无障碍节点只有"文字标签";这里用同一个 id 再登记一次(后写覆盖),
			// 把**扩展名**与**创建位置**写进 Value/Tooltip —— 右侧那行扩展名提示是画出来的,
			// 读屏与脚本读不到,必须同时进节点(用户 2026-09-18:「引擎的 UI 对 AI 要无障碍」)。
			{
				Wui::WuiAccessNode node;
				node.Id = itemId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "menu-item";
				node.Label = label;
				// 文件夹没有扩展名 → 值写明 "(folder)",脚本据此区分"目录"与"文件类型"。
				node.Value = desc.Extension.empty() ? "(folder)" : desc.Extension;
				node.Tooltip = tooltip;
				node.Rect = row;
				Wui::WuiAccessibility::Get().Register(node);
			}
			if (clicked)
			{
				std::string error;
				if (!CreateAssetFromRegistry(desc.Id, &error))
					NotifyAssetFailure(error);
				m_NewMenuOwner = 0;
				ctx.CloseAllPopups();
				break;
			}
			// 右侧扩展名提示(材料 → .wmat / 场景 → .wd / 脚本 → .lua),一眼可辨且与"另存为"同名。
			if (!desc.Extension.empty())
			{
				const float textWidth = ctx.MeasureTextWidth(desc.Extension, 12.0f);
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ row.X + row.W - textWidth - 8.0f, row.Y + (row.H - 15.0f) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, desc.Extension, 12.0f, false });
			}
			Wui::Tooltip(ctx, row, tooltip);
		}
		return panel;
	}

	void ContentBrowserPanel::NotifyAssetFailure(const std::string& error)
	{
		const std::string text = Wui::Tr("panel.content_browser.new.failed", "Could not create asset: ") + error;
		WLD_CORE_ERROR("[content-browser] {0}", text);
		m_Host.Notify(text);
	}

	bool ContentBrowserPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
	{
		(void)alt;
		// P4-UX16:Ctrl+N = 打开"新建"清单;Ctrl+Shift+N = 新建文件夹(资源管理器同款)。
		// 本函数是三层路由的第 2 层:内容浏览器**有焦点**时优先于引擎全局 Ctrl+N
		// (File ▸ New Scene);没焦点时那条全局命令照旧生效,不抢别处。
		if (!ctrl || keyCode != KeyCodes::N)
			return false;
		// 快捷键在渲染之外到达(拿不到 ctx),只排队;真正的动作在下一帧 OnRender 里做。
		m_PendingNewShortcut = shift ? 2 : 1;
		return true;
	}

	void ContentBrowserPanel::ApplyRename(const std::filesystem::path& target, const std::string& newName)
	{
		if (newName.empty())
		{
			m_Model.RenameTarget.clear();
			m_Model.RenameActive = false;
			m_TreeRenameTarget.clear();
			return;
		}
		const std::filesystem::path newPath = target.parent_path() / newName;
		if (newPath != target)
		{
			std::error_code ignored;
			std::filesystem::rename(target, newPath, ignored);
			if (m_Ctx) m_Ctx->RecordOp("browser", "rename", target.filename().string(), "-> " + newPath.filename().string());
		}
		m_Model.RenameTarget.clear();
		m_Model.RenameActive = false;
		m_TreeRenameTarget.clear();
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::StartRename(Wui::WuiContext& ctx, const std::filesystem::path& path)
	{
		m_Model.RenameTarget = path;
		// P4-UX15:重命名默认只编辑**主名**,后缀单独保留 —— 与资源管理器一致
		// (用户:"重命名时还是会显示后缀名")。提交时若用户没写后缀,自动补回原名后缀。
		m_Model.RenameExtension = path.extension().string();
		m_Model.RenameEdit = m_Model.RenameExtension.empty() ? path.filename().string() : path.stem().string();
		std::strncpy(m_Model.RenameBuffer, m_Model.RenameEdit.c_str(), sizeof(m_Model.RenameBuffer) - 1);
		m_Model.RenameActive = true;
		ctx.SetFocus(Wui::HashId("browser.rename"));
		ctx.SetTextInputActive(true);
	}

	void ContentBrowserPanel::RenderRenameField(Wui::WuiContext& ctx, const std::filesystem::path& path, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const Wui::WuiId renameId = Wui::HashId("browser.rename");
		// U2d:输入非法(空 / 非法字符 / 同目录重名)时用 TextFieldEx 行内显示原因并拒绝提交。
		// TextFieldEx 没有 cancelled 回调,所以 Esc 取消在这里先于控件处理(否则会把"取消"
		// 当成失焦提交,把非法名字写进 ApplyRename —— 语义就变了)。
		const bool cancelRequested = ctx.Focus() == renameId && ctx.IsKeyPressed(KeyCodes::Escape);
		const std::string error = RenameErrorFor(path, m_Model.RenameEdit);
		const bool submitted = Wui::TextFieldEx(ctx, renameId, rect, m_Model.RenameEdit, theme, error);
		if (cancelRequested)
		{
			// Escape 丢弃(与旧 TextField 的 cancelled 语义相同)。
			m_Model.RenameTarget.clear();
			m_Model.RenameEdit.clear();
			m_Model.RenameActive = false;
			m_TreeRenameTarget.clear();
		}
		else if (submitted)
		{
			// 回车提交:非法名字拒绝提交。TextFieldCore 在提交帧会自己失焦,这里把焦点还给
			// 输入框 —— 否则下一帧就按"失焦提交"把框收掉,错误只闪一帧、也没法继续改。
			if (error.empty())
				ApplyRename(path, RenameNameWithExtension(path, m_Model.RenameEdit));
			else
			{
				WLD_CORE_WARN("[browser] rename rejected: {0}", error);
				ctx.SetFocus(renameId);
				ctx.SetTextInputActive(true);
			}
		}
		else if (m_Model.RenameActive && ctx.Focus() != renameId)
		{
			// 失焦提交:点选其他条目或空白处时收起重命名框。非法名字不提交(原名字不变,
			// 也不留一个失去焦点、无法继续编辑的输入框)。
			if (error.empty())
				ApplyRename(path, RenameNameWithExtension(path, m_Model.RenameEdit));
			else
			{
				WLD_CORE_WARN("[browser] rename rejected on blur: {0}", error);
				m_Model.RenameTarget.clear();
				m_Model.RenameEdit.clear();
				m_Model.RenameActive = false;
				m_TreeRenameTarget.clear();
			}
		}
	}

	void ContentBrowserPanel::OpenInExplorer(const std::filesystem::path& path)
	{
		// /select 让资源管理器打开所在文件夹并选中该项,而不是直接打开文件。
		const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) <= 32)
			WLD_CORE_WARN("Could not open Explorer for '{0}' (error {1})", path.string(), reinterpret_cast<intptr_t>(result));
	}

	void ContentBrowserPanel::OpenFolderInExplorer(const std::filesystem::path& path)
	{
		// D10-6:直接打开该目录本身(面板既有的系统调用风格,见 OpenItem 的兜底分支)。
		const std::string cmd = "start \"\" explorer.exe \"" + std::filesystem::absolute(path).string() + "\"";
		system(cmd.c_str());
	}

	void ContentBrowserPanel::Cut()
	{
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = true;
	}

	void ContentBrowserPanel::Copy()
	{
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = false;
	}

	void ContentBrowserPanel::SelectAll(const std::vector<std::filesystem::path>& paths)
	{
		m_Model.Selected.clear();
		m_Model.Selected.insert(paths.begin(), paths.end());
		if (!paths.empty())
			m_Model.LastSelected = paths.back();
	}

	// D10-10(用户 2026-09-19):导入位置选择器已搬到 EditorShell 的窗口级模态(居中 + 全窗口挡输入)。
	// 面板这里只保留 shell 需要的刷新入口(与工具栏 Refresh / 内容区菜单同一条路径)。
	void ContentBrowserPanel::RefreshContents()
	{
		InvalidateContents();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	// ---- P4-UX14:内容区统一切片(网格)----
	// 每格 = 图标 → 名称(居中、超宽省略号)→ 次级信息(文件夹 / 扩展名 · 大小)。
	// 悬停 = BorderStrong 描边 + 图标轻微放大(≤4%);选中 = 2px Accent 描边 + 名称 Selection 底。
	// 尺寸:切片最小 96×96,列数随面板宽度自适应,间距一律 theme.Pad。
	void ContentBrowserPanel::RenderGridSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area,
		const Wui::WuiTheme& theme, const std::vector<BrowserSlice>& slices,
		const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact,
		bool treeRenameDrawn)
	{
		const float gap = theme.Pad;
		const int columns = std::max(1, static_cast<int>((area.W + gap) / (kSliceMinSize + gap)));
		const float cellW = std::max(kSliceMinSize,
			(area.W - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
		const float cellH = std::max(kSliceMinSize, cellW);
		const size_t rowCount = (slices.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns);
		const float contentHeight = gap + static_cast<float>(rowCount) * (cellH + gap);
		Wui::BeginScrollArea(ctx, area, contentHeight, m_Model.ContentScroll, theme);
		const float nameSize = theme.FontSizeBody;
		const float infoSize = theme.FontSizeCaption;
		const float textBudget = std::max(0.0f, cellW - gap * 2.0f);
		for (size_t i = 0; i < slices.size(); ++i)
		{
			const BrowserSlice& slice = slices[i];
			const int column = static_cast<int>(i % static_cast<size_t>(columns));
			const int rowIndex = static_cast<int>(i / static_cast<size_t>(columns));
			const Wui::WuiRect cell {
				area.X + (cellW + gap) * static_cast<float>(column),
				area.Y + gap + (cellH + gap) * static_cast<float>(rowIndex) - m_Model.ContentScroll,
				cellW, cellH };
			if (cell.Y + cell.H < area.Y || cell.Y > area.Y + area.H)
				continue; // 视野外:不绘制也不命中(与旧 GridView 相同)
			// U25-M2:切片进无障碍树 —— 脚本按 "browser.slice.<逻辑路径>" 拿到矩形
			// (拖放到材质编辑器的贴图槽要按住一个具体的 .png 切片)。
			RegisterSliceNode(slice, cell);
			const bool selected = m_Model.Selected.find(slice.Path) != m_Model.Selected.end();
			const bool hovered = ctx.IsHovered(cell);

			// 图标:悬停时绕自身中心轻微放大,名称/信息的基线不动。
			const float iconSize = kSliceIconSize * (hovered ? kSliceHoverIconScale : 1.0f);
			const uint64_t icon = slice.IsDir ? m_DirIconId : m_FileIconId;
			if (icon != 0)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Image,
					{ cell.X + cell.W * 0.5f - iconSize * 0.5f,
					  cell.Y + gap - (iconSize - kSliceIconSize) * 0.5f, iconSize, iconSize },
					Wui::WuiColor { 1, 1, 1, 1 }, 0.0f, 1.0f, "", nameSize, false, icon, { 0, 1, 1, -1 } });

			// P4-U13:prefab 是"可复用实体子树",不是普通文件 —— 给一枚常驻小标签,
			// 让它在网格里一眼可辨(文件图标/名称都看不出它是资产还是随便一个文件)。
			if (slice.Extension == ".wprefab")
			{
				const Wui::LocalizedLabel badge = Wui::TrLabel("asset.type.prefab", "Prefab");
				const float badgeSize = infoSize;
				const float badgeW = ctx.MeasureTextWidth(badge.Text, badgeSize) + theme.PadSmall * 2.0f;
				const float badgeH = badgeSize + 4.0f;
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
					{ cell.X + gap, cell.Y + gap, badgeW, badgeH }, theme.Accent, badgeH * 0.5f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ cell.X + gap + theme.PadSmall, cell.Y + gap + 2.0f, 0.0f, 0.0f },
					Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, 0.0f, 1.0f, badge.Text, badgeSize, true });
			}

			// 名称(居中;选中时先铺一层 Selection 底再画文字)。
			const float infoY = cell.Y + cell.H - gap - infoSize;
			const float nameY = infoY - theme.PadSmall - nameSize;
			const std::string nameText = EllipsizeToWidth(ctx, slice.Name, textBudget, nameSize);
			const float nameW = ctx.MeasureTextWidth(nameText, nameSize);
			const float nameX = cell.X + (cell.W - nameW) * 0.5f;
			if (selected)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
					{ nameX - theme.PadSmall, nameY - 1.0f, nameW + theme.PadSmall * 2.0f, nameSize + 4.0f },
					theme.Selection, theme.Radius });
			if (!nameText.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { nameX, nameY, 0.0f, 0.0f },
					theme.Text, 0.0f, 1.0f, nameText, nameSize, false });

			// 次级信息:文件夹 = "文件夹";文件 = 扩展名 · 大小。
			// 大小按需 stat,只统计**可见**切片(大目录在网格模式下不再每帧全量 stat)。
			const uintmax_t bytes = slice.IsDir ? 0 : (slice.SizeKnown ? slice.Size : FileSize(slice.Path));
			const std::string info = slice.IsDir
				? Wui::Tr("panel.content_browser.slice.folder", "Folder")
				// glTF/GLB 用"导入源"而不是裸扩展名:它们在网格视图里必须一眼看出不是可引用资产。
				// prefab 同理:显示类型名而不是 ".wprefab"。
				: ((slice.Extension == ".gltf" || slice.Extension == ".glb" || slice.Extension == ".wprefab")
					? slice.TypeLabel
					: (slice.Extension.empty() ? slice.TypeLabel : slice.Extension)) + " · "
					+ FormatBytes(static_cast<size_t>(bytes));
			const std::string infoText = EllipsizeToWidth(ctx, info, textBudget, infoSize);
			if (!infoText.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ cell.X + (cell.W - ctx.MeasureTextWidth(infoText, infoSize)) * 0.5f, infoY, 0.0f, 0.0f },
					theme.TextMuted, 0.0f, 1.0f, infoText, infoSize, false });

			// 描边:常驻 1px Border → 悬停 BorderStrong → 选中 2px Accent(选中 + 悬停再提亮一档)。
			Wui::WuiColor stroke = theme.Border;
			float thickness = 1.0f;
			if (selected)
			{
				stroke = hovered
					? MixColor(theme.Accent, Wui::WuiColor { 1, 1, 1, 1 }, kSelectedHoverLighten)
					: theme.Accent;
				thickness = 2.0f;
			}
			else if (hovered)
				stroke = theme.BorderStrong;
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, cell, stroke, theme.Radius, thickness });

			interact(slice.Path, cell, slice.IsDir);
			// D10-6:从树菜单发起的重命名画在树行上;内容区这一份画在名称位置上。
			if (!treeRenameDrawn && m_Model.RenameTarget == slice.Path)
				RenderRenameField(ctx, slice.Path,
					{ cell.X + gap, nameY - 1.0f, textBudget, nameSize + 4.0f }, theme);
		}
		Wui::EndScrollArea(ctx);
	}

	// ---- P4-UX14:内容区统一切片(列表 + 常驻表头)----
	// 表头用 Wui::TableHeader(列节点由控件自己登记,点击列头排序);
	// 数据行 = 图标(16px) + 名称 + 类型 + 大小,行高 22;悬停 HoverBg,
	// 选中 = Selection 底 + 左侧 2px Accent 条(与树一致)。
	void ContentBrowserPanel::RenderListSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area,
		const Wui::WuiTheme& theme, const std::vector<BrowserSlice>& slices, int& sortColumn, bool& sortAscending,
		const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact,
		bool treeRenameDrawn)
	{
		// 列宽:名称列吃掉剩余宽度;面板很窄时先压类型/大小列(名称列保底 theme.Pad * 10)。
		const float typeW = std::min(kListTypeColumnWidth, std::max(theme.Pad * 4.0f, area.W * kListColumnShare));
		const float sizeW = std::min(kListSizeColumnWidth, std::max(theme.Pad * 4.0f, area.W * kListColumnShare));
		const float nameW = std::max(theme.Pad * 10.0f, area.W - typeW - sizeW);
		const std::vector<std::string> columns {
			Wui::Tr("panel.content_browser.column.name", "Name"),
			Wui::Tr("panel.content_browser.column.type", "Type"),
			Wui::Tr("panel.content_browser.column.size", "Size") };
		const std::vector<float> columnWidths { nameW, typeW, sizeW };
		Wui::TableHeader(ctx, Wui::HashId("browser.list.header"),
			{ area.X, area.Y, area.W, kTableHeaderHeight }, columns, columnWidths, sortColumn, sortAscending, theme);

		const Wui::WuiRect body { area.X, area.Y + kTableHeaderHeight, area.W,
			std::max(0.0f, area.H - kTableHeaderHeight) };
		const float contentHeight = theme.PadSmall * 2.0f + kListRowHeight * static_cast<float>(slices.size());
		Wui::BeginScrollArea(ctx, body, contentHeight, m_Model.ContentScroll, theme);
		const float typeX = body.X + nameW;
		const float sizeX = typeX + typeW;
		const float smallSize = theme.FontSizeSmall;
		for (size_t i = 0; i < slices.size(); ++i)
		{
			const BrowserSlice& slice = slices[i];
			const Wui::WuiRect row { body.X,
				body.Y + theme.PadSmall + kListRowHeight * static_cast<float>(i) - m_Model.ContentScroll,
				body.W, kListRowHeight };
			if (row.Y + row.H < body.Y || row.Y > body.Y + body.H)
				continue;
			RegisterSliceNode(slice, row);
			const bool selected = m_Model.Selected.find(slice.Path) != m_Model.Selected.end();
			const bool hovered = ctx.IsHovered(row);
			if (selected)
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.Selection, theme.Radius });
				// 左侧 2px 强调条:与树的选中语言一致(行内上下各留 1px)。
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
					{ row.X, row.Y + 1.0f, 2.0f, row.H - 2.0f }, theme.Accent, 1.0f });
			}
			else if (hovered)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.HoverBg, theme.Radius });

			const uint64_t icon = slice.IsDir ? m_DirIconId : m_FileIconId;
			if (icon != 0)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Image,
					{ row.X + theme.PadSmall, row.Y + (row.H - kListIconSize) * 0.5f, kListIconSize, kListIconSize },
					Wui::WuiColor { 1, 1, 1, 1 }, 0.0f, 1.0f, "", smallSize, false, icon, { 0, 1, 1, -1 } });

			const float nameX = row.X + theme.PadSmall * 2.0f + kListIconSize;
			const float nameBudget = std::max(0.0f, nameW - (nameX - row.X) - theme.PadSmall);
			const std::string nameText = EllipsizeToWidth(ctx, slice.Name, nameBudget, theme.FontSizeBody);
			if (!nameText.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ nameX, row.Y + (row.H - theme.FontSizeBody) * 0.5f, 0.0f, 0.0f },
					theme.Text, 0.0f, 1.0f, nameText, theme.FontSizeBody, false });

			const std::string typeText = EllipsizeToWidth(ctx, slice.TypeLabel,
				std::max(0.0f, typeW - theme.PadSmall * 2.0f), smallSize);
			if (!typeText.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ typeX + theme.PadSmall, row.Y + (row.H - smallSize) * 0.5f, 0.0f, 0.0f },
					theme.TextMuted, 0.0f, 1.0f, typeText, smallSize, false });

			const std::string sizeText = slice.IsDir
				? std::string("-")
				: FormatBytes(static_cast<size_t>(slice.SizeKnown ? slice.Size : FileSize(slice.Path)));
			const std::string sizeShown = EllipsizeToWidth(ctx, sizeText,
				std::max(0.0f, sizeW - theme.PadSmall * 2.0f), smallSize);
			if (!sizeShown.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ sizeX + theme.PadSmall, row.Y + (row.H - smallSize) * 0.5f, 0.0f, 0.0f },
					theme.TextMuted, 0.0f, 1.0f, sizeShown, smallSize, false });

			interact(slice.Path, row, slice.IsDir);
			// D10-6:从树菜单发起的重命名画在树行上,内容区不再重复画同 id 输入框。
			if (!treeRenameDrawn && m_Model.RenameTarget == slice.Path)
				RenderRenameField(ctx, slice.Path, { nameX, row.Y + 1.0f, nameBudget, row.H - 2.0f }, theme);
		}
		Wui::EndScrollArea(ctx);
	}

	// ---- U25-M2:E 写材质的工作流(新建向导 + 从选中对象提取)----
	bool ContentBrowserPanel::OpenNewMaterialWizard(std::string* message)
	{
		m_NewMaterialPendingOpen = true;
		m_NewMaterialPendingFromSelection = false;
		if (message)
			*message = "new-material wizard queued";
		return true;
	}

	bool ContentBrowserPanel::OpenNewMaterialFromSelection(std::string* message)
	{
		const Entity selected = m_Host.GetSelectedEntity();
		if (!selected.IsValid())
		{
			if (message)
				*message = "no entity selected (select one in the Scene Hierarchy first)";
			return false;
		}
		m_NewMaterialPendingOpen = true;
		m_NewMaterialPendingFromSelection = true;
		if (message)
			*message = "extract-from-selection wizard queued";
		return true;
	}

	std::string ContentBrowserPanel::NewMaterialBaseName() const
	{
		return AssetBaseName(m_NewMaterialName);
	}

	std::string ContentBrowserPanel::NewMaterialTarget() const
	{
		const std::string name = NewMaterialBaseName();
		std::string folder;
		if (m_NewMaterialFolderIndex >= 0 && m_NewMaterialFolderIndex < static_cast<int>(m_NewMaterialFolders.size()))
			folder = m_NewMaterialFolders[static_cast<size_t>(m_NewMaterialFolderIndex)];
		std::string target = folder.empty() ? std::string() : (folder + "/");
		target += name.empty() ? std::string("(name)") : name;
		target += ".wmat";
		return target;
	}

	std::string ContentBrowserPanel::NewMaterialNameError() const
	{
		const std::string name = NewMaterialBaseName();
		if (name.empty())
			return Wui::Tr("panel.content_browser.new_material.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("panel.content_browser.new_material.name.dot", "Name cannot be '.' or '..'");
		for (const char character : name)
			if (character == '\\' || character == '/' || character == ':' || character == '*'
				|| character == '?' || character == '"' || character == '<' || character == '>'
				|| character == '|')
				return Wui::Tr("panel.content_browser.new_material.name.illegal",
					"Name cannot contain \\ / : * ? \" < > |");
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("panel.content_browser.new_material.name.trailing",
				"Name cannot end with a dot or a space");
		return {};
	}

	MaterialDesc ContentBrowserPanel::MaterialDescForTemplate(int templateIndex, const std::string& name,
		const MaterialDesc& seed)
	{
		// seed = Extract 读到的初值(普通新建 = MaterialDesc 默认值)。
		MaterialDesc desc = seed;
		desc.Name = name;
		switch (templateIndex)
		{
			case 1:   // Unlit-ish(近似:自发光 = 基色,金属度/粗糙度归零)
				desc.Emissive = glm::vec3(desc.BaseColor);
				desc.Metallic = 0.0f;
				desc.Roughness = 0.0f;
				break;
			case 2:   // Transparent(= 现有 blend 字段能表达的 alpha 混合)
				desc.BlendMode = MaterialBlendMode::Transparent;
				desc.DoubleSided = false;
				break;
			case 3:   // Additive(近似:Transparent + 自发光 = 基色 x 2;真正的 additive 值属 M3)
				desc.BlendMode = MaterialBlendMode::Transparent;
				desc.DoubleSided = false;
				desc.Emissive = glm::vec3(desc.BaseColor) * 2.0f;
				break;
			default:
				break;
		}
		return desc;
	}

	void ContentBrowserPanel::OpenNewMaterialModal(Wui::WuiContext& ctx, bool fromSelection)
	{
		m_NewMaterialOpen = true;
		m_NewMaterialOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewMaterialFailure.clear();
		m_NewMaterialFailureFor.clear();
		m_NewMaterialTemplate = 0;
		m_NewMaterialSeed = MaterialDesc {};
		m_NewMaterialAssignEntity = Entity {};
		m_NewMaterialFromSelection = fromSelection;
		// M3:Parent 下拉 = 引擎内置默认(下标 0)+ 内容根下所有已有 .wmat。
		m_NewMaterialParentPaths.clear();
		m_NewMaterialParentPaths.push_back(std::string());
		for (const std::string& path : MaterialLibrary::Get().ScanMaterials())
			m_NewMaterialParentPaths.push_back(path);
		m_NewMaterialParentIndex = 0;

		std::string defaultName;
		std::string defaultFolder;
		if (fromSelection)
		{
			// Extract from Selection:初值 = 选中实体 MeshRenderer 的当前材质;
			// 没有材质资产时用实体的 Color 当基色(用户看到的颜色就是初值)。
			Entity selected = m_Host.GetSelectedEntity();
			m_NewMaterialAssignEntity = selected;
			std::string entityName;
			const Ref<Scene> scene = m_Host.GetActiveScene();
			if (selected.IsValid() && scene)
			{
				const Scene& sceneRef = *scene;
				const entt::registry& registry = sceneRef.GetRegistry();
				const entt::entity handle = static_cast<entt::entity>(selected);
				if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
				{
					if (!mesh->MaterialPath.empty())
					{
						std::string loadError;
						if (const Ref<Material> source = MaterialLibrary::Get().Load(mesh->MaterialPath, &loadError))
							m_NewMaterialSeed = source->GetDesc();
						defaultFolder = std::filesystem::path(mesh->MaterialPath).parent_path().generic_string();
					}
					else
					{
						m_NewMaterialSeed.BaseColor = mesh->Color;
					}
				}
				if (selected.HasComponent<TagComponent>())
					entityName = selected.GetComponent<TagComponent>().Tag;
			}
			defaultName = SanitizeAssetName(entityName);
			if (defaultName.empty())
				defaultName = "material";
			defaultName += "_material";
		}
		else
		{
			defaultName = "material";
		}
		m_NewMaterialName = defaultName;
		if (defaultFolder.empty())
		{
			// 默认落点 = 内容浏览器当前目录(相对内容根);根目录时退回 materials/。
			std::error_code relativeError;
			const std::filesystem::path relative =
				std::filesystem::relative(m_NewMaterialPendingDir.empty() ? m_Model.Current
					: m_NewMaterialPendingDir, m_Model.Root, relativeError);
			defaultFolder = relativeError || relative.empty() || relative.generic_string() == "."
				? std::string("materials") : relative.generic_string();
		}
		m_NewMaterialFolders = Editor::AssetCatalog::Dirs();
		auto found = std::find(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end(), defaultFolder);
		if (found == m_NewMaterialFolders.end())
		{
			m_NewMaterialFolders.push_back(defaultFolder);
			std::sort(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end());
			found = std::find(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end(), defaultFolder);
		}
		m_NewMaterialFolderIndex = found == m_NewMaterialFolders.end()
			? 0 : static_cast<int>(found - m_NewMaterialFolders.begin());
		ctx.SetModal(Wui::HashId("material.new.modal"));
		m_Host.SetPanelModalOwner(Id());
		ctx.SetFocus(Wui::HashId("material.new.name"));
		ctx.RecordOp("browser", fromSelection ? "new-material-extract-ask" : "new-material-ask",
			m_NewMaterialName, defaultFolder);
	}

	void ContentBrowserPanel::CloseNewMaterialModal(Wui::WuiContext& ctx)
	{
		m_NewMaterialOpen = false;
		m_NewMaterialFromSelection = false;
		m_NewMaterialFolders.clear();
		m_NewMaterialFolderIndex = 0;
		m_NewMaterialFailure.clear();
		m_NewMaterialFailureFor.clear();
		m_NewMaterialSeed = MaterialDesc {};
		m_NewMaterialAssignEntity = Entity {};
		m_NewMaterialPendingDir.clear();
		if (ctx.Modal() == Wui::HashId("material.new.modal"))
			ctx.ClearModal();
		ctx.ClosePopup(Wui::HashId("material.new.folder"));
		ctx.ClosePopup(Wui::HashId("material.new.template"));
		m_Host.SetPanelModalOwner(std::string());
	}

	void ContentBrowserPanel::DrawNewMaterialModal(Wui::WuiContext& ctx)
	{
		if (!m_NewMaterialOpen)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.new.modal");
		const Wui::WuiTheme& theme = m_Host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = m_NewMaterialFromSelection
			? Wui::Tr("panel.content_browser.new_material.title.extract", "Extract Material from Selection")
			: Wui::Tr("panel.content_browser.new_material.title", "New Material");
		// M3:向导多了一行 Parent(选父级 + 起始覆盖),模态相应加高。
		frameDesc.Size = { 620.0f, 470.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			CloseNewMaterialModal(ctx);
			return;
		}
		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 68.0f;
		const Wui::WuiId templateId = Wui::HashId("material.new.template");
		const Wui::WuiId parentId = Wui::HashId("material.new.parent");
		const Wui::WuiId nameId = Wui::HashId("material.new.name");
		const Wui::WuiId folderId = Wui::HashId("material.new.folder");
		const bool templatePopupWasOpen = ctx.IsPopupOpen(templateId);
		const bool parentPopupWasOpen = ctx.IsPopupOpen(parentId);
		const bool folderPopupWasOpen = ctx.IsPopupOpen(folderId);
		const bool justOpened = ctx.Frame() == m_NewMaterialOpenedFrame;

		// ---- 起始覆盖下拉(Standard / Unlit-ish / Transparent / Additive)----
		// M3:这一栏不再假装是"着色模型":它描述的是**这次新建会写下哪些覆盖字段**
		// (父级是引擎默认时 = 自包含材质;父级是 .wmat 时 = 相对父级的起始覆盖)。
		float cursorY = frame.Y + 46.0f;
		const std::string templateLabel = Wui::Tr("panel.content_browser.new_material.template",
			"Starting overrides");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, theme.TextMuted, 13.0f);
		std::vector<std::string> templateOptions;
		for (const NewMaterialTemplate& entry : kNewMaterialTemplates)
			templateOptions.push_back(Wui::Tr(entry.LabelKey, entry.LabelEn));
		const Wui::WuiRect templateRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::Combo(ctx, templateId, templateRect, templateLabel, templateOptions, m_NewMaterialTemplate, theme);
		const int templateIndex = std::clamp(m_NewMaterialTemplate, 0, kNewMaterialTemplateCount - 1);
		const std::string templateDoc = Wui::Tr(kNewMaterialTemplates[templateIndex].DocKey,
			kNewMaterialTemplates[templateIndex].DocEn);
		cursorY += 30.0f;
		const std::pair<std::string, std::string> docLines =
			WrapTwoLines(ctx, templateDoc, frame.W - 32.0f, 12.0f);
		Wui::Label(ctx, { labelX, cursorY }, docLines.first, theme.TextMuted, 12.0f);
		if (!docLines.second.empty())
			Wui::Label(ctx, { labelX, cursorY + 15.0f }, docLines.second, theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.template.doc");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = templateLabel;
			node.Value = templateDoc;
			node.Tooltip = templateDoc;
			node.Rect = { labelX, cursorY, frame.W - 32.0f, docLines.second.empty() ? 16.0f : 31.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += docLines.second.empty() ? 24.0f : 39.0f;

		// M3:起始覆盖 + 名称 + 目录三行之后,与底部 Parent 行之间始终留出两行说明的高度 ——
		// 这样"起始覆盖"下拉的选项条目(弹层从它下方展开)不会压到"目录"触发条上:
		// WUI 的 popup 只在**下一帧**遮挡下层控件,点选项那一下的 release 会落到条目正下方的
		// 控件上(实测)。留白是这条规则下的必要布局约束,不是随手加的空行。
		cursorY += 39.0f;

		// ---- 名称(后缀自动补)----
		const std::string nameLabel = Wui::Tr("panel.content_browser.new_material.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		const bool nameFocused = ctx.Focus() == nameId;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("panel.content_browser.new_material.name.placeholder", "Material name");
		Wui::TextField(ctx, nameId, nameRect, m_NewMaterialName, theme, nullptr, &nameA11y);
		const bool nameSubmitted = nameFocused && ctx.IsKeyPressed(KeyCodes::Enter);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".wmat", theme.TextMuted, 13.0f);
		const std::string nameError = NewMaterialNameError();
		if (!nameError.empty())
			Wui::Label(ctx, { fieldX, cursorY + 27.0f }, nameError, theme.Danger, 12.0f);
		cursorY += 46.0f;

		// ---- 目录(内容根下的目录,可搜索)----
		const std::string folderLabel = Wui::Tr("panel.content_browser.new_material.folder", "Folder");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, folderLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect folderRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::SearchableCombo(ctx, folderId, folderRect, folderLabel, m_NewMaterialFolders,
			m_NewMaterialFolderIndex, theme);
		const std::string folderText = (m_NewMaterialFolderIndex >= 0
			&& m_NewMaterialFolderIndex < static_cast<int>(m_NewMaterialFolders.size()))
			? m_NewMaterialFolders[static_cast<size_t>(m_NewMaterialFolderIndex)] : std::string();
		{
			Wui::WuiAccessNode node;
			node.Id = folderId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = folderLabel;
			node.Value = folderText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.folder.tooltip",
				"Folder under the content root (searchable); a missing folder is created on confirm.");
			node.Rect = folderRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const std::filesystem::path folderAbsolute = m_Model.Root / std::filesystem::path(folderText);
		if (!folderText.empty() && !std::filesystem::is_directory(folderAbsolute))
			Wui::Label(ctx, { fieldX, cursorY + 27.0f },
				Wui::Tr("panel.content_browser.new_material.folder.note",
					"Folder does not exist yet — it will be created"), theme.Warning, 12.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显 + 覆盖警告 ----
		const std::string target = NewMaterialTarget();
		if (!m_NewMaterialFailure.empty() && m_NewMaterialFailureFor != target)
		{
			m_NewMaterialFailure.clear();
			m_NewMaterialFailureFor.clear();
		}
		const std::string previewLabel = Wui::Tr("panel.content_browser.new_material.preview.label", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, previewLabel, theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, target, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = previewLabel;
			node.Value = target;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.preview.tooltip",
				"Logical path of the file that will be written (folder + name + .wmat).");
			node.Rect = { fieldX, cursorY - 3.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;
		std::error_code existsError;
		const bool targetExists = nameError.empty()
			&& std::filesystem::exists(m_Model.Root / std::filesystem::path(target), existsError);
		std::string warningText;
		if (!m_NewMaterialFailure.empty())
			warningText = m_NewMaterialFailure;
		else if (targetExists)
			warningText = Wui::Tr("panel.content_browser.new_material.exists",
				"Already exists — overwriting: ") + target;
		if (!warningText.empty())
			Wui::Label(ctx, { fieldX, cursorY }, warningText, theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.content_browser.new_material.warning.label", "Warning");
			node.Value = warningText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.warning.tooltip",
				"Red line = the target exists and would be overwritten; empty = no conflict.");
			node.Rect = { fieldX, cursorY - 4.0f, fieldW, 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- M3:Parent(父级)—— 默认引擎内置默认;可选内容根下任意 .wmat ----
		// 位置 = 内容最下方、按钮条正上方,而且**收窄宽度**:
		//  - 它的弹层从下方展开(条目多、可以很长),下面只剩空白与按钮条左侧的空区;
		//    收窄后弹层 x 范围与 OK/Cancel 不重叠 —— 点条目那一下的 release 不会顺手按到按钮
		//    (WUI 的 popup 只在下一帧遮挡下层控件,选项 release 会落到条目正下方的控件上);
		//  - 上面的"起始覆盖/目录"弹层不会向下延伸到这里(见前面的留白注释)。
		const float footerY = frame.Y + frame.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const std::string parentLabel = Wui::Tr("panel.content_browser.new_material.parent", "Parent");
		std::vector<std::string> parentOptions;
		parentOptions.push_back(Wui::Tr("panel.content_browser.new_material.parent.engine",
			"Engine Default"));
		for (size_t index = 1; index < m_NewMaterialParentPaths.size(); ++index)
			parentOptions.push_back(m_NewMaterialParentPaths[index]);
		m_NewMaterialParentIndex = std::clamp(m_NewMaterialParentIndex, 0,
			static_cast<int>(parentOptions.size()) - 1);
		const std::string parentPath = m_NewMaterialParentIndex > 0
			? parentOptions[static_cast<size_t>(m_NewMaterialParentIndex)] : std::string();
		const std::string parentDoc = parentPath.empty()
			? Wui::Tr("panel.content_browser.new_material.parent.engine.doc",
				"Engine Default: the new material is self-contained — every field is written, "
				"exactly like a material created before M3.")
			: Wui::Tr("panel.content_browser.new_material.parent.file.doc",
				"Inherit from:") + " " + parentPath
				+ Wui::Tr("panel.content_browser.new_material.parent.file.doc2",
					" — the file stores Parent plus the starting overrides below; everything else "
					"keeps following the parent (edit it later in the material editor).");
		const Wui::WuiRect parentRect { fieldX, footerY - 30.0f, std::min(fieldW, 300.0f), 24.0f };
		Wui::Label(ctx, { labelX, parentRect.Y + 5.0f }, parentLabel, theme.TextMuted, 13.0f);
		Wui::Combo(ctx, parentId, parentRect, parentLabel, parentOptions,
			m_NewMaterialParentIndex, theme);
		const Wui::WuiRect parentHint { labelX, parentRect.Y - 18.0f,
			std::max(60.0f, frame.W - 32.0f), 16.0f };
		Wui::Label(ctx, { parentHint.X, parentHint.Y },
			EllipsizeToWidth(ctx, parentDoc, parentHint.W, 12.0f), theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.parent.doc");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = parentLabel;
			node.Value = parentDoc;
			node.Tooltip = parentDoc;
			node.Rect = parentHint;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		Wui::Tooltip(ctx, parentRect, parentDoc);

		// ---- 底部按钮条 ----
		const bool canCreate = nameError.empty();
		const Wui::WuiRect okRect { frame.X + frame.W - 16.0f - 150.0f, footerY, 150.0f,
			Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { okRect.X - 8.0f - 96.0f, footerY, 96.0f, Wui::ModalFooterHeight };
		const std::string okLabel = targetExists
			? Wui::Tr("panel.content_browser.new_material.overwrite", "Overwrite")
			: Wui::Tr("panel.content_browser.new_material.create", "Create Material");
		const std::string okTooltip = !canCreate
			? (nameError + " — " + Wui::Tr("panel.content_browser.new_material.ok.disabled",
				"fix the name to enable this button"))
			: (targetExists
				? Wui::Tr("panel.content_browser.new_material.overwrite.tooltip",
					"The file exists: overwrite it with this template")
				: (m_NewMaterialFromSelection
					? Wui::Tr("panel.content_browser.new_material.create.extract.tooltip",
						"Write the .wmat, assign it back to the selected entity and open it in the material editor")
					: Wui::Tr("panel.content_browser.new_material.create.tooltip",
						"Write the .wmat and open it in the material editor")));
		const bool okClicked = ModalActionButton(ctx, Wui::HashId("material.new.ok"), okRect, okLabel,
			okTooltip, canCreate, true, theme);
		const bool cancelClicked = ModalActionButton(ctx, Wui::HashId("material.new.cancel"), cancelRect,
			Wui::Tr("panel.content_browser.new_material.cancel", "Cancel"),
			Wui::Tr("panel.content_browser.new_material.cancel.tooltip",
				"Close without writing anything (Esc)"),
			true, false, theme);
		bool closeRequested = false;
		if ((okClicked || (nameSubmitted && !justOpened)) && canCreate)
		{
			const std::string absolute = (m_Model.Root / std::filesystem::path(target)).generic_string();
			const std::string base = NewMaterialBaseName();
			std::string error;
			bool wrote = false;
			if (parentPath.empty())
			{
				// 父级 = 引擎内置默认:自包含材质(全字段、没有 Parent 行 → 仍是 v1 写法,
				// 与 M3 前新建的 .wmat 逐字节一致)。
				const MaterialDesc desc = MaterialDescForTemplate(templateIndex, base, m_NewMaterialSeed);
				// 绝对路径:MaterialIO 对"还不存在的相对路径"会解析到 Game/<path>(CreateMaterialAsset 记的坑)。
				wrote = MaterialIO::WriteFileText(absolute, MaterialIO::Serialize(desc), &error);
			}
			else
			{
				// 父级 = 已有 .wmat:真材质实例 —— 只写 Parent + 本次的起始覆盖。
				Ref<Material> instance = MaterialLibrary::Get().CreateInstance(parentPath, base, &error);
				if (instance)
				{
					// 起始覆盖按预设落到具体字段(相对父级的当前解析值):
					// Standard = 不改任何字段(纯继承),其余三个只改它们真正涉及的那几项。
					const MaterialDesc resolved = instance->GetDesc();
					switch (templateIndex)
					{
						case 1:   // Unlit-ish:自发光 = 基色,金属度/粗糙度归零
							instance->SetEmissive(glm::vec3(resolved.BaseColor));
							instance->SetMetallic(0.0f);
							instance->SetRoughness(0.0f);
							break;
						case 2:   // Transparent:混合模式
							instance->SetBlendMode(MaterialBlendMode::Transparent);
							break;
						case 3:   // Additive(近似):透明 + 自发光 = 基色 x 2
							instance->SetBlendMode(MaterialBlendMode::Transparent);
							instance->SetEmissive(glm::vec3(resolved.BaseColor) * 2.0f);
							break;
						default:
							break;
					}
					wrote = MaterialLibrary::Get().Save(instance, target, &error);
				}
			}
			if (!wrote)
			{
				m_NewMaterialFailure = error.empty() ? std::string("could not write the .wmat") : error;
				m_NewMaterialFailureFor = target;
				NotifyAssetFailure(m_NewMaterialFailure);
				WLD_CORE_WARN("New material failed: {0}", m_NewMaterialFailure);
			}
			else
			{
				WLD_CORE_INFO("[material-ui] created {0} (template {1})", target, templateIndex);
				// 内容浏览器选中新资产(与"新建资产"同一条通道)+ 在材质编辑器里打开它。
				SelectCreated(m_Model.Root / std::filesystem::path(target), "new-material");
				m_Host.OpenMaterialEditor(target);
				std::string notice = Wui::Tr("panel.content_browser.new_material.created", "Created ")
					+ target;
				if (m_NewMaterialFromSelection && m_NewMaterialAssignEntity.IsValid())
				{
					std::string assignMessage;
					if (m_Host.SetEntityMaterialPath(m_NewMaterialAssignEntity, target, &assignMessage))
						notice += " · " + Wui::Tr("panel.content_browser.new_material.assigned",
							"assigned back to the selected entity");
					else
						notice += " · " + Wui::Tr("panel.content_browser.new_material.assign_failed",
							"could not assign it back: ") + assignMessage;
				}
				m_Host.Notify(notice);
				closeRequested = true;
			}
		}
		else if (cancelClicked)
			closeRequested = true;
		else if (escapePressed)
		{
			// Esc 分层:先关展开的下拉,第二次才关模态。
			if (templatePopupWasOpen)
				ctx.ClosePopup(templateId);
			else if (parentPopupWasOpen)
				ctx.ClosePopup(parentId);
			else if (folderPopupWasOpen)
				ctx.ClosePopup(folderId);
			else
				closeRequested = true;
		}
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == modalId)
			CloseNewMaterialModal(ctx);
	}

	void ContentBrowserPanel::RegisterSliceNode(const BrowserSlice& slice, const Wui::WuiRect& rect)
	{
		std::error_code relativeError;
		const std::filesystem::path relative =
			std::filesystem::relative(slice.Path, m_Model.Root, relativeError);
		const std::string logical = relativeError ? slice.Path.filename().generic_string()
			: relative.generic_string();
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId(("browser.slice." + logical).c_str());
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "asset-slice";
		node.Label = slice.Name;
		node.Value = logical;
		node.Tooltip = slice.IsDir
			? Wui::Tr("panel.content_browser.slice.folder.tooltip", "Folder under the content root: ") + logical
			: Wui::Tr("panel.content_browser.slice.asset.tooltip", "Asset: ") + logical
				+ Wui::Tr("panel.content_browser.slice.asset.hint",
					" — drag it onto a texture slot / material title");
		node.Rect = rect;
		node.Enabled = true;
		node.Interactive = true;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
	}

	bool ContentBrowserPanel::GlobalCursorScreen(const Wui::WuiContext& ctx, float* outX, float* outY) const
	{
		if (!Application::HasInstance())
			return false;
		int windowX = 0, windowY = 0;
		Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const glm::vec2 mouse = ctx.Input().MousePos;
		if (outX)
			*outX = static_cast<float>(windowX) + mouse.x * scale;
		if (outY)
			*outY = static_cast<float>(windowY) + mouse.y * scale;
		return true;
	}

	void ContentBrowserPanel::DeliverCrossWindowDrop(Wui::WuiContext& ctx)
	{
		std::string payload;
		if (!ctx.IsDragActive(&payload) || payload.rfind("file:", 0) != 0)
			return;
		// 只在**释放那一帧**投递:EndFrame 才把拖拽收口,这里读到的还是"正在拖"的状态。
		if (!ctx.Input().MouseReleased[0])
			return;
		float screenX = 0.0f, screenY = 0.0f;
		if (!GlobalCursorScreen(ctx, &screenX, &screenY))
			return;
		// 命中别的窗口登记过的落点(材质编辑器的贴图槽 / 标题)才消费;
		// 命中不了 = 什么都不做 —— 浏览器自己的移动/落点逻辑照旧(下一帧 AcceptDrop)。
		if (Editor::AssetDropBridge::Get().DeliverFromScreen(screenX, screenY, payload))
		{
			if (m_Ctx)
				m_Ctx->RecordOp("browser", "drop-to-window", payload, "cross-window");
			// 这一次拖拽由别的窗口消费了:清掉浏览器自己的候选落点,免得它留到
			// **下一次**拖拽被 AcceptDrop 误当成落点(移动文件)。
			m_Model.PendingDropDest.clear();
			ctx.EndDrag();
		}
	}

	void ContentBrowserPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		m_Ctx = &ctx;
		const Wui::WuiTheme& theme = host.Theme();
		// U25-M2:向导请求(注册表回调 / Window 菜单 / 材质面板的 Extract)在**下一帧的这里**落地 ——
		// 打开模态需要 WuiContext 与布局,只有渲染期才具备。
		if (m_NewMaterialPendingOpen)
		{
			m_NewMaterialPendingOpen = false;
			OpenNewMaterialModal(ctx, m_NewMaterialPendingFromSelection);
			m_NewMaterialPendingFromSelection = false;
		}
		// U25-M2:跨窗口拖放(内容浏览器 → 材质编辑器的贴图槽/标题):在**释放那一帧**把
		// "file:<逻辑路径>" 交给登记过落点的面板(核心 WUI 的拖拽态是每窗口一份,见 AssetDropBridge)。
		DeliverCrossWindowDrop(ctx);
		// P4-U13d:选中状态进无障碍树 —— 创建资产(预制体等)之后,脚本/读屏要能确认
		// "内容浏览器真的选中了哪个资产"。此前选中只画在画面上,ui.tree 读不到。
		{
			std::string selectedText;
			if (!m_Model.LastSelected.empty())
			{
				std::error_code selectionError;
				const std::filesystem::path relative =
					std::filesystem::relative(m_Model.LastSelected, m_Model.Root, selectionError);
				selectedText = selectionError ? m_Model.LastSelected.generic_string()
					: relative.generic_string();
			}
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("browser.selection");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.content_browser.selection", "Selected asset");
			node.Value = selectedText;   // 逻辑路径;空 = 当前没有选中
			node.Tooltip = Wui::Tr("panel.content_browser.selection.tooltip",
				"Logical path of the selected asset (empty = nothing selected)");
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// D10:OS 文件拖放(资源管理器 → 窗口)。平台层把拖入路径记在**收到拖放的窗口**上,
		// 这里消费主窗口的队列:`.gltf/.glb` → 导入到**当前文件夹**(用户 Q1/Q2);
		// 其它类型明确提示"不支持该类型",不静默丢弃。
		{
			std::vector<std::string> dropped = Application::Get().GetWindow().ConsumeDroppedFiles();
			for (const std::string& droppedPath : dropped)
			{
				const std::filesystem::path source(droppedPath);
				const std::string extension = source.extension().string();
				if (extension == ".gltf" || extension == ".glb")
				{
					std::error_code destError;
					const std::filesystem::path destRelative =
						std::filesystem::relative(m_Model.Current, m_Model.Root, destError);
					const std::string destination =
						destError ? std::string() : destRelative.generic_string();
					std::string message;
					std::string logicalModel;
					if (!m_Host.ImportModelFileTo(source.string(), destination, &message, &logicalModel))
					{
						WLD_CORE_WARN("[drop] 导入 '{0}' 失败: {1}", droppedPath, message);
						if (m_Ctx) m_Ctx->RecordOp("browser", "drop-import-failed",
							source.filename().string(), message);
					}
					else
					{
						WLD_CORE_INFO("[drop] {0}", message);
						if (m_Ctx) m_Ctx->RecordOp("browser", "drop-import",
							source.filename().string(), message);
						InvalidateContents();
						if (m_Model.Search[0])
							UpdateSearch();
						if (!logicalModel.empty())
						{
							SelectCreated(m_Model.Root / logicalModel, "import-model");
							m_Host.OpenModelPreview(logicalModel);
						}
					}
				}
				else
				{
					// Q2:非 glTF 类型不支持(不复制、不静默)。
					const std::string message = Wui::Tr("panel.content_browser.drop.unsupported",
						"Unsupported file type: ") + extension
						+ Wui::Tr("panel.content_browser.drop.unsupported_hint",
							"(only .gltf / .glb can be dropped for import)");
					WLD_CORE_WARN("[drop] {0}", message);
					if (m_Ctx) m_Ctx->RecordOp("browser", "drop-rejected",
						source.filename().string(), message);
				}
			}
		}
		// 窗口/GL 上下文重建后旧图标纹理失效:丢弃缓存,重新加载并注册。
		if (m_TextureEpoch != host.TextureEpoch())
		{
			m_TextureEpoch = host.TextureEpoch();
			m_DirIcon = nullptr;
			m_FileIcon = nullptr;
			m_DirIconId = 0;
			m_FileIconId = 0;
		}
		if (!m_DirIcon)
			m_DirIcon = Texture2D::Create(EditorResourcePath("Resource/Icons/ContentBrowser/DirectoryIcon.png"));
		if (!m_FileIcon)
			m_FileIcon = Texture2D::Create(EditorResourcePath("Resource/Icons/ContentBrowser/FileIcon.png"));
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (registry.Generation() != m_IconGeneration)
		{
			m_DirIconId = registry.RegisterTexture2D(m_DirIcon);
			m_FileIconId = registry.RegisterTexture2D(m_FileIcon);
			m_IconGeneration = registry.Generation();
		}

		std::string filePayload;
		const bool fileDrag = ctx.IsDragActive(&filePayload) && filePayload.rfind("file:", 0) == 0;

		// ---- 工具栏(P4-UX14 重新设计)----
		// 用户:"顶部那排按钮真的有必要存在吗?" —— 结论:**只留三样**。
		//   ① 面包屑(路径本身就是导航,点任意一级直接跳);
		//   ② 搜索(常驻,高频);
		//   ③ 一个 `⋯` 菜单(新建/刷新/视图切换/在资源管理器中打开/后退前进上一级)。
		// 为什么删掉那六个动作按钮:后退/前进/上一级在树和面包屑里都有等价入口,
		// 新建/刷新/视图是低频动作 —— 六个常驻按钮换一个菜单,工具栏从"一排控件"回到"一行路径",
		// 与 VS Code 资源管理器/Blender 浏览器一致(导航靠树,动作靠菜单 + 快捷键)。
		if (!m_Toolbar)
		{
			m_Toolbar = std::make_shared<Wui::WuiBox>();
			m_Toolbar->Direction = Wui::WuiDirection::Row;
			m_Toolbar->Gap = 6;
			m_Toolbar->AlignCross = Wui::WuiAlign::Center;

			auto addButton = [&](const std::string& label, std::function<void()> action, float width,
				bool centerLabel = false)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->CenterLabel = centerLabel;
				button->OnClick = std::move(action);
				m_Toolbar->Add(button, { width, width, 0, 24, 0 });
				return button;
			};
			m_Breadcrumbs = std::make_shared<Wui::WuiBox>();
			m_Breadcrumbs->Direction = Wui::WuiDirection::Row;
			m_Breadcrumbs->Gap = 4;
			m_Breadcrumbs->AlignCross = Wui::WuiAlign::Center;
			// P4-UX13:面包屑**吃掉剩余宽度**(以前靠 spacer 顶到右边),这样面板变窄时先挤的是路径,
			// 而不是把右侧的搜索框/动作挤出画面(实测 890px 宽时搜索框整个看不见)。
			// P4-UX15:面包屑**不再进布局树**(嵌套 Box 的宽度预算在窄面板/深路径下不可控,
			// 实测路径会整条消失)。这里只放一个占位 spacer,真正的面包屑在工具栏画完后
			// 用立即模式逐枚画(见下方"面包屑(立即模式)")。
			auto toolbarSpacer = std::make_shared<Wui::WuiSpacer>();
			m_Toolbar->Add(toolbarSpacer, { 8, 1e30f, 0, 24, 1 });

			// `⋯` 菜单:动作收进一处,低频动作不再占用常驻空间。
			// "…"(U+2026)在字体子集里有;"⋯"(U+22EF)没有 → 会画成乱码(用户实测)。
			m_MoreButton = addButton("…", [this] { m_ToolbarMenuOpen = true; }, 30, true);
			m_MoreButton->CenterLabel = true;

			m_SearchField = std::make_shared<Wui::WuiTextField>();
			// D10:给搜索框一个稳定 id —— AI/脚本可以 ui.type 驱动它(以前只能手点)。
			m_SearchField->SetId(Wui::HashId("browser.search"));
			// P4-U5a:占位提示是面板自己画的 Label,读屏/脚本读不到 → 显式喂给控件
			// (节点 label=控件名,value=空输入时的占位文案)。
			m_SearchField->A11yLabel = Wui::Tr("panel.content_browser.search.a11y", "Search assets");
			m_SearchField->A11yPlaceholder = Wui::Tr("panel.content_browser.search.hint", "Search assets…");
			m_SearchField->Buffer = &m_Model.SearchEdit;
			m_SearchField->OnCommit = [this]
				{
					std::strncpy(m_Model.Search, m_Model.SearchEdit.c_str(), sizeof(m_Model.Search) - 1);
					m_Model.Search[sizeof(m_Model.Search) - 1] = 0;
					UpdateSearch();
				};
			m_Toolbar->Add(m_SearchField, { 140, 162, 0, 24, 0 });
		}

		// 导航快捷键(工具栏删掉三个导航按钮后的补偿):Alt+←/→ 后退/前进、Alt+↑ 上一级。
		// 文本控件持焦点时不抢(否则在搜索框里按方向键会跳目录)。
		if (!Wui::WuiTextFocus::Get().Active() && ctx.Input().Alt)
		{
			if (ctx.WasKeyPressed(KeyCodes::Left) && m_Model.HistoryIndex > 0)
				GoBack();
			else if (ctx.WasKeyPressed(KeyCodes::Right)
				&& m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1)
			{
				++m_Model.HistoryIndex;
				m_Model.Current = m_Model.History[m_Model.HistoryIndex];
				m_Model.Selected.clear();
				m_Model.LastSelected.clear();
				Reveal(m_Model.Current);
				m_Model.ListingDirty = true;
				m_Model.ListingStamp = {};
				SaveState();
				if (m_Model.Search[0])
					UpdateSearch();
			}
			else if (ctx.WasKeyPressed(KeyCodes::Up) && m_Model.Current != m_Model.Root)
				GoUp();
		}

		// 面包屑随路径变化重建。
		if (m_LastCrumbPath != m_Model.Current)
		{
			m_LastCrumbPath = m_Model.Current;
			m_CrumbButtons.clear();
			m_CrumbDests.clear();
			m_CrumbLabels.clear();
			m_Breadcrumbs->Clear();
			const auto addCrumb = [&](const std::string& label, const std::filesystem::path& destination)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->OnClick = [this, destination] { Navigate(destination); };
				m_Breadcrumbs->Add(button, { static_cast<float>(label.size() * 8 + 20), static_cast<float>(label.size() * 8 + 20), 0, 24, 0 });
				m_CrumbButtons.push_back(button);
				m_CrumbDests.push_back(destination);
				m_CrumbLabels.push_back(label);
			};
			addCrumb(Wui::Tr("panel.content_browser.root", "Root"), m_Model.Root);
			// 面包屑用 › 分隔而不是一串等距按钮:路径的"层级感"来自分隔符与当前段的高亮。
			const auto addCrumbSeparator = [&]()
			{
				auto separator = std::make_shared<Wui::WuiLabel>();
				separator->Text = "›";
				separator->Color = theme.TextMuted;
				separator->FontSize = 13.0f;
				separator->FixedWidth = 12.0f;
				m_Breadcrumbs->Add(separator, { 12.0f, 12.0f, 0, 24, 0 });
			};
			std::filesystem::path accumulated = m_Model.Root;
			// 路径太深时只显示最后两级 + "...":整条路径铺不下会把面包屑挤爆(面板一窄就"消失")。
			std::vector<std::string> parts;
			for (const auto& part : m_Model.Current.lexically_relative(m_Model.Root))
			{
				// 根目录自身的相对路径是 ".":它不是一个真实层级,别画成一枚空的 crumb
				// (实测工具条最右侧出现一个 "." 按钮,看起来像坏掉的控件)。
				if (part == "." || part.empty())
					continue;
				parts.push_back(part.string());
			}
			const size_t firstShown = parts.size() > 3 ? parts.size() - 2 : 0;
			if (firstShown > 0)
			{
				// 前面被折叠的部分不再逐级列出,直接给一个不可点的省略号占位。
				auto dots = std::make_shared<Wui::WuiLabel>();
				dots->Text = "…";
				dots->Color = theme.TextMuted;
				dots->FontSize = 13.0f;
				dots->FixedWidth = 14.0f;
				m_Breadcrumbs->Add(dots, { 14.0f, 14.0f, 0, 24, 0 });
				for (size_t i = 0; i < firstShown; ++i)
					accumulated /= parts[i];
			}
			for (size_t i = firstShown; i < parts.size(); ++i)
			{
				accumulated /= parts[i];
				addCrumbSeparator();
				addCrumb(parts[i], accumulated);
			}
		}

		Wui::LayoutWidgetTree(m_Toolbar, { rect.X + 6, rect.Y + 6, rect.W - 12, 24 });
		Wui::WuiPaintContext toolbarPaint(ctx);
		m_Toolbar->Paint(toolbarPaint);
		// ---- 面包屑(立即模式)----
		// P4-UX15:不再依赖嵌套 Box —— 显式按 x 预算排布,放不下的中间层级收成 "…",
		// 保证"进入任意深度的文件夹,路径都看得见"(用户实测:以前一进目录整条路径消失)。
		{
			const float budgetRight = m_SearchField->Rect().X - 12.0f;
			float x = rect.X + 6.0f;
			for (size_t i = 0; i < m_CrumbLabels.size(); ++i)
			{
				const std::string& label = m_CrumbLabels[i];
				const bool last = (i + 1) == m_CrumbLabels.size();
				const float width = std::min(180.0f, ctx.MeasureTextWidth(label, 13.0f) + 18.0f);
				if (!last && x + width > budgetRight)
				{
					Wui::Label(ctx, { x, rect.Y + 11.0f }, "…", theme.TextMuted, 12.0f);
					x += 16.0f;
					continue;
				}
				const Wui::WuiRect crumb { x, rect.Y + 6.0f, width, 24.0f };
				const Wui::WuiId crumbId = Wui::HashId(("browser.crumb." + std::to_string(i)).c_str());
				if (Wui::Button(ctx, crumbId, crumb, label, theme) && !last)
					Navigate(m_CrumbDests[i]);
				Wui::WuiAccessNode node;
				node.Id = crumbId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "crumb";
				node.Label = label;
				node.Value = last ? "current" : "parent";
				node.Rect = crumb;
				node.Enabled = true;
				node.Interactive = !last;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				if (!last)
					Wui::Label(ctx, { crumb.X + crumb.W + 2.0f, rect.Y + 11.0f }, "›", theme.TextMuted, 12.0f);
				x += width + 14.0f;
			}
		}

		// P4-UX13:工具条 = 四组(导航 | 路径 | 搜索 | 视图与动作),组间画 1px 分隔线;
		// 每个图标按钮都登记悬停说明 —— 图标没有 tooltip 就等于没解释。
		{
			const auto separator = [&](const Wui::WuiRect& firstOfGroup)
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
					{ firstOfGroup.X - 8.0f, rect.Y + 9.0f, 1.0f, 18.0f }, theme.Border, 0.0f });
			};
			separator(m_SearchField->Rect());
			if (m_MoreButton)
				separator(m_MoreButton->Rect());

			if (m_MoreButton)
				Wui::Tooltip(ctx, m_MoreButton->Rect(), Wui::Tr("panel.content_browser.more.tooltip",
					"More actions: New / Refresh / View / Back-Forward (Alt+←/→/↑)"));
			// P4-UX16:`…` 是工具条上唯一的"动作入口",但对象式 WuiButton 不进无障碍树 ——
			// 结果脚本只能靠猜坐标点它(实测:布局一变就点空)。这里按面包屑同款做法补一个稳定节点,
			// `ui.invoke id=browser.more` 就能打开菜单(与真实点击同一条输入路径)。
			if (m_MoreButton)
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("browser.more");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "button";
				node.Label = "…";
				node.Value = ctx.IsPopupOpen(Wui::HashId("browser.toolbar.menu")) ? "open" : "closed";
				node.Tooltip = Wui::Tr("panel.content_browser.more.tooltip",
					"More actions: New / Refresh / View / Back-Forward (Alt+←/→/↑)");
				node.Rect = m_MoreButton->Rect();
				Wui::WuiAccessibility::Get().Register(node);
			}
			// 搜索框:空时给占位提示,有内容时右侧给"清除"(与浏览器/编辑器的搜索框一致)。
			const Wui::WuiRect searchRect = m_SearchField->Rect();
			if (m_Model.SearchEdit.empty())
				Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f },
					Wui::Tr("panel.content_browser.search.hint", "Search assets…"), theme.TextDisabled, 12.0f);
			else
			{
				const Wui::WuiRect clearRect { searchRect.X + searchRect.W - 20.0f, searchRect.Y + 2.0f, 18.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("browser.search.clearfield"), clearRect, "x", theme))
				{
					m_Model.SearchEdit.clear();
					m_Model.Search[0] = 0;
					UpdateSearch();
				}
				Wui::Tooltip(ctx, clearRect, Wui::Tr("panel.content_browser.search.clear.tooltip", "Clear search"));
			}
		}

		if (fileDrag)
			for (size_t i = 0; i < m_CrumbButtons.size(); ++i)
				if (ctx.IsHovered(m_CrumbButtons[i]->Rect()))
				{
					ctx.DropTarget(m_CrumbButtons[i]->Rect(), "file:");
					m_Model.PendingDropDest = m_CrumbDests[i];
					Wui::HighlightOutline(ctx, m_CrumbButtons[i]->Rect(), theme.Accent, 2.0f, 2.0f);
				}

		float y = rect.Y + 38;

		// ---- 左侧目录树 ----
		const float treeW = 190;
		const Wui::WuiRect treeRect { rect.X, y, treeW, rect.H - (y - rect.Y) };
		Wui::PanelBackground(ctx, treeRect, { 0.09f, 0.095f, 0.10f, 1 });
		RefreshTree(false);
		// 目录树走 TreeView 组件:展开箭头/悬停/选中由组件绘制,
		// 导航、拖拽起手、拖入目标仍由面板处理(用组件返回的 ItemRects)。
		std::vector<const BrowserDirNode*> visibleNodes;
		std::vector<Wui::TreeViewItem> treeItems;
		for (const BrowserDirNode& node : m_Model.DirTree)
		{
			if (node.Depth > 1 && m_Model.TreeOpen.find(node.Path.parent_path()) == m_Model.TreeOpen.end())
				continue;
			const std::filesystem::path rel = node.Path.lexically_relative(m_Model.Root);
			Wui::TreeViewItem item;
			item.Id = Wui::HashId(("browser.tree." + rel.generic_string()).c_str());
			item.Label = node.Path.filename().string();
			item.Depth = node.Depth;
			item.HasChildren = node.HasChildren;
			item.Expanded = m_Model.TreeOpen.find(node.Path) != m_Model.TreeOpen.end();
			item.Selected = m_Model.Current == node.Path;
			visibleNodes.push_back(&node);
			treeItems.push_back(std::move(item));
		}
		bool treeRenameDrawn = false;
		{
			// P4-UX14:传 id 之后树才有键盘(Tab 停在这里时 ↑↓←→/Enter 生效)。
			Wui::TreeViewResult tree = Wui::TreeView(ctx, treeRect, treeItems, 22.0f, m_Model.TreeScroll, theme,
				Wui::HashId("browser.tree"));
			// 键盘导航:控件只报"想做什么",改模型仍走与鼠标同一条路径。
			if (tree.KeyMoveTo >= 0 && tree.KeyMoveTo < static_cast<int>(visibleNodes.size()))
			{
				Navigate(visibleNodes[tree.KeyMoveTo]->Path);
				m_Model.TreeScroll = std::max(0.0f, 22.0f * static_cast<float>(tree.KeyMoveTo) - treeRect.H * 0.5f);
			}
			else if (tree.KeyToggleExpand >= 0 && tree.KeyToggleExpand < static_cast<int>(visibleNodes.size()))
			{
				const std::filesystem::path& path = visibleNodes[tree.KeyToggleExpand]->Path;
				// 固定根不参与展开/折叠(控件本来也只为 HasChildren 行发这个键)。
				if (path != m_Model.Root)
				{
					if (treeItems[tree.KeyToggleExpand].Expanded)
						m_Model.TreeOpen.erase(path);
					else
						m_Model.TreeOpen.insert(path);
					SaveState();
				}
			}
			else if (tree.KeyActivate >= 0 && tree.KeyActivate < static_cast<int>(visibleNodes.size()))
			{
				Navigate(visibleNodes[tree.KeyActivate]->Path);
			}
			// D10-6:树行右键 → 打开树菜单(命中由 TreeView 的 ContextClicked 提供,不自己写命中检测)。
			if (tree.ContextClicked >= 0 && tree.ContextClicked < static_cast<int>(visibleNodes.size()))
			{
				// 与内容区菜单互斥:开新菜单前关掉旧菜单,避免两个菜单叠在一起。
				ctx.CloseAllPopups();
				m_TreeMenuPath = visibleNodes[tree.ContextClicked]->Path;
				m_TreeMenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("browser.tree.context"));
			}
			for (size_t i = 0; i < visibleNodes.size(); ++i)
			{
				const BrowserDirNode& node = *visibleNodes[i];
				if (i >= tree.ItemRects.size())
					break;
				const Wui::WuiRect& row = tree.ItemRects[i];
				const bool hovered = ctx.IsHovered(row);
				if (tree.ClickedArrow == static_cast<int>(i))
				{
					if (node.Depth > 0)   // 固定根没有箭头,这里只是防御性判断
					{
						if (treeItems[i].Expanded) m_Model.TreeOpen.erase(node.Path);
						else m_Model.TreeOpen.insert(node.Path);
						SaveState();
					}
				}
				else if (tree.Clicked == static_cast<int>(i))
				{
					// 树菜单发起的重命名:点击落在该行输入框上时不要顺带导航进这个目录。
					if (!(m_Model.RenameActive && m_Model.RenameTarget == node.Path && m_TreeRenameTarget == node.Path))
						Navigate(node.Path);
				}
				// D10-6:从树菜单发起的重命名,输入框画在树行上(内容区那一份跳过,避免同一 id 画两份)。
				if (!treeRenameDrawn && m_Model.RenameTarget == node.Path && m_TreeRenameTarget == node.Path
					&& !(row.Y + row.H < treeRect.Y || row.Y > treeRect.Y + treeRect.H))
				{
					RenderRenameField(ctx, node.Path,
						{ row.X + 18.0f, row.Y + 1.0f, std::max(60.0f, row.W - 22.0f), row.H - 2.0f }, theme);
					treeRenameDrawn = true;
				}
				if (ctx.Input().MouseDown[0] && hovered && node.Path != m_Model.Root)
				{
					const std::filesystem::path rel = node.Path.lexically_relative(m_Model.Root);
					ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
					ctx.SetCursor(Wui::WuiCursor::Hand);
				}
				if (fileDrag && hovered)
				{
					ctx.DropTarget(row, "file:");
					m_Model.PendingDropDest = node.Path;
					Wui::HighlightOutline(ctx, row, theme.Accent, 2.0f, 2.0f);
				}
			}
		}

		// ---- 树行右键菜单(D10-6:基础操作;每个菜单项带稳定无障碍 id,AI 可点) ----
		// ---- `⋯` 工具栏菜单(P4-UX14):新建/刷新/视图切换/打开/导航都收在这里 ----
		const Wui::WuiId toolbarPopup = Wui::HashId("browser.toolbar.menu");
		if (m_ToolbarMenuOpen)
		{
			m_ToolbarMenuOpen = false;
			ctx.CloseAllPopups();
			m_ToolbarMenuPos = m_MoreButton
				? glm::vec2 { m_MoreButton->Rect().X, m_MoreButton->Rect().Y + m_MoreButton->Rect().H + 2.0f }
				: glm::vec2 { rect.X + 6.0f, rect.Y + 32.0f };
			ctx.OpenPopup(toolbarPopup);
		}
		if (ctx.IsPopupOpen(toolbarPopup))
		{
			const float menuW = 236.0f;
			const float itemH = 22.0f;
			// P4-UX16:新建从"两条硬编码项"变成一行 "New ▶" + 注册表驱动的子菜单 → 项数 8 → 7。
			const int itemCount = 7;
			const Wui::WuiRect menuRect { m_ToolbarMenuPos.x, m_ToolbarMenuPos.y, menuW,
				itemH * static_cast<float>(itemCount) + 8.0f };
			const auto goForward = [this]
			{
				if (m_Model.HistoryIndex >= static_cast<int>(m_Model.History.size()) - 1)
					return;
				++m_Model.HistoryIndex;
				m_Model.Current = m_Model.History[m_Model.HistoryIndex];
				m_Model.Selected.clear();
				m_Model.LastSelected.clear();
				Reveal(m_Model.Current);
				m_Model.ListingDirty = true;
				m_Model.ListingStamp = {};
				SaveState();
				if (m_Model.Search[0])
					UpdateSearch();
			};
			ctx.PushOverlay();
			Wui::DrawPanelSurface(ctx, menuRect, theme);
			// P4-U7:登记为覆盖层矩形 → 下一帧只挡下层控件(树/切片不再吃掉菜单上的点击)。
			ctx.RegisterOverlayRect(menuRect);
			int hoveredIndex = -1;
			auto item = [&](int index, const std::string& label, bool enabled, std::function<void()> action)
			{
				const Wui::WuiRect row { menuRect.X + 4.0f,
					menuRect.Y + 4.0f + itemH * static_cast<float>(index), menuW - 8.0f, itemH };
				if (enabled && ctx.IsHovered(row))
					hoveredIndex = index;
				const Wui::WuiId itemId = Wui::HashId(("browser.toolbar.menu." + std::to_string(index)).c_str());
				if (Wui::MenuItem(ctx, itemId, row, label, enabled, theme) && enabled)
				{
					action();
					ctx.ClosePopup(toolbarPopup);
				}
			};
			// 0 = 新建(展开的清单 = 资产类型注册表,不再硬编码类型)
			const Wui::WuiRect newRow { menuRect.X + 4.0f, menuRect.Y + 4.0f, menuW - 8.0f, itemH };
			if (RenderNewAssetRow(ctx, Wui::HashId("browser.toolbar.menu.0"), newRow, theme))
				m_NewMenuOwner = (m_NewMenuOwner == 1) ? 0 : 1;
			if (ctx.IsHovered(newRow))
				hoveredIndex = 0;
			item(1, Wui::Tr("panel.content_browser.refresh", "Refresh"), true, [this]
				{
					InvalidateContents();
					if (m_Model.Search[0])
						UpdateSearch();
				});
			item(2, m_Model.ListMode
					? Wui::Tr("panel.content_browser.menu.to_grid", "View: Switch to Grid")
					: Wui::Tr("panel.content_browser.menu.to_list", "View: Switch to List"),
				true, [this] { m_Model.ListMode = !m_Model.ListMode; SaveState(); });
			item(3, Wui::Tr("panel.content_browser.menu.reveal", "Open in Explorer"),
				m_Model.Current != m_Model.Root, [this] { OpenFolderInExplorer(m_Model.Current); });
			item(4, Wui::Tr("panel.content_browser.menu.back", "Back") + "   (Alt+←)",
				m_Model.HistoryIndex > 0, [this] { GoBack(); });
			item(5, Wui::Tr("panel.content_browser.menu.forward", "Forward") + "   (Alt+→)",
				m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1, goForward);
			item(6, Wui::Tr("panel.content_browser.menu.up", "Up") + "   (Alt+↑)",
				m_Model.Current != m_Model.Root, [this] { GoUp(); });
			// 展开的"新建"清单(右/左侧贴边翻转);idx>0 的行被悬停 = 用户离开了新建行 → 收起。
			Wui::WuiRect newMenuRect;
			if (m_NewMenuOwner == 1)
				newMenuRect = RenderNewAssetItems(ctx, "browser.toolbar.menu.new.", menuRect, rect, theme);
			ctx.PopOverlay();
			if (m_NewMenuOwner == 1 && hoveredIndex > 0)
				m_NewMenuOwner = 0;
			// Esc:先收子菜单,再收父菜单(与右键菜单一致)。
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				if (m_NewMenuOwner == 1)
					m_NewMenuOwner = 0;
				else
					ctx.ClosePopup(toolbarPopup);
			}
			// P4-UX15:点面板里其它任何地方都收起 `…` 菜单(用户实测:以前点外面不关)。
			// P4-UX16:展开的"新建"清单与父菜单算**同一块**点击区 —— 否则点子菜单里的项会
			// 被当成"点到了外面"而先把父菜单关掉。
			Wui::WuiRect clickBlock = menuRect;
			if (newMenuRect.W > 0.0f)
			{
				const float right = std::max(menuRect.X + menuRect.W, newMenuRect.X + newMenuRect.W);
				const float bottom = std::max(menuRect.Y + menuRect.H, newMenuRect.Y + newMenuRect.H);
				clickBlock.X = std::min(clickBlock.X, newMenuRect.X);
				clickBlock.Y = std::min(clickBlock.Y, newMenuRect.Y);
				clickBlock.W = right - clickBlock.X;
				clickBlock.H = bottom - clickBlock.Y;
			}
			ctx.ClosePopupsOnOutsideClick({ toolbarPopup }, clickBlock);
		}
		else if (m_NewMenuOwner == 1)
		{
			// 父菜单被关掉(点外面/Esc/执行了菜单项)→ 子菜单不能再留着。
			m_NewMenuOwner = 0;
		}

		const Wui::WuiId treePopup = Wui::HashId("browser.tree.context");
		if (ctx.IsPopupOpen(treePopup) && !m_TreeMenuPath.empty())
		{
			// 根行可新建/刷新/打开,但重命名/删除内容根会让整棵树失效 → 这两项对根行禁用。
			const bool treeRoot = m_TreeMenuPath == m_Model.Root;
			struct TreeMenuItem
			{
				const char* Label;
				Wui::WuiId Id;
				bool Enabled;
				std::function<void()> Action;
			};
			const std::vector<TreeMenuItem> items = {
				{ "New Folder", Wui::HashId("browser.tree.menu.newfolder"), true,
					[this, &ctx]
					{
						const std::filesystem::path parent = m_TreeMenuPath;
						// 父行刚被右键过说明它已可见;展开它保证新建的子行能看到(重命名输入框画在那里)。
						m_Model.TreeOpen.insert(parent);
						const std::filesystem::path created = CreateFolderIn(ctx, parent);
						if (!created.empty())
							m_TreeRenameTarget = created;
					} },
				{ "Rename", Wui::HashId("browser.tree.menu.rename"), !treeRoot,
					[this, &ctx]
					{
						StartRename(ctx, m_TreeMenuPath);
						m_TreeRenameTarget = m_TreeMenuPath;
					} },
				{ "Delete", Wui::HashId("browser.tree.menu.delete"), !treeRoot,
					[this]
					{
						// 复用内容区删除流程:选中该行 → 现有删除确认弹窗 → DeleteSelection。
						m_Model.Selected.clear();
						m_Model.Selected.insert(m_TreeMenuPath);
						m_Model.LastSelected = m_TreeMenuPath;
						m_Model.ShowDeleteModal = true;
					} },
				{ "Open in Explorer", Wui::HashId("browser.tree.menu.openinexplorer"), true,
					[this] { OpenFolderInExplorer(m_TreeMenuPath); } },
				{ "Refresh", Wui::HashId("browser.tree.menu.refresh"), true,
					[this]
					{
						InvalidateContents();
						if (m_Model.Search[0])
							UpdateSearch();
					} },
			};
			// 与内容区两个菜单同一套组件:位置钉住 + 外部点击/Esc 关闭。
			Wui::WuiRect menuPanel;
			if (Wui::BeginContextMenu(ctx, treePopup, m_TreeMenuPos, 180.0f, items.size(), &menuPanel, theme))
			{
				for (size_t i = 0; i < items.size(); ++i)
				{
					const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 22, menuPanel.W - 8, 22 };
					if (Wui::ContextMenuItem(ctx, items[i].Id, item, items[i].Label, theme, items[i].Enabled))
					{
						items[i].Action();
						if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser-tree");
						ctx.CloseAllPopups();
					}
				}
				if (ctx.IsKeyPressed(KeyCodes::Escape))
					ctx.ClosePopup(treePopup);
				Wui::EndContextMenu(ctx, treePopup, menuPanel, theme);
			}
		}
		else
		{
			// 菜单已关闭/无目标 → 清掉跨帧目标,避免下一帧按旧位置画出孤儿菜单。
			m_TreeMenuPath.clear();
			if (ctx.IsPopupOpen(treePopup))
				ctx.ClosePopup(treePopup);
		}

		// ---- 右侧内容区 ----
		const Wui::WuiRect content { rect.X + treeW + 6, y, rect.W - treeW - 6, rect.H - (y - rect.Y) };
		const bool searching = m_Model.Search[0] != 0;
		if (!searching)
			RefreshListing();
		const std::vector<std::filesystem::path>& paths = searching ? m_Model.SearchResults : m_Model.Listing;

		if (fileDrag && ctx.IsHovered(content))
		{
			ctx.DropTarget(content, "file:");
			m_Model.PendingDropDest = m_Model.Current;
			Wui::HighlightOutline(ctx, content, theme.Accent, 0.0f, 2.0f);
		}

		// ---- P4-UX14:内容区统一切片 ----
		// 网格与列表共用同一份切片数据(图标 + 名称 + 类型/大小);排序与绘制都基于它。
		// 排序状态走 ctx.Persist(与控件状态同一条路):0=名称 1=类型 2=大小,-1=未排序。
		int& sortColumn = ctx.Persist<int>(Wui::HashId("browser.list.sort"), -1);
		bool& sortAscending = ctx.Persist<bool>(Wui::HashId("browser.list.sort.asc"), true);
		std::vector<BrowserSlice> slices;
		slices.reserve(paths.size());
		for (const std::filesystem::path& path : paths)
		{
			std::error_code dirError;
			BrowserSlice slice;
			slice.Path = path;
			slice.IsDir = std::filesystem::is_directory(path, dirError);
			// P4-UX14:后缀已经在切片底部单独一行显示,名字里就不再重复
			// (用户:"后缀名放底部这个设计很好,所以文件名部分就不需要再显示后缀名了")。
			slice.Name = path.extension().empty() ? path.filename().string() : path.stem().string();
			const EditorAssetType type = DescribeAssetType(path, slice.IsDir);
			slice.Type = type.Name;
			// P4-U10:切片上显示的类型文案走本地化;glTF/GLB 明确写成"导入源(导入用)",
			// 不再和原生 .wmodel 混在一起(用户 2026-09-21 报的歧义)。
			switch (type.Kind)
			{
				case EditorAssetKind::Folder: slice.TypeLabel = Wui::Tr("asset.file.folder", "Folder"); break;
				case EditorAssetKind::Scene: slice.TypeLabel = Wui::Tr("asset.file.scene", "Scene"); break;
				case EditorAssetKind::Material: slice.TypeLabel = Wui::Tr("asset.file.material", "Material"); break;
				case EditorAssetKind::Model: slice.TypeLabel = Wui::Tr("asset.file.model", "Model"); break;
				case EditorAssetKind::ModelSource:
					slice.TypeLabel = Wui::Tr("asset.file.model_source", "glTF source (import only)"); break;
				case EditorAssetKind::Texture: slice.TypeLabel = Wui::Tr("asset.file.texture", "Texture"); break;
				case EditorAssetKind::Script: slice.TypeLabel = Wui::Tr("asset.file.script", "Script"); break;
				default: slice.TypeLabel = type.Name; break;
			}
			slice.Extension = LowerExtension(path);
			// 列表模式本来就要显示"大小"列:整表统计沿用旧行为;
			// 网格模式只对**可见**切片按需 stat(见 RenderGridSlices),大目录不做全量 stat。
			if (m_Model.ListMode)
			{
				slice.Size = slice.IsDir ? 0 : FileSize(path);
				slice.SizeKnown = true;
			}
			slices.push_back(std::move(slice));
		}

		// 本次只排"面板内这一份显示顺序":m_Model.Listing 与磁盘顺序都不动;
		// 搜索结果保持命中顺序,不参与排序(派工:只在 m_Model.Listing 上排序)。
		if (m_Model.ListMode && !searching && sortColumn >= 0 && slices.size() > 1)
		{
			std::stable_sort(slices.begin(), slices.end(),
				[&](const BrowserSlice& left, const BrowserSlice& right)
				{
					// 大小列:文件夹恒排前(升/降序都成立),只在同组内按字节数比较。
					if (sortColumn == 2 && left.IsDir != right.IsDir)
						return left.IsDir;
					int order = 0;
					if (sortColumn == 0)
					{
						// 名称 = 字典序(ASCII 大小写不敏感;全等时用原文做稳定补充)。
						order = LowerAscii(left.Name).compare(LowerAscii(right.Name));
						if (order == 0)
							order = left.Name.compare(right.Name);
					}
					else if (sortColumn == 1)
						order = left.Extension.compare(right.Extension);
					else
						order = left.Size < right.Size ? -1 : (left.Size > right.Size ? 1 : 0);
					// 同键时按名称、再按完整路径收尾:顺序稳定、可复现(不依赖扫描顺序)。
					if (order == 0)
						order = LowerAscii(left.Name).compare(LowerAscii(right.Name));
					if (order == 0)
						order = left.Path.generic_string().compare(right.Path.generic_string());
					return sortAscending ? order < 0 : order > 0;
				});
		}

		bool itemRightClicked = false;
		// D10:双击"进入文件夹/打开资产"必须**延迟到遍历结束**再执行 ——
		// `paths` 是 m_Model.SearchResults 的引用(Navigate→UpdateSearch 会清空重填),
		// 在循环里直接 OpenItem 会把正在遍历的容器清掉(迭代器失效 → 崩溃;
		// 用户实测"搜索后点击进入文件夹会报错")。
		std::optional<std::filesystem::path> pendingOpen;
		auto interact = [&](const std::filesystem::path& path, const Wui::WuiRect& itemRect, bool isDir)
		{
			const bool selected = m_Model.Selected.find(path) != m_Model.Selected.end();
			const bool hovered = ctx.IsHovered(itemRect);
			// P4-U10:`.gltf/.glb` 是**导入源**而不是可引用资产 —— 悬停直接说清"导入后场景引用
			// 产出的 .wmodel"(用户 2026-09-21:「gltf 和 wmodel 现在有歧义,尤其是网格选取时」)。
			if (hovered && !isDir)
			{
				const std::string extension = LowerExtension(path);
				if (extension == ".gltf" || extension == ".glb")
					Wui::Tooltip(ctx, itemRect, Wui::Tr("panel.content_browser.source_hint",
						"Import source (not referenceable): import it, then reference the produced .wmodel")
						+ "  →  " + path.stem().string() + ".wmodel");
			}
			if (isDir && fileDrag && hovered)
			{
				ctx.DropTarget(itemRect, "file:");
				m_Model.PendingDropDest = path;
				Wui::HighlightOutline(ctx, itemRect, theme.Accent, 2.0f, 2.0f);
			}
			if (ctx.Input().MouseDown[0] && hovered)
			{
				const std::filesystem::path rel = path.lexically_relative(m_Model.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (ctx.IsDoubleClicked(itemRect))
				pendingOpen = path;
			else if (ctx.IsClicked(itemRect))
			{
				if (ctx.Input().Ctrl)
				{
					if (selected) m_Model.Selected.erase(path);
					else m_Model.Selected.insert(path);
				}
				else if (ctx.Input().Shift && !m_Model.LastSelected.empty())
				{
					// 范围选择按**当前显示顺序**(列表排序后就是用户看到的顺序)取区间。
					auto start = std::find_if(slices.begin(), slices.end(),
						[&](const BrowserSlice& slice) { return slice.Path == m_Model.LastSelected; });
					auto end = std::find_if(slices.begin(), slices.end(),
						[&](const BrowserSlice& slice) { return slice.Path == path; });
					if (start != slices.end() && end != slices.end())
					{
						if (std::distance(start, end) < 0) std::swap(start, end);
						for (auto it = start; it <= end; ++it)
							m_Model.Selected.insert(it->Path);
					}
				}
				else
				{
					m_Model.Selected.clear();
					m_Model.Selected.insert(path);
				}
				m_Model.LastSelected = path;
			}
			else if (ctx.Input().MouseClicked[1] && hovered)
			{
				itemRightClicked = true;
				if (!selected)
				{
					m_Model.Selected.clear();
					m_Model.Selected.insert(path);
					m_Model.LastSelected = path;
				}
				m_Model.ContextMenuPath = path;
				m_Model.ContextMenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("browser.context"));
			}
			return selected || hovered;
		};

		if (paths.empty())
		{
			// U2d:两种"空"分开表达 —— "目录本身为空"与"搜索无结果"不是一回事。
			const Wui::WuiRect emptyRect { content.X + 12.0f, content.Y + 12.0f,
				std::max(0.0f, content.W - 24.0f), std::max(0.0f, content.H - 24.0f) };
			if (searching)
			{
				const bool clearRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
					Wui::Tr("panel.content_browser.search.empty_title", "No search results"),
					Wui::Tr("panel.content_browser.search.empty_hint",
						"Nothing matches in this folder or its subfolders. Try another keyword, or clear the search."),
					Wui::Tr("panel.content_browser.search.empty_action", "Clear Search"),
					Wui::HashId("browser.search.clear"), theme);
				if (clearRequested)
				{
					// 真的清掉搜索词并让下一帧重新扫描目录(搜索框缓冲与提交值一起清)。
					m_Model.Search[0] = 0;
					m_Model.SearchEdit.clear();
					m_Model.SearchResults.clear();
					m_Model.ListingDirty = true;
					m_Model.ListingStamp = {};
				}
			}
			else
			{
				const bool createRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
					Wui::Tr("panel.content_browser.empty.title", "This folder is empty"),
					Wui::Tr("panel.content_browser.empty.hint",
						"Create a folder here, or drop a .gltf / .glb model into the window to import it."),
					Wui::Tr("panel.content_browser.empty.action", "New Folder"),
					Wui::HashId("browser.empty.newfolder"), theme);
				if (createRequested)
					CreateFolder(ctx);
			}
		}
		else if (m_Model.ListMode)
			// 列表:常驻表头(名称/类型/大小,点击列头在面板内排序)+ 图标/名称/类型/大小四列。
			RenderListSlices(ctx, content, theme, slices, sortColumn, sortAscending, interact, treeRenameDrawn);
		else
			// 网格:图标 → 名称 → 次级信息(扩展名 · 大小 / 文件夹),切片 ≥96×96。
			RenderGridSlices(ctx, content, theme, slices, interact, treeRenameDrawn);

		// D10:遍历结束后再执行"双击打开" —— 此时 Navigate→UpdateSearch 清空/重填
		// SearchResults 不会再破坏正在遍历的容器(见 pendingOpen 处的说明)。
		if (pendingOpen.has_value())
			OpenItem(*pendingOpen);

		if (ctx.AcceptDrop(&filePayload, "file:"))
		{
			if (!m_Model.PendingDropDest.empty())
			{
				const std::filesystem::path dragged = m_Model.Root / filePayload.substr(5);
				const std::filesystem::path dest = m_Model.PendingDropDest;
				auto sameOrAncestor = [](const std::filesystem::path& a, const std::filesystem::path& b)
				{
					const std::filesystem::path na = a.lexically_normal();
					const std::filesystem::path nb = b.lexically_normal();
					auto ia = na.begin();
					auto ib = nb.begin();
					while (ia != na.end() && ib != nb.end() && *ia == *ib)
					{
						++ia;
						++ib;
					}
					return ia == na.end();
				};
				const bool samePath = dragged == dest;
				const bool sameParent = dragged.parent_path() == dest;
				const bool intoOwnSubtree = std::filesystem::is_directory(dragged) && !samePath && sameOrAncestor(dragged, dest);
				if (!samePath && !sameParent && !intoOwnSubtree)
				{
					const std::filesystem::path movedTo = dest / dragged.filename();
					try
					{
						std::filesystem::rename(dragged, movedTo);
						if (m_Ctx) m_Ctx->RecordOp("browser", "move", dragged.filename().string(), "-> " + dest.string());
						if (m_Model.Selected.erase(dragged) > 0)
							m_Model.Selected.insert(movedTo);
						if (m_Model.LastSelected == dragged)
							m_Model.LastSelected = movedTo;
						if (m_Model.Current == dragged)
						{
							m_Model.Current = movedTo;
							if (m_Model.Search[0])
								UpdateSearch();
						}
						Reveal(dest);
						InvalidateContents();
						SaveState();
					}
					catch (const std::exception& error)
					{
						WLD_CORE_ERROR("Content browser move failed: {0}", error.what());
					}
				}
			}
			m_Model.PendingDropDest.clear();
		}
		else if (!fileDrag)
		{
			m_Model.PendingDropDest.clear();
		}

		// ---- 右键菜单 ----
		const Wui::WuiId popup = Wui::HashId("browser.context");
		if (ctx.IsPopupOpen(popup) && !m_Model.ContextMenuPath.empty())
		{
			struct BrowserItem { std::string Label; std::function<void()> Action; };
			const bool single = m_Model.Selected.size() == 1;
			// P4-UX16:"新建 X"不再挂在**条目**右键菜单上 —— 它建在"当前文件夹",和条目无关
			// (资源管理器同款:新建属于空白处,条目菜单只做对该条目本身的操作)。
			// P4-U13:prefab 的菜单按"它是资产不是文件"来排 —— 打开编辑 / 实例化到当前场景
			// 排在最前(这两个才是日常动作),文件操作(剪切/复制…)跟在后面。
			const bool prefabFile =
				!std::filesystem::is_directory(m_Model.ContextMenuPath)
				&& LowerExtension(m_Model.ContextMenuPath) == ".wprefab";
			std::vector<BrowserItem> items;
			if (prefabFile)
			{
				// P4-U13c:Open = 打开资产窗口(与双击同一条);编辑是单独一条(文档会话)。
				items.push_back({ Wui::Tr("panel.content_browser.item.open_prefab", "Open Prefab Window"),
					[this] { OpenItem(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.edit_prefab", "Edit Prefab"),
					[this]
					{
						const std::filesystem::path contentRoot = m_Model.Root;
						std::error_code ec;
						const std::filesystem::path relative =
							std::filesystem::relative(m_Model.ContextMenuPath, contentRoot, ec);
						const std::string logical =
							ec ? m_Model.ContextMenuPath.generic_string() : relative.generic_string();
						m_Host.OpenPrefabEditor(logical);
					} });
				items.push_back({ Wui::Tr("panel.content_browser.item.instantiate", "Instantiate in Scene"),
					[this]
					{
						const std::filesystem::path contentRoot = m_Model.Root;
						std::error_code ec;
						const std::filesystem::path relative =
							std::filesystem::relative(m_Model.ContextMenuPath, contentRoot, ec);
						const std::string logical =
							ec ? m_Model.ContextMenuPath.generic_string() : relative.generic_string();
						std::string message;
						if (!m_Host.InstantiatePrefabAsset(logical, &message))
							WLD_CORE_WARN("[prefab] instantiate failed: {0}", message);
						else
							WLD_CORE_INFO("[prefab] {0}", message);
					} });
				items.push_back({ Wui::Tr("panel.content_browser.item.rename", "Rename"),
					[this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.reveal", "Show in Explorer"),
					[this] { OpenInExplorer(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.delete", "Delete"),
					[this] { m_Model.ShowDeleteModal = true; } });
			}
			else
			{
				items = {
					{ "Open", [this] { OpenItem(m_Model.ContextMenuPath); } },
					{ "Cut", [this] { Cut(); } },
					{ "Copy", [this] { Copy(); } },
					{ "Paste", [this] { PasteInto(std::filesystem::is_directory(m_Model.ContextMenuPath) ? m_Model.ContextMenuPath : m_Model.Current); } },
					{ "Rename", [this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } },
					{ "Open in Explorer", [this] { OpenInExplorer(m_Model.ContextMenuPath); } },
					{ "Delete", [this] { m_Model.ShowDeleteModal = true; } },
				};
			}
			// 右键菜单走组件(ContextMenu):位置钉住 + 外部点击/Esc 关闭统一处理。
			Wui::WuiRect menuPanel;
			if (Wui::BeginContextMenu(ctx, popup, m_Model.ContextMenuPos, 180.0f, items.size(), &menuPanel, theme))
			{
				for (size_t i = 0; i < items.size(); ++i)
				{
					const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 22, menuPanel.W - 8, 22 };
					if (Wui::ContextMenuItem(ctx, Wui::HashId(("browser.item." + std::string(items[i].Label)).c_str()),
						item, items[i].Label, theme))
					{
						items[i].Action();
						if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser");
						ctx.CloseAllPopups();
					}
				}
				if (ctx.IsKeyPressed(KeyCodes::Escape))
					ctx.ClosePopup(popup);
				Wui::EndContextMenu(ctx, popup, menuPanel, theme);
			}
		}
		else
		{
			m_Model.ContextMenuPath.clear();
			if (ctx.IsPopupOpen(popup))
				ctx.ClosePopup(popup);
		}

		// ---- 内容区空白处右键 ----
		if (ctx.Input().MouseClicked[1] && ctx.IsHovered(content) && !itemRightClicked)
		{
			m_Model.BlankMenuPos = ctx.Input().MousePos;
			ctx.OpenPopup(Wui::HashId("browser.blankcontext"));
		}
		const Wui::WuiId blankPopup = Wui::HashId("browser.blankcontext");
		if (ctx.IsPopupOpen(blankPopup))
		{
			ctx.PushOverlay();
			// P4-UX16:新建收成一行 "New ▶"(清单来自注册表)+ Paste + Refresh。
			const Wui::WuiRect menuPanel { m_Model.BlankMenuPos.x, m_Model.BlankMenuPos.y, 180, 3 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, theme);
			// P4-U7:同上(空白右键菜单)。
			ctx.RegisterOverlayRect(menuPanel);
			const Wui::WuiRect newRow { menuPanel.X + 4, menuPanel.Y + 4, menuPanel.W - 8, 22 };
			if (RenderNewAssetRow(ctx, Wui::HashId("browser.blank.new"), newRow, theme))
				m_NewMenuOwner = (m_NewMenuOwner == 2) ? 0 : 2;
			struct BlankItem { const char* Label; std::function<void()> Action; };
			const std::vector<BlankItem> items = {
				{ "Paste", [this] { PasteInto(m_Model.Current); } },
				{ "Refresh", [this] { InvalidateContents(); if (m_Model.Search[0]) UpdateSearch(); } },
			};
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + (i + 1) * 24, menuPanel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("browser.blank." + std::string(items[i].Label)).c_str()), item, items[i].Label, true, theme))
				{
					items[i].Action();
					if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser-blank");
					ctx.CloseAllPopups();
				}
			}
			Wui::WuiRect newMenuRect;
			if (m_NewMenuOwner == 2)
				newMenuRect = RenderNewAssetItems(ctx, "browser.blank.new.", menuPanel, rect, theme);
			// 父菜单 + 展开的"新建"清单算同一块点击区(否则点子菜单会把父菜单一起关掉)。
			Wui::WuiRect clickBlock = menuPanel;
			if (newMenuRect.W > 0.0f)
			{
				const float right = std::max(menuPanel.X + menuPanel.W, newMenuRect.X + newMenuRect.W);
				const float bottom = std::max(menuPanel.Y + menuPanel.H, newMenuRect.Y + newMenuRect.H);
				clickBlock.X = std::min(clickBlock.X, newMenuRect.X);
				clickBlock.Y = std::min(clickBlock.Y, newMenuRect.Y);
				clickBlock.W = right - clickBlock.X;
				clickBlock.H = bottom - clickBlock.Y;
			}
			ctx.ClosePopupsOnOutsideClick({ blankPopup }, clickBlock);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				if (m_NewMenuOwner == 2)
					m_NewMenuOwner = 0;
				else
					ctx.ClosePopup(blankPopup);
			}
			ctx.PopOverlay();
		}
		else if (m_NewMenuOwner == 2)
		{
			m_NewMenuOwner = 0;
		}

		// ---- 删除确认 ----
		const Wui::WuiId deleteModal = Wui::HashId("browser.delete");
		if (m_Model.ShowDeleteModal && (ctx.Modal() == 0 || ctx.Modal() == deleteModal))
			ctx.SetModal(deleteModal);
		else if (!m_Model.ShowDeleteModal && ctx.Modal() == deleteModal)
			ctx.ClearModal();
		Wui::WuiRect panel;
		bool escapePressed = false;
		// D10-11:与导入位置模态共用 WuiModal 组件(居中/遮罩/标题栏/Esc/按钮条);
		// 文案与 id 逐字不变(Yes → DeleteSelection,No/Esc → 关)。
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = deleteModal;
		frameDesc.Title = "Delete Confirmation";
		frameDesc.Size = { 380.0f, 160.0f };
		if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "Delete " + std::to_string(m_Model.Selected.size()) + " item(s)? This cannot be undone.", theme.Text, 14.0f);
			const Wui::ModalResult result = Wui::ModalFooter(ctx, panel, "Yes", "No",
				Wui::HashId("browser.delete.yes"), Wui::HashId("browser.delete.no"), true, theme);
			if (result == Wui::ModalResult::Confirm)
			{
				DeleteSelection();
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			else if (result == Wui::ModalResult::Cancel || escapePressed)
			{
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
		}

		// ---- U25-M2:E 新建材质向导(面板级模态;外壳按 m_PanelModalOwner 封锁其余面板输入)----
		DrawNewMaterialModal(ctx);

		// ---- 面板级快捷键(放在**最后**判定)----
		// 关键:重命名输入框是在本函数后半段才绘制的,`SetTextInputActive` 也是那时才登记
		// 文本焦点;若在前面判定,本帧的 WuiTextFocus 还是空的 → Ctrl+A 会去全选文件夹
		// (用户实测两次)。放到函数末尾,读到的就是本帧已经登记好的焦点状态。
		const std::vector<std::filesystem::path>& shortcutPaths =
			searching ? m_Model.SearchResults : m_Model.Listing;
		const bool textFocusActive = Wui::WuiTextFocus::Get().Active();
		if (!textFocusActive && ctx.Input().Ctrl && ctx.IsKeyPressed(KeyCodes::A) && ctx.IsHovered(content))
			SelectAll(shortcutPaths);
		if (!textFocusActive && ctx.IsKeyPressed(KeyCodes::Delete) && !m_Model.Selected.empty() && ctx.IsHovered(content))
			m_Model.ShowDeleteModal = true;
		if (!textFocusActive && ctx.IsKeyPressed(KeyCodes::F2) && m_Model.Selected.size() == 1 && ctx.IsHovered(content))
			StartRename(ctx, *m_Model.Selected.begin());

		// ---- P4-UX16:由 OnShortcut(第 2 层路由)排队的"新建"动作 ----
		// 快捷键事件在渲染之外到达,那里没有 ctx;这里执行真正的动作,与菜单走同一条
		// CreateAssetFromRegistry 路径(所以注册表里新增的类型同样自动获得快捷键语义)。
		if (m_PendingNewShortcut != 0)
		{
			const int request = m_PendingNewShortcut;
			m_PendingNewShortcut = 0;
			if (request == 2)
			{
				std::string error;
				if (!CreateAssetFromRegistry("folder", &error))
					NotifyAssetFailure(error);
			}
			else
			{
				// Ctrl+N:打开 `…` 菜单并直接展开"新建"清单(键盘可达,不必先点 ⋯)。
				m_ToolbarMenuOpen = true;
				m_NewMenuOwner = 1;
			}
		}
	}
}

