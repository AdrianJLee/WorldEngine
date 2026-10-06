#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;

namespace ContentBrowserPanelDetail
{
		// PLUG-T3:插件清单文件名(`plugin.we.yaml`)—— 项目插件根下它是"插件清单"类型,
		// 双击/右键走外部 Visual Studio(与 C++ 源码同一条路径)。
bool IsPluginManifestPath(const std::filesystem::path& path){
			return path.filename() == "plugin.we.yaml";
		}


		// 禁用菜单项的第二通道(与内容区右键菜单同一口径):理由常驻在同一个 id 的 a11y 节点上
		// (Register 是 upsert,以最后一次为准),鼠标真停在行上时再弹气泡。灰项不能没有理由。
void RegisterDisabledMenuItem(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& reason){
			if (ctx.IsHovered(rect))
				ctx.SetTooltip(reason);
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "menu-item";
			node.Label = label;
			node.Tooltip = reason;
			node.Rect = rect;
			node.Enabled = false;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}


std::string FormatBytes(size_t bytes){
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


		// M4-TEX P4:纹理源图徽标的短文案(网格胶囊 / 列表类型列后缀共用同一份口径)。
std::string TextureArtifactBadgeText(Editor::TextureArtifactState state){
			switch (state)
			{
				case Editor::TextureArtifactState::Fresh:
					return Wui::Tr("panel.content_browser.badge.texture_baked", "Baked");
				case Editor::TextureArtifactState::Stale:
					return Wui::Tr("panel.content_browser.badge.texture_needs_bake", "Needs bake");
				case Editor::TextureArtifactState::NoSource:
				case Editor::TextureArtifactState::Invalid:
					return Wui::Tr("panel.content_browser.badge.texture_unreadable", "Unreadable");
				case Editor::TextureArtifactState::NoArtifact:
				default:
					return Wui::Tr("panel.content_browser.badge.texture_unbaked", "Unbaked");
			}
		}


		// 产物状态码(无障碍节点 value):脚本按它断言,不用读本地化文字。
const char* TextureArtifactBadgeCode(Editor::TextureArtifactState state){
			switch (state)
			{
				case Editor::TextureArtifactState::Fresh: return "baked";
				case Editor::TextureArtifactState::Stale: return "needs-rebake";
				case Editor::TextureArtifactState::NoSource: return "no-source";
				case Editor::TextureArtifactState::Invalid: return "unreadable";
				case Editor::TextureArtifactState::NoArtifact:
				default: return "unbaked";
			}
		}


		// 判断 candidate 是否等于 root 或位于 root 的子树内。
bool IsWithinOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& root){
			if (candidate == root)
				return true;
			const std::filesystem::path relative = candidate.lexically_relative(root);
			const std::string text = relative.generic_string();
			return !text.empty() && text.rfind("..", 0) != 0;
		}


		// M4-TEX P5:内容浏览器里**不显示**的产物流(目前只有纹理烘焙产物 `.wtexc`)。
		// 它们由 `.wtex` 资产烘出来、运行时按逻辑路径找;用户既不该双击也不该误删,
		// 所以列目录 / 搜索 / a11y 树里一律过滤(用户 2026-09-25:「wtexc 没必要显示在引擎里吧」)。
bool IsHiddenContentArtifact(const std::filesystem::path& path){
			return LowerExtension(path) == ".wtexc";
		}


		// M4-TEX P5:拖放导入的目标名字(重名自动加 -1 / -2 … 后缀,**不覆盖**已有文件)。
std::filesystem::path MakeUniqueFileName(const std::filesystem::path& dir, const std::string& fileName){
			const std::filesystem::path name(fileName);
			std::filesystem::path candidate = dir / name;
			int counter = 1;
			std::error_code existsError;
			while (std::filesystem::exists(candidate, existsError))
				candidate = dir / (name.stem().string() + "-" + std::to_string(counter++)
					+ name.extension().string());
			return candidate;
		}


		// 图片源(png/jpg/jpeg/tga/bmp)= 纹理导入的入口类型。
bool IsTextureSourcePath(const std::filesystem::path& path){
			return DescribeAssetType(path, false).Kind == EditorAssetKind::TextureSource;
		}


