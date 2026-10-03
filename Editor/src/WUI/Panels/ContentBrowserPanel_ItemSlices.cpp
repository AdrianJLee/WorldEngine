#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;

namespace ContentBrowserPanelDetail
{

Wui::WuiColor MixColor(const Wui::WuiColor& from, const Wui::WuiColor& to, float amount){
			return { from.R + (to.R - from.R) * amount, from.G + (to.G - from.G) * amount,
				from.B + (to.B - from.B) * amount, from.A + (to.A - from.A) * amount };
		}


std::string LowerAscii(std::string text){
			for (char& character : text)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return text;
		}


		// 文本按像素宽度截断(超宽补 '…')。与 WuiWidgets.cpp 内部同名实现同语义
		// (那边是文件内匿名实现,不可跨 TU 复用):逐码点累加,候选宽度 + 12px 余量超过
		// maxWidth 就停;maxWidth <= 0 时不裁剪。放不下一个字符时返回空串。
std::string EllipsizeToWidth(const Wui::WuiContext& ctx, std::string_view text, float maxWidth, float fontSize){
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
std::filesystem::path MakeUniqueAssetPath(const std::filesystem::path& dir, const std::string& base, const std::string& extension){
			std::filesystem::path candidate = dir / (base + extension);
			int counter = 1;
			std::error_code existsError;
			while (std::filesystem::exists(candidate, existsError))
				candidate = dir / (base + " (" + std::to_string(counter++) + ")" + extension);
			return candidate;
		}


		// U25-M2:把任意文本(实体名等)变成合法的资产文件名:剥掉首尾空白、
		// Windows 非法字符替换成 '_';不做其它改写(空结果由调用方兜底)。
std::string SanitizeAssetName(const std::string& raw){
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
std::string AssetBaseName(const std::string& raw){
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

		// Slang-B1:用户把后缀也敲进名称框时不产生 "x.slang.slang"(后缀剥掉)。
std::string ShaderBaseName(const std::string& raw){
			std::string name = SanitizeAssetName(raw);
			for (const std::string suffix : { std::string(".slang") })
			{
				if (name.size() <= suffix.size())
					continue;
				std::string tail = name.substr(name.size() - suffix.size());
				std::transform(tail.begin(), tail.end(), tail.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				if (tail == suffix)
				{
					name = name.substr(0, name.size() - suffix.size());
					break;
				}
			}
			return name;
		}


		// 两行折行(说明文案长于一行时按空白切一刀,第二行超出部分省略)。
std::pair<std::string, std::string> WrapTwoLines(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize){
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
bool ModalActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& tooltip, bool enabled, bool primary, const Wui::WuiTheme& theme){
			const bool hovered = ctx.IsHovered(rect);
			const Wui::WuiColor fill = !enabled ? theme.PanelBg
				: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
			Wui::PanelBackground(ctx, rect, fill, 3.0f);
			Wui::HighlightOutline(ctx, rect,
				enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f);
			const Wui::WuiColor textColor = !enabled ? theme.TextDisabled
				: (primary ? theme.WindowBg : theme.Text);
			Wui::Label(ctx, { rect.X + 9.0f, rect.Y + (rect.H - 15.0f) * 0.5f }, label, textColor, 15.0f);
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

ContentBrowserPanel::ContentBrowserPanel(PanelHost& host) : m_Host(host), m_StatePath(std::string(WLD_LOCAL_DIR) + "wui-browser.json"){
		m_Model.Current = m_Model.Root;
		LoadState();
		RegisterDefaultAssetTypes();
	}


ContentBrowserPanel::~ContentBrowserPanel(){
		// 注册表里的 Create 回调捕获了 this:必须先反注册再让面板析构,否则回调悬空。
		UnregisterDefaultAssetTypes();
		SaveState();
	}


void ContentBrowserPanel::UpdateSearch(){
		m_Model.SearchResults.clear();
		std::string query = m_Model.Search;
		std::transform(query.begin(), query.end(), query.begin(), ::tolower);
		std::error_code searchError;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_Model.Current, std::filesystem::directory_options::skip_permission_denied, searchError))
		{
			if (searchError)
				break;
			// M4-TEX P5:`.wtexc` 是烘出来的**平台产物**,不是资产 —— 不进列表、不参与搜索/选择。
			if (IsHiddenContentArtifact(entry.path()))
				continue;
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			if (name.find(query) != std::string::npos)
				m_Model.SearchResults.push_back(entry.path());
		}
	}


void ContentBrowserPanel::RefreshTree(bool force){
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


void ContentBrowserPanel::RefreshListing(){
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
			// M4-TEX P5:`.wtexc` 是烘出来的平台产物(用户 2026-09-25:「wtexc 没必要显示在引擎里吧」),
			// 列目录时直接过滤 —— 刷新 / 搜索 / 选择 / a11y 树里都不出现。
			if (IsHiddenContentArtifact(entry.path()))
				continue;
			m_Model.Listing.push_back(entry.path());
		}
		std::sort(m_Model.Listing.begin(), m_Model.Listing.end());
		m_Model.ListingPath = m_Model.Current;
		m_Model.ListingStamp = stamp;
		m_Model.ListingDirty = false;
	}


uintmax_t ContentBrowserPanel::FileSize(const std::filesystem::path& path){
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


void ContentBrowserPanel::InvalidateContents(){
		m_Model.DirTreeDirty = true;
		m_Model.ListingDirty = true;
		m_Model.TreeStamp = {};
		m_Model.ListingStamp = {};
	}


void ContentBrowserPanel::SaveState(){
		// PROJ-3/T4-FIX:内容根还没真正挂上时(典型 = 纯启动器模式的哨兵项目根,或项目 assets 缺失)
		// 不要写状态 —— 那会用"空浏览位置"覆盖用户上次的位置。LoadState 在这种根下也无法把
		// 相对路径解析回来,所以这里以"根目录是否存在"为准,而不是靠加载成败记账。
		{
			std::error_code rootError;
			if (m_Model.Root.empty() || !std::filesystem::is_directory(m_Model.Root, rootError))
				return;
		}
		try
		{
			// CPPSRC-1(双根)+ PLUG-T3(三根):每个根各存一份(旧版扁平格式只有内容根,
			// 见 LoadState 的 v2/v3 口径)。
			// 活动根 = 内存里的当前值,另一个根 = 状态桶里上次 Capture 的那份;
			// 采集用的相对路径一律相对**各自的根**,切根不会互相覆盖对方的位置。
			CaptureRootState(m_Model.Scope);
			const auto writeBucket = [this](BrowserRootScope scope)
			{
				const RootState& state = m_RootStates[static_cast<int>(scope)];
				const std::filesystem::path root = RootPathFor(scope);
				Wui::JsonValue node;
				node.type = Wui::JsonValue::Type::Object;
				node.Object.push_back({ "current",
					Wui::JsonValue::MakeString(state.Current.lexically_relative(root).generic_string()) });
				node.Object.push_back({ "treeScroll", Wui::JsonValue::MakeNumber(state.TreeScroll) });
				node.Object.push_back({ "contentScroll", Wui::JsonValue::MakeNumber(state.ContentScroll) });
				Wui::JsonValue open;
				open.type = Wui::JsonValue::Type::Array;
				for (const auto& path : state.TreeOpen)
					open.Array.push_back(Wui::JsonValue::MakeString(path.lexically_relative(root).generic_string()));
				node.Object.push_back({ "treeOpen", std::move(open) });
				return node;
			};
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			// PLUG-T3:持久化升到 v3(新增 roots.projectPlugins);LoadState 仍按 v2 读入
			// (两个根就两个桶,第三个桶缺失 = 该根还没进过,首次进入从干净状态起步)。
			root.Object.push_back({ "version", Wui::JsonValue::MakeNumber(3) });
			root.Object.push_back({ "listMode", Wui::JsonValue::MakeBool(m_Model.ListMode) });
			root.Object.push_back({ "scope", Wui::JsonValue::MakeString(ScopeKey(m_Model.Scope)) });
			Wui::JsonValue roots;
			roots.type = Wui::JsonValue::Type::Object;
			roots.Object.push_back({ "content", writeBucket(BrowserRootScope::Content) });
			if (m_RootStatesReady[static_cast<int>(BrowserRootScope::ProjectSources)])
				roots.Object.push_back({ "projectSources", writeBucket(BrowserRootScope::ProjectSources) });
			if (m_RootStatesReady[static_cast<int>(BrowserRootScope::ProjectPlugins)])
				roots.Object.push_back({ "projectPlugins", writeBucket(BrowserRootScope::ProjectPlugins) });
			root.Object.push_back({ "roots", std::move(roots) });
			std::ofstream stream(m_StatePath, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << root.Dump();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to save content browser state: {0}", error.what());
		}
	}


void ContentBrowserPanel::LoadState(){
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
			// 一个根的桶:v2 从 `roots.<name>` 读,旧版扁平格式整个对象就是内容根的桶
			// (向后兼容:老用户的浏览位置不会丢)。
			const auto readBucket = [this](const Wui::JsonValue& object, BrowserRootScope scope)
			{
				const int index = static_cast<int>(scope);
				RootState& state = m_RootStates[index];
				const std::filesystem::path root = RootPathFor(scope);
				if (const Wui::JsonValue* value = object.Find("current"))
				{
					const std::string relative = value->AsString("");
					if (!relative.empty())
					{
						const std::filesystem::path target = root / std::filesystem::path(relative);
						std::error_code dirError;
						if (std::filesystem::is_directory(target, dirError))
							state.Current = target;
					}
				}
				if (const Wui::JsonValue* value = object.Find("treeScroll"))
					state.TreeScroll = static_cast<float>(value->AsNumber(0));
				if (const Wui::JsonValue* value = object.Find("contentScroll"))
					state.ContentScroll = static_cast<float>(value->AsNumber(0));
				state.TreeOpen.clear();
				if (const Wui::JsonValue* value = object.Find("treeOpen"))
				{
					for (const auto& item : value->Array)
					{
						const std::string relative = item.AsString("");
						if (!relative.empty())
							state.TreeOpen.insert(root / std::filesystem::path(relative));
					}
				}
				m_RootStatesReady[index] = true;
			};
			const Wui::JsonValue* roots = parsed->Find("roots");
			if (roots)
			{
				if (const Wui::JsonValue* content = roots->Find("content"))
					readBucket(*content, BrowserRootScope::Content);
				if (const Wui::JsonValue* sources = roots->Find("projectSources"))
					readBucket(*sources, BrowserRootScope::ProjectSources);
				// v3(PLUG-T3):第三个根的项目插件桶;v2 存档没有这一段 —— 缺省即"从未进过该根"。
				if (const Wui::JsonValue* plugins = roots->Find("projectPlugins"))
					readBucket(*plugins, BrowserRootScope::ProjectPlugins);
			}
			else
			{
				readBucket(*parsed, BrowserRootScope::Content);
			}
			// 构造期恒为内容根:把内容根的桶装回模型(旧版行为逐字保留)。
			m_Model.Scope = BrowserRootScope::Content;
			m_Model.Root = m_Model.ContentRoot;
			ApplyRootState(BrowserRootScope::Content);
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to load content browser state: {0}", error.what());
		}
	}

}