		// M4-TEX P5 探针钩子:`WLD_DROP_FILES="a.png;b.png"` 把路径注进**同一个**队列消费点。
		// 为什么不用真 WM_DROPFILES:HDROP 必须由系统在目标进程里分配(收到消息的一方会
		// DragFinish→GlobalFree 它),进程外伪造不可靠。判据完全相同:鼠标位置决定落点目录、
		// 重名去重、写 `.wtex`、选中。真实拖放链路(平台层队列)保持原样。
		// `WLD_DROP_TRIGGER`(可选)= 触发文件路径:它出现之后才注入 —— 让探针先把光标移到目标
		// 区域、把浏览器切到目标目录,从而把"落点由鼠标位置决定"这条规则变成可复现的判据。
void AppendInjectedDroppedFiles(std::vector<std::string>& dropped){
			static bool injected = false;
			if (injected)
				return;
			const char* inject = std::getenv("WLD_DROP_FILES");
			if (inject == nullptr || *inject == '\0')
				return;
			if (const char* trigger = std::getenv("WLD_DROP_TRIGGER"); trigger != nullptr && *trigger != '\0')
			{
				std::error_code triggerError;
				if (!std::filesystem::exists(std::filesystem::path(trigger), triggerError))
					return;
			}
			injected = true;
			const std::string list(inject);
			size_t start = 0;
			size_t added = 0;
			while (start <= list.size())
			{
				const size_t end = list.find(';', start);
				const std::string item = list.substr(start,
					end == std::string::npos ? std::string::npos : end - start);
				if (!item.empty())
				{
					dropped.push_back(item);
					++added;
				}
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			WLD_CORE_INFO("[switch] WLD_DROP_FILES: injected {0} path(s) into the content browser drop queue",
				added);
		}


		// 仅按 ASCII 大小写比较:重命名到"仅大小写不同"的名字在 Windows 上是合法操作,
		// 不能把它当成"同目录已有同名文件"拦掉。
		// P4-UX15:重命名框默认只编辑主名(后缀藏起来),提交时如果用户没写后缀就补回原名后缀。
std::string RenameNameWithExtension(const std::filesystem::path& target, const std::string& stemEdit){
			if (target.extension().empty() || std::filesystem::path(stemEdit).has_extension())
				return stemEdit;
			return stemEdit + target.extension().string();
		}


bool EqualsNoCaseAscii(const std::string& left, const std::string& right){
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
std::string RenameErrorFor(const std::filesystem::path& target, const std::string& newName){
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

}

	// ---- P4-UX14:内容区统一切片(网格)----
	// 每格 = 图标 → 名称(居中、超宽省略号)→ 次级信息(文件夹 / 扩展名 · 大小)。
	// 悬停 = BorderStrong 描边 + 图标轻微放大(≤4%);选中 = 2px Accent 描边 + 名称 Selection 底。
	// 尺寸:切片最小 96×96,列数随面板宽度自适应,间距一律 theme.Pad。
void ContentBrowserPanel::RenderGridSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area, const Wui::WuiTheme& theme, const std::vector<BrowserSlice>& slices, const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact, bool treeRenameDrawn){
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
			// M18(GameUI):网格图标染色表 —— `.slang` 代码蓝、`.wui` 紫罗兰;其余类型保持原色(白),
			// 即"没有专属 tint 的类型"逐字段不变(`.wd`/`.wmat`/`.wmodel`/`.wprefab` 等)。
			const Wui::WuiColor iconTint = slice.Kind == EditorAssetKind::Shader ? kShaderIconTint
				: (slice.Kind == EditorAssetKind::UiDocument ? kUiIconTint
					: Wui::WuiColor { 1, 1, 1, 1 });
			if (icon != 0)
				// W3.6:染色图标走库件 `Wui::Icon`(命令逐字段等价:矩形/uv/tint 都不变;
				// 保留 icon != 0 守卫 ⇒ 不改变"无图不画"的既有行为,空图兜底不在这条路径上)。
				Wui::Icon(ctx, { cell.X + cell.W * 0.5f - iconSize * 0.5f,
						cell.Y + gap - (iconSize - kSliceIconSize) * 0.5f, iconSize, iconSize },
					icon, { 0, 1, 1, -1 }, iconTint, theme);

			// Slang-B1:着色器的常驻类型徽标(与 prefab 徽标同一套画法;代码蓝底)。
			if (slice.Kind == EditorAssetKind::Shader)
			{
				const Wui::LocalizedLabel badge =
					Wui::TrLabel("asset.file.shader", "Material Shader");
				const float badgeSize = infoSize;
				const float badgeW = ctx.MeasureTextWidth(badge.Text, badgeSize) + theme.PadSmall * 2.0f;
				const float badgeH = badgeSize + 4.0f;
				// W3.6:徽标(胶囊底 + 粗体居中字形)收进库件 `Wui::Badge`。文字坐标与手写等价:
				// 水平 (badgeW-tw)/2 = PadSmall、垂直 (badgeH-badgeSize)/2 = 2 ⇒ 与原来两行逐字段相同。
				Wui::Badge(ctx, { cell.X + gap, cell.Y + gap, badgeW, badgeH }, badge.Text,
					kShaderBadgeFill, kShaderBadgeText, theme, badgeSize);
			}

			// P4-U13:prefab 是"可复用实体子树",不是普通文件 —— 给一枚常驻小标签,
			// 让它在网格里一眼可辨(文件图标/名称都看不出它是资产还是随便一个文件)。
			if (slice.Extension == ".wprefab")
			{
				const Wui::LocalizedLabel badge = Wui::TrLabel("asset.type.prefab", "Prefab");
				const float badgeSize = infoSize;
				const float badgeW = ctx.MeasureTextWidth(badge.Text, badgeSize) + theme.PadSmall * 2.0f;
				const float badgeH = badgeSize + 4.0f;
				// W3.6:同上,prefab 徽标(Accent 底 + 白字)也走 `Wui::Badge`。
				Wui::Badge(ctx, { cell.X + gap, cell.Y + gap, badgeW, badgeH }, badge.Text,
					theme.Accent, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, badgeSize);
			}

			// M18(GameUI):`.wui` 游戏 UI 文档 —— 与 `.wprefab`/`.slang` 同做法:一枚常驻类型徽标
			// (自带配色)+ 图标染色,网格里一眼可辨;类型名复用**已存在**的本地化键 `asset.file.ui`。
			if (slice.Kind == EditorAssetKind::UiDocument)
			{
				const Wui::LocalizedLabel badge = Wui::TrLabel("asset.file.ui", "UI Document");
				const float badgeSize = infoSize;
				const float badgeW = ctx.MeasureTextWidth(badge.Text, badgeSize) + theme.PadSmall * 2.0f;
				const float badgeH = badgeSize + 4.0f;
				Wui::Badge(ctx, { cell.X + gap, cell.Y + gap, badgeW, badgeH }, badge.Text,
					kUiBadgeFill, kUiBadgeText, theme, badgeSize);
			}

			// CPPSRC-1:项目源码根下的 `Generated/**`(构建生成物)挂一枚常驻徽标 ——
			// "能改的源码"和"别手改的生成物"在同一格里必须一眼分开。
			if (slice.GeneratedSource)
			{
				const std::string badgeText = Wui::Tr("asset.badge.generated", "Generated");
				const float badgeSize = infoSize;
				const float badgeW = ctx.MeasureTextWidth(badgeText, badgeSize) + theme.PadSmall * 2.0f;
				const float badgeH = badgeSize + 4.0f;
				Wui::Badge(ctx, { cell.X + gap, cell.Y + gap, badgeW, badgeH }, badgeText,
					theme.Warning, Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, badgeSize);
				RegisterGeneratedBadgeNode(slice, { cell.X + gap, cell.Y + gap, badgeW, badgeH });
			}

			// M4-TEX P4:纹理源图的两枚常驻徽标 —— "有资产 / 默认设置"(设置住在 `.wtex` 资产里,
			// 源图自己不再是设置入口)+ 产物状态("已烘焙 / 需重烘 / 未烘焙")。文字保持 2 词内,
			// 免得在 96px 格子里压住居中的图标;完整状态连同 a11y 节点一起给脚本读。
			if (slice.TextureArtifactKnown)
			{
				const float badgeSize = infoSize;
				const float badgeH = badgeSize + 4.0f;
				float badgeX = cell.X + gap;
				const std::string assetText = slice.HasTextureAsset
					? Wui::Tr("panel.content_browser.badge.texture_asset", "Asset")
					: Wui::Tr("panel.content_browser.badge.texture_defaults", "Defaults");
				const float assetW = ctx.MeasureTextWidth(assetText, badgeSize) + theme.PadSmall * 2.0f;
				Wui::Badge(ctx, { badgeX, cell.Y + gap, assetW, badgeH }, assetText,
					slice.HasTextureAsset ? theme.Accent : theme.ActiveBg, theme.Text, theme, badgeSize);
				badgeX += assetW + theme.PadSmall;
				const std::string artifactText = TextureArtifactBadgeText(slice.TextureArtifact);
				const float artifactW = ctx.MeasureTextWidth(artifactText, badgeSize) + theme.PadSmall * 2.0f;
				const bool fresh = slice.TextureArtifact == Editor::TextureArtifactState::Fresh;
				const bool broken = slice.TextureArtifact == Editor::TextureArtifactState::Invalid
					|| slice.TextureArtifact == Editor::TextureArtifactState::NoSource;
				Wui::Badge(ctx, { badgeX, cell.Y + gap, artifactW, badgeH }, artifactText,
					fresh ? theme.Success : (broken ? theme.Danger : (slice.TextureArtifact == Editor::TextureArtifactState::Stale
						? theme.Warning : theme.ActiveBg)),
					Wui::WuiColor { 1.0f, 1.0f, 1.0f, 1.0f }, theme, badgeSize);
				badgeX += artifactW;
				RegisterTextureBadgeNode(slice,
					{ cell.X + gap, cell.Y + gap, std::max(1.0f, badgeX - cell.X - gap), badgeH });
			}

			// 名称(居中;选中时先铺一层 Selection 底再画文字)。
			const float infoY = cell.Y + cell.H - gap - infoSize;
			const float nameY = infoY - theme.PadSmall - nameSize;
			const std::string nameText = EllipsizeToWidth(ctx, slice.Name, textBudget, nameSize);
			const float nameW = ctx.MeasureTextWidth(nameText, nameSize);
			const float nameX = cell.X + (cell.W - nameW) * 0.5f;
			if (selected)
				Wui::PanelBackground(ctx,
					{ nameX - theme.PadSmall, nameY - 1.0f, nameW + theme.PadSmall * 2.0f, nameSize + 4.0f },
					theme.Selection, theme.Radius);
			if (!nameText.empty())
				Wui::Label(ctx, { nameX, nameY }, nameText, theme.Text, nameSize);

			// 次级信息:文件夹 = "文件夹";文件 = 扩展名 · 大小。
			// 大小按需 stat,只统计**可见**切片(大目录在网格模式下不再每帧全量 stat)。
			const uintmax_t bytes = slice.IsDir ? 0 : (slice.SizeKnown ? slice.Size : FileSize(slice.Path));
			const std::string info = slice.IsDir
				? Wui::Tr("panel.content_browser.slice.folder", "Folder")
				// glTF/GLB 用"导入源"而不是裸扩展名:它们在网格视图里必须一眼看出不是可引用资产。
				// prefab / 着色器同理:显示类型名而不是 ".wprefab" / ".slang"。
				// M18(GameUI):`.wui` 同款 —— 显示 "UI Document" 而不是裸 ".wui"。
				// CPPSRC-1:项目源码根下的 C++ 文件同样给类型名(网格里就是"C++ 头文件/源文件",
				// 而不是裸 ".h/.cpp")—— 与列表模式的类型列同一口径。
				: ((slice.Extension == ".gltf" || slice.Extension == ".glb" || slice.Extension == ".wprefab"
						|| slice.Extension == ".slang" || slice.Kind == EditorAssetKind::CppHeader
						|| slice.Kind == EditorAssetKind::CppSource || slice.PluginManifest
						|| slice.Kind == EditorAssetKind::UiDocument)
					? slice.TypeLabel
					: (slice.Extension.empty() ? slice.TypeLabel : slice.Extension)) + " · "
					+ FormatBytes(static_cast<size_t>(bytes));
			const std::string infoText = EllipsizeToWidth(ctx, info, textBudget, infoSize);
			if (!infoText.empty())
				Wui::Label(ctx,
					{ cell.X + (cell.W - ctx.MeasureTextWidth(infoText, infoSize)) * 0.5f, infoY },
					infoText, theme.TextMuted, infoSize);

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
			Wui::HighlightOutline(ctx, cell, stroke, theme.Radius, thickness);

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
void ContentBrowserPanel::RenderListSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area, const Wui::WuiTheme& theme, const std::vector<BrowserSlice>& slices, int& sortColumn, bool& sortAscending, const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact, bool treeRenameDrawn){
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
				Wui::PanelBackground(ctx, row, theme.Selection, theme.Radius);
				// 左侧 2px 强调条:与树的选中语言一致(行内上下各留 1px)。
				Wui::PanelBackground(ctx, { row.X, row.Y + 1.0f, 2.0f, row.H - 2.0f }, theme.Accent, 1.0f);
			}
			else if (hovered)
				Wui::PanelBackground(ctx, row, theme.HoverBg, theme.Radius);

			const uint64_t icon = slice.IsDir ? m_DirIconId : m_FileIconId;
			if (icon != 0)
				// W3.6:列表图标同样走库件 `Wui::Icon`(Slang-B1:与网格同一枚"代码蓝"染色)。
				Wui::Icon(ctx,
					{ row.X + theme.PadSmall, row.Y + (row.H - kListIconSize) * 0.5f, kListIconSize, kListIconSize },
					icon, { 0, 1, 1, -1 },
					slice.Kind == EditorAssetKind::Shader
						? kShaderIconTint : Wui::WuiColor { 1, 1, 1, 1 }, theme);

			const float nameX = row.X + theme.PadSmall * 2.0f + kListIconSize;
			const float nameBudget = std::max(0.0f, nameW - (nameX - row.X) - theme.PadSmall);
			const std::string nameText = EllipsizeToWidth(ctx, slice.Name, nameBudget, theme.FontSizeBody);
			if (!nameText.empty())
				Wui::Label(ctx, { nameX, row.Y + (row.H - theme.FontSizeBody) * 0.5f },
					nameText, theme.Text, theme.FontSizeBody);

			// M4-TEX P4:源图行的类型列追加两枚徽标的文字版(有资产 / 已烘焙),并登记同一枚
			// badge 无障碍节点 —— 列表模式没有胶囊位,但状态同样要能被脚本/读屏读到。
			std::string typeLabel = slice.TypeLabel;
			if (slice.TextureArtifactKnown)
			{
				typeLabel += " · "
					+ (slice.HasTextureAsset
						? Wui::Tr("panel.content_browser.badge.texture_asset", "Asset")
						: Wui::Tr("panel.content_browser.badge.texture_defaults", "Defaults"))
					+ " · " + TextureArtifactBadgeText(slice.TextureArtifact);
				RegisterTextureBadgeNode(slice, { typeX, row.Y, typeW, row.H });
			}
			// CPPSRC-1:列表模式没有胶囊位 —— 生成物标记追加在类型列上(文字版 + 同一枚 a11y 节点)。
			if (slice.GeneratedSource)
			{
				typeLabel += " · " + Wui::Tr("asset.badge.generated", "Generated");
				RegisterGeneratedBadgeNode(slice, { typeX, row.Y, typeW, row.H });
			}
			const std::string typeText = EllipsizeToWidth(ctx, typeLabel,
				std::max(0.0f, typeW - theme.PadSmall * 2.0f), smallSize);
			if (!typeText.empty())
				Wui::Label(ctx, { typeX + theme.PadSmall, row.Y + (row.H - smallSize) * 0.5f },
					typeText, theme.TextMuted, smallSize);

			const std::string sizeText = slice.IsDir
				? std::string("-")
				: FormatBytes(static_cast<size_t>(slice.SizeKnown ? slice.Size : FileSize(slice.Path)));
			const std::string sizeShown = EllipsizeToWidth(ctx, sizeText,
				std::max(0.0f, sizeW - theme.PadSmall * 2.0f), smallSize);
			if (!sizeShown.empty())
				Wui::Label(ctx, { sizeX + theme.PadSmall, row.Y + (row.H - smallSize) * 0.5f },
					sizeShown, theme.TextMuted, smallSize);

			interact(slice.Path, row, slice.IsDir);
			// D10-6:从树菜单发起的重命名画在树行上,内容区不再重复画同 id 输入框。
			if (!treeRenameDrawn && m_Model.RenameTarget == slice.Path)
				RenderRenameField(ctx, slice.Path, { nameX, row.Y + 1.0f, nameBudget, row.H - 2.0f }, theme);
		}
		Wui::EndScrollArea(ctx);
	}

	void ContentBrowserPanel::NotifyAssetWritten(const std::filesystem::path& absolute)
	{
		// m_Model.Root 就是内容根(见 ContentBrowserPanel.h 的 Model::Root 声明),
		// 所以"相对它"就是 AssetCatalog 认的逻辑路径。换算失败(不在内容根内)时静默跳过 ——
		// 那种文件本来也不该进目录。
		std::error_code ec;
		const std::filesystem::path relative = std::filesystem::relative(absolute, m_Model.Root, ec);
		if (ec || relative.empty())
			return;
		m_Host.RefreshAssetCatalog(relative.generic_string());
	}
}
