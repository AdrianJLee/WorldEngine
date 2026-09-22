#include "wldpch.h"
#include "MaterialEditorPanel.h"
#include "EditorAssetCatalog.h"
#include "ViewportPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "World/WUI/WuiWidgets.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#endif

namespace World
{
	namespace
	{
		// ---- 排版(设计单位,4px 基准栅格;与设置面板同一密度)----
		constexpr float kHeaderBaseHeight = 50.0f;   // 动作行 + 名称/路径行
		constexpr float kGroupHeaderHeight = 26.0f;
		constexpr float kRowHeight = 26.0f;
		constexpr float kRowHeightStacked = 46.0f;
		// U24:B1 的"恢复默认"改成**固定占位图标**(we_engine 的 ResetDefaultButton 画回旋箭头,
		// rect 建议 24×24)。旧实现用 56px 文本按钮、且只在偏离默认时出现 —— 点一下整行控件
		// 宽度就跳一次(用户反馈②:恢复默认会突然改变布局)。
		constexpr float kResetWidth = 24.0f;
		constexpr float kTwoColumnMinWidth = 560.0f; // 与模型/预制体面板同口径
		constexpr float kLabelColumnMax = 150.0f;
		constexpr float kStackedThreshold = 380.0f;  // 参数列窄于它 → 标签/控件分两行
		// 预览离屏目标的长边范围(等比缩放;4K 窗口不把预览拖成大目标)。
		constexpr float kPreviewTargetMaxSide = 2048.0f;
		constexpr float kPreviewTargetMinSide = 128.0f;
		// 轨道旋转俯仰限位 ±89°(与模型/预制体面板同口径)。
		constexpr float kPitchLimit = 1.55334f;
		// UV 棋盘格:每个方向 8 格(方案 §1.C「看平铺是否拉伸」)。
		constexpr float kUvCheckerCells = 8.0f;
		constexpr float kPi = 3.14159265358979323846f;
		// U22:参数区"组 = 容器"的排版(组头 + 子项缩进 + 组间留白/描边)。
		constexpr float kGroupIndent = 10.0f;        // 子项缩进:一眼看出这几行属于上面那个组头
		constexpr float kGroupGap = 6.0f;            // 组之间的留白(配合容器描边就是分隔线)
		// U23:预览区的分段标签条(网格|背景|光照|显示)与"仅影响预览显示"标注(U23 §B)。
		constexpr float kPreviewTabHeight = 26.0f;
		constexpr float kPreviewNoteHeight = 16.0f;
		constexpr float kPreviewImageMinSide = 96.0f;  // 空间再紧也留一块能看的预览
		constexpr float kPreviewInlineMinWidth = 240.0f; // 预览设置行:窄于它才改成标签/控件两行
		constexpr float kParamsMinHeight = 120.0f;     // 窄窗单列时给参数区留的最小高度
		// 头部行高(U23:头部只按内容占位,不再留空白带 —— 间距全部走主题令牌)。
		// 高度 = 该行文字的**实际墨迹高度**(名称 13px / 路径 11px 下移 2px / 状态 11px),
		// 这样头部底边就是最后一行文字的底边,头部与内容之间不会多出一条看不见的空白带。
		constexpr float kHeaderTextHeight = 13.0f;     // 材质名 / 路径行
		constexpr float kHeaderStatusHeight = 11.0f;   // 状态行

		// ---- U25-M2(工作流)----
		// 头部动作按钮的最小宽度;实际宽度按文案量出来(中英文都放得下,不硬编码 76)。
		constexpr float kActionMinWidth = 52.0f;
		// 引用者条(常驻一行)+ 展开时的每行高度;列表最多显示 8 条(更多用 "…" 提示)。
		constexpr float kRefsRowHeight = 22.0f;
		constexpr float kRefsItemHeight = 20.0f;
		constexpr size_t kRefsMaxShown = 8;
		// 引用者扫描 TTL(方案:2s;不每帧读盘)。
		constexpr double kRefsTtlSeconds = 2.0;

		// U25-M2:动作按钮(与 U13d 的 ModalActionButton 同一套画法)。
		// 主按钮 = accent 填充;禁用 = 同尺寸弱化绘制,并把"为什么不可用"同时写进
		// 无障碍节点的 Tooltip 与悬停提示 —— 灰按钮不能没有理由。
		bool ActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect,
			const std::string& label, const std::string& tooltip, bool enabled, bool primary,
			const Wui::WuiTheme& theme)
		{
			const bool hovered = ctx.IsHovered(rect);
			const Wui::WuiColor fill = !enabled ? theme.PanelBg
				: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, fill, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect,
				enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f });
			// accent 填充上压深色文字(白字对比度不够);禁用态用 TextDisabled。
			const Wui::WuiColor textColor = !enabled ? theme.TextDisabled
				: (primary ? theme.WindowBg : theme.Text);
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
				{ rect.X + 8.0f, rect.Y + (rect.H - 14.0f) * 0.5f, 0.0f, 0.0f },
				textColor, 0.0f, 1.0f, label, 13.0f, false });
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

		// 逻辑路径的小写扩展名(含点)。
		std::string LowerExtension(const std::string& path)
		{
			const size_t dot = path.find_last_of('.');
			if (dot == std::string::npos)
				return {};
			std::string extension = path.substr(dot);
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension;
		}

		bool IsTextureExtension(const std::string& extension)
		{
			return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga";
		}

		// Save As / 新建向导共用的"基础名":去首尾空白 + 剥掉用户多打的 .wmat 后缀
		// (后缀在落点回显行里常显),不做其它"善意改写" —— 非法字符由校验行说出来。
		std::string MaterialBaseName(const std::string& raw)
		{
			std::string name = raw;
			const auto notSpace = [](unsigned char character) { return std::isspace(character) == 0; };
			name.erase(name.begin(), std::find_if(name.begin(), name.end(), notSpace));
			name.erase(std::find_if(name.rbegin(), name.rend(), notSpace).base(), name.end());
			constexpr size_t kSuffixLength = 5;   // ".wmat"
			if (name.size() > kSuffixLength)
			{
				std::string suffix = name.substr(name.size() - kSuffixLength);
				std::transform(suffix.begin(), suffix.end(), suffix.begin(),
					[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
				if (suffix == ".wmat")
					name = name.substr(0, name.size() - kSuffixLength);
			}
			return name;
		}

		// 预览组的分段标签:行 key → 标签下标(0=网格 1=背景 2=光照 3=显示)。
		int PreviewTabFor(const std::string& key)
		{
			if (key == std::string("preview.mesh"))
				return 0;
			if (key == std::string("preview.bg"))
				return 1;
			if (key.rfind("preview.light", 0) == 0)
				return 2;
			return 3;   // preview.wireframe / preview.normals / preview.uvchecker
		}

		// U22:写进 .wmat 的标量/颜色先量化到 1e-3。
		//
		// 为什么:滑杆/色板产生的是全精度 float(拖一下就是 0.4997164…),而 `.wmat` 写出
		// 用的是 6 位小数(Material.cpp FormatFloat,std::fixed + precision(6)),
		// 保存时的**回读校验**按逐位相等比对(`verify != material->GetDesc()`)——
		// 于是"拖滑杆 → 保存"会被判成写入校验失败:状态栏报 "Save failed: 写入校验失败",
		// 而磁盘其实已经写出去了,**脏标记永远清不掉**(2026-09-22 实测复现,
		// 证据:build/x64-Debug/u22-material/dirty-status.png)。
		// 量化到 3 位小数后,内存值与文件值逐位一致,回读校验通过,手感无差别。
		// 真正的根治在 World 侧(回读校验按容差比较,或写出更高精度)—— 已记进 U22 报告。
		constexpr float kMaterialValueStep = 1000.0f;

		float QuantizeMaterialValue(float value)
		{
			return std::round(value * kMaterialValueStep) / kMaterialValueStep;
		}

		glm::vec4 QuantizeMaterialValue(const glm::vec4& value)
		{
			return { QuantizeMaterialValue(value.x), QuantizeMaterialValue(value.y),
				QuantizeMaterialValue(value.z), QuantizeMaterialValue(value.w) };
		}

		glm::vec3 QuantizeMaterialValue(const glm::vec3& value)
		{
			return { QuantizeMaterialValue(value.x), QuantizeMaterialValue(value.y),
				QuantizeMaterialValue(value.z) };
		}

		std::string ShortenPath(const std::string& path)
		{
			if (path.size() <= 46)
				return path;
			return "…" + path.substr(path.size() - 45);
		}

		std::string ToLowerAscii(const std::string& text)
		{
			std::string result = text;
			std::transform(result.begin(), result.end(), result.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return result;
		}

		// 按 UTF-8 码点边界裁剪到宽度(直接按字节切会切碎中文)。
		std::string EllipsizeToWidth(const Wui::WuiContext& ctx, const std::string& text, float width,
			float fontSize)
		{
			if (text.empty() || width <= 0.0f)
				return {};
			if (ctx.MeasureTextWidth(text, fontSize) <= width)
				return text;
			const std::string dots = "…";
			std::vector<size_t> boundaries;
			for (size_t index = 0; index < text.size(); ++index)
				if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80)
					boundaries.push_back(index);
			for (size_t count = boundaries.size(); count > 0; --count)
			{
				const std::string candidate = text.substr(0, boundaries[count - 1]) + dots;
				if (ctx.MeasureTextWidth(candidate, fontSize) <= width)
					return candidate;
			}
			return dots;
		}

		// U2d:未落盘材质的"另存为"目标路径校验,返回给用户看的具体原因(空字符串 = 可提交)。
		// 与 MaterialLibrary::Save + MaterialIO::WriteFileText 的落盘规则一致:
		//  - 缺 .wmat 后缀会自动补;  - 写入目标为内容根 Game/assets。
		// 名字里真正非法的只有 < > : " | ? *(反斜杠/斜杠是路径分隔符,不是非法字符)。
		std::string NewMaterialPathError(const std::string& raw)
		{
			if (raw.empty())
				return Wui::Tr("panel.material.newpath.error.empty", "Path cannot be empty");
			for (const char character : raw)
			{
				if (character == '<' || character == '>' || character == ':'
					|| character == '"' || character == '|' || character == '?' || character == '*')
					return Wui::Tr("panel.material.newpath.error.illegal",
						"Path contains illegal characters (< > : \" | ? *)");
			}
			std::string key = MaterialLibrary::NormalizePath(raw);
			if (key.size() < 5 || key.substr(key.size() - 5) != ".wmat")
				key.append(".wmat");
			std::error_code existsError;
			const std::filesystem::path contentRoot =
				std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
			if (std::filesystem::exists(contentRoot / key, existsError))
				return Wui::Tr("panel.material.newpath.error.exists", "A file already exists at this path");
			return {};
		}

		// 扫描内容根下的贴图资产(下拉选择用);按扩展名白名单过滤。
		std::vector<std::string> ScanTextureCatalog()
		{
			std::vector<std::string> paths;
			std::error_code ec;
			const std::filesystem::path root = std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
			if (!std::filesystem::exists(root, ec))
				return paths;
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::recursive_directory_iterator(root,
					std::filesystem::directory_options::skip_permission_denied, ec))
			{
				if (!entry.is_regular_file(ec))
					continue;
				std::string extension = entry.path().extension().string();
				std::transform(extension.begin(), extension.end(), extension.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".tga")
					continue;
				const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, ec);
				if (!ec)
					paths.push_back(MaterialLibrary::NormalizePath(relative.generic_string()));
			}
			std::sort(paths.begin(), paths.end());
			return paths;
		}

		std::filesystem::path ContentRootPath()
		{
			return std::filesystem::path(std::string(WLD_GAME_DIR)) / "assets";
		}

		// 逻辑贴图路径是否能在磁盘上找到(绝对路径也支持:编辑器自带资源用绝对路径)。
		bool TextureAssetExists(const std::string& logical)
		{
			if (logical.empty())
				return true;
			std::error_code ec;
			const std::filesystem::path path(MaterialLibrary::NormalizePath(logical));
			if (path.is_absolute())
				return std::filesystem::exists(path, ec);
			return std::filesystem::exists(ContentRootPath() / path, ec);
		}

		// "引用在内容根内":打包/复制项目不会丢的引用。绝对路径与 ".." 逃逸都算问题
		// (U21 校验区第三类:未引用资产 —— 引用了不随项目分发的文件)。
		bool TextureInsideContentRoot(const std::string& logical)
		{
			if (logical.empty())
				return true;
			const std::filesystem::path path(MaterialLibrary::NormalizePath(logical));
			if (path.is_absolute())
				return false;
			for (const std::filesystem::path& part : path)
				if (part == "..")
					return false;
			return true;
		}

		// ---- 参数分组(方案 §1.A:按物理意义分组;组序 = 参数区从上到下)----
		// U23(用户 2026-09-22「预览这个大分类应该和其他的分开;预览只是为了看,并不影响实际效果」):
		// 预览分组**不在**这里 —— 它搬进预览区自己的卡片(DrawPreview)。这张表只剩会影响材质
		// 本身的字段,组标题里因此不会再出现"预览"(与 MaterialEditorPanel::m_SectionOpen 同长)。
		struct GroupDesc
		{
			const char* Key;
			const char* Label;   // 默认英文(zh-CN 由 catalog 覆盖)
			int Index;
		};

		const GroupDesc kGroups[] = {
			{ "base", "Base Appearance", 0 },
			{ "detail", "Surface Detail", 1 },
			{ "emissive", "Emissive", 2 },
			{ "blend", "Transparency & Blend", 3 },
			{ "sampling", "Texture Sampling", 4 },
			{ "advanced", "Advanced", 5 },
		};
		constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));
		static_assert(kGroupCount == 6, "m_SectionOpen 的容量与材质参数分组数必须一致");

		const GroupDesc* FindGroup(const std::string& key)
		{
			for (const GroupDesc& group : kGroups)
				if (key == group.Key)
					return &group;
			return nullptr;
		}

		std::string GroupLabel(const GroupDesc& group)
		{
			return Wui::Tr(std::string("material.group.") + group.Key, group.Label);
		}

		// ---- 参数行表(唯一事实源:分组 / 参数名 / 文案 / 只读 / 是否有默认值)----
		struct RowSpec
		{
			const char* Group;
			const char* Key;        // 参数名:决定控件 id、复位 id、校验定位
			const char* LabelKey;
			const char* LabelEn;
			const char* DocKey;
			const char* DocEn;
			bool ReadOnly;
			bool HasReset;
		};

		const RowSpec kRowSpecs[] = {
			// ---- 预览控制(方案 §1.C;只影响预览,不写进 .wmat)----
			{ "preview", "preview.mesh", "material.prop.preview.mesh", "Mesh",
				"material.prop.preview.mesh.doc",
				"Preview mesh: sphere / cube / plane. Preview only, never saved into the .wmat. Default: Sphere.",
				0, 1 },
			{ "preview", "preview.bg", "material.prop.preview.bg", "Background",
				"material.prop.preview.bg.doc",
				"Preview background: solid colour or gradient. Preview only. Default: Solid.",
				0, 1 },
			{ "preview", "preview.light", "material.prop.preview.light", "Lighting",
				"material.prop.preview.light.doc",
				"Lighting preset: three-point / single directional / none (unlit shows emissive only). "
				"Preview only. Default: Three-Point.",
				0, 1 },
			{ "preview", "preview.light.intensity", "material.prop.preview.light.intensity",
				"Light Intensity", "material.prop.preview.light.intensity.doc",
				"Key light intensity multiplier, 0-4. Preview only. Default: 1.",
				0, 1 },
			{ "preview", "preview.light.azimuth", "material.prop.preview.light.azimuth",
				"Light Azimuth", "material.prop.preview.light.azimuth.doc",
				"Key light azimuth in degrees, -180 to 180. Preview only. Default: 35.",
				0, 1 },
			{ "preview", "preview.light.elevation", "material.prop.preview.light.elevation",
				"Light Elevation", "material.prop.preview.light.elevation.doc",
				"Key light elevation in degrees, -85 to 85. Preview only. Default: 45.",
				0, 1 },
			{ "preview", "preview.wireframe", "material.prop.preview.wireframe", "Wireframe",
				"material.prop.preview.wireframe.doc",
				"Draw a wireframe overlay (every triangle edge) on the preview mesh. Default: off.",
				0, 1 },
			{ "preview", "preview.normals", "material.prop.preview.normals", "Normals",
				"material.prop.preview.normals.doc",
				"Draw per-vertex normal ticks on the preview mesh to spot flipped normals. Default: off.",
				0, 1 },
			{ "preview", "preview.uvchecker", "material.prop.preview.uvchecker", "UV Checker",
				"material.prop.preview.uvchecker.doc",
				"Show an 8x8 checker in UV space instead of the material, to spot stretched or flipped UVs. "
				"Default: off.",
				0, 1 },
			// ---- 基础外观 ----
			{ "base", "base", "material.prop.base", "Base Color", "material.prop.base.doc",
				"Base colour in sRGB; albedo when no albedo texture is set. Default: white.", 0, 1 },
			// 贴图槽放在它所属的物理组里(基础外观 = 基础色 + 它的贴图),而不是单独塞进"采样":
			// 一来这是 Unreal 一类编辑器的分组口径,二来**首屏**就能看到贴图槽
			// (verify-ai-control.py 与用户习惯都不希望"先滚一屏才能换贴图")。
			{ "base", "albedo", "material.prop.albedo", "Albedo Texture", "material.prop.albedo.doc",
				"Base colour texture; decoded from sRGB to linear by the hardware. Empty = base colour only. "
				"Default: none.",
				0, 1 },
			{ "base", "metallic", "material.prop.metallic", "Metallic", "material.prop.metallic.doc",
				"0 = dielectric, 1 = metal; shifts the diffuse/specular balance. Default: 0.", 0, 1 },
			{ "base", "roughness", "material.prop.roughness", "Roughness", "material.prop.roughness.doc",
				"0 = mirror-like highlight, 1 = fully diffuse. Default: 0.5.", 0, 1 },
			// ---- 表面细节 ----
			{ "detail", "normal", "material.prop.normal", "Normal Texture", "material.prop.normal.doc",
				"Tangent-space normal map (linear colour space). Empty = flat surface. Default: none.",
				0, 1 },
			{ "detail", "normal.space", "material.prop.normal.space", "Normal Colour Space",
				"material.prop.normal.space.doc",
				"Normal maps are read as linear data (UNORM, no sRGB decode). Fixed by the engine.",
				1, 0 },
			// ---- 自发光 ----
			{ "emissive", "emissive", "material.prop.emissive", "Emissive", "material.prop.emissive.doc",
				"Self-illumination added after lighting, sRGB, range 0-8. Visible with lights off. "
				"Default: 0,0,0.",
				0, 1 },
			// ---- 透明度与混合 ----
			{ "blend", "blend", "material.prop.blend", "Blend Mode", "material.prop.blend.doc",
				"Opaque writes depth and culls back faces; Transparent blends and does not write depth. "
				"Default: Opaque.",
				0, 1 },
			{ "blend", "doublesided", "material.prop.doublesided", "Double Sided",
				"material.prop.doublesided.doc",
				"Draw back faces as well (no back-face culling); useful for planes. Default: off.",
				0, 1 },
			// ---- 贴图采样 ----
			{ "sampling", "albedo.space", "material.prop.albedo.space", "Albedo Colour Space",
				"material.prop.albedo.space.doc",
				"Albedo textures are created as sRGB; sampling decodes them to linear. Fixed by the engine.",
				1, 0 },
			{ "sampling", "sampler", "material.prop.sampler", "Sampler", "material.prop.sampler.doc",
				"Material textures use linear filtering with repeat wrap; anisotropy comes from "
				"Project Settings. Fixed by the engine.",
				1, 0 },
			// ---- 高级 ----
			{ "advanced", "name", "material.prop.name", "Display Name", "material.prop.name.doc",
				"Display name written into the .wmat. Free text (no default); asset identity is the file path.",
				0, 0 },
			{ "advanced", "format", "material.prop.format", "Format Version", "material.prop.format.doc",
				"Material format version written by the engine on save.", 1, 0 },
			{ "advanced", "revision", "material.prop.revision", "Revision", "material.prop.revision.doc",
				"In-memory revision counter; every edit bumps it so the renderer rebuilds GPU state.",
				1, 0 },
			{ "advanced", "disk", "material.prop.disk", "Disk State", "material.prop.disk.doc",
				"clean = the file on disk matches memory; modified = unsaved edits are kept in memory.",
				1, 0 },
			{ "advanced", "reset_all", "material.prop.reset_all", "Restore Material Defaults",
				"material.prop.reset_all.doc",
				"Reset every material property (not the preview options) to its engine default. "
				"The .wmat is not written until you press Save.",
				0, 0 },
		};
		constexpr int kRowSpecCount = static_cast<int>(sizeof(kRowSpecs) / sizeof(kRowSpecs[0]));

		std::string ResetIdFor(const std::string& key)
		{
			return "material.prop." + key + ".reset";
		}

		// ---- U24:字段的**控件分类**(唯一落点;规则原文见 World/src/World/WUI/WuiWidgets.h)----
		//   DragBarFloat     = 感知型归一化区间(0..1 比例、角度、强度、透明度、平铺系数);
		//   NumberFieldInt   = 计数/索引/ID/大范围整数;StepperInt = 小整数(1..16)。
		// 材质面板的**全部**可编辑数值字段都是感知型 —— 金属度/粗糙度(0..1)、预览光照强度(0..4)
		// 与两个角度(±180°/±85°,顺时针手感)→ DragBarFloat;计数类字段(格式版本 / 修订号)是
		// **只读信息行**(Label+文本值),不是控件。因此本面板既没有 NumberFieldInt/StepperInt 的
		// 调用点,也没有"用进度条显示计数"的行(用户反馈④的落点:进度条不再出现在非感知字段上)。
		// 自发光是**三分量颜色**(0..8),按颜色语义用 Vec3Field(每分量值常显 + 可输入),
		// 与"颜色不是进度条"的既有口径一致。
		enum class RowControl : uint8_t
		{
			Button = 0,     // 动作行(material.prop.reset_all)
			Color = 1,      // 基色(material.base,ColorField)
			Vec3 = 2,       // 三分量颜色(自发光)
			DragBar = 3,    // 感知型归一化标量(DragBarFloat,值常显 + 值区输入 + ↑/↓ 步进)
			Combo = 4,      // 单选下拉
			Segmented = 5,  // 分段按钮
			Checkbox = 6,   // 开关
			Asset = 7,      // 资产下拉(.png / .wmat 槽)
			Text = 8,       // 自由文本
			ReadOnly = 9,   // 只读信息行(含计数类诊断)
		};

		RowControl RowControlFor(const std::string& key)
		{
			if (key == "reset_all")
				return RowControl::Button;
			if (key == "base")
				return RowControl::Color;
			if (key == "emissive")
				return RowControl::Vec3;
			if (key == "metallic" || key == "roughness" || key == "preview.light.intensity"
				|| key == "preview.light.azimuth" || key == "preview.light.elevation")
				return RowControl::DragBar;
			if (key == "blend")
				return RowControl::Combo;
			if (key == "preview.mesh" || key == "preview.bg" || key == "preview.light")
				return RowControl::Segmented;
			if (key == "doublesided" || key == "preview.wireframe" || key == "preview.normals"
				|| key == "preview.uvchecker")
				return RowControl::Checkbox;
			if (key == "albedo" || key == "normal")
				return RowControl::Asset;
			if (key == "name")
				return RowControl::Text;
			return RowControl::ReadOnly;
		}

		// ---- .wmat 的字段默认值(MaterialDesc 的默认构造;恢复默认 = 写回这里)----
		bool FieldDiffersFromDefault(const std::string& key, const MaterialDesc& desc,
			const MaterialDesc& defaults)
		{
			if (key == "base")
				return desc.BaseColor != defaults.BaseColor;
			if (key == "metallic")
				return desc.Metallic != defaults.Metallic;
			if (key == "roughness")
				return desc.Roughness != defaults.Roughness;
			if (key == "emissive")
				return desc.Emissive != defaults.Emissive;
			if (key == "blend")
				return desc.BlendMode != defaults.BlendMode;
			if (key == "doublesided")
				return desc.DoubleSided != defaults.DoubleSided;
			if (key == "albedo")
				return desc.AlbedoTexture != defaults.AlbedoTexture;
			if (key == "normal")
				return desc.NormalTexture != defaults.NormalTexture;
			return false;   // 只读行与自由文本(显示名)没有"默认值"概念
		}

		// ---- 预览派生网格的几何工具 ----
		// 顶点布局与 Mesh::MakeStandardLayout() 一致:position(12) + normal(12) + uv(8)。
		struct MeshBuilder
		{
			std::vector<uint8_t> Vertices;
			std::vector<uint32_t> Indices;

			uint32_t Add(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& uv)
			{
				const float data[8] = { position.x, position.y, position.z,
					normal.x, normal.y, normal.z, uv.x, uv.y };
				const uint32_t index = static_cast<uint32_t>(Vertices.size() / sizeof(data));
				const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
				Vertices.insert(Vertices.end(), bytes, bytes + sizeof(data));
				return index;
			}
			void Triangle(uint32_t a, uint32_t b, uint32_t c)
			{
				Indices.push_back(a);
				Indices.push_back(b);
				Indices.push_back(c);
			}
			void Quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
			{
				Triangle(a, b, c);
				Triangle(a, c, d);
			}
			Ref<Mesh> Build(const std::string& name)
			{
				MeshDesc desc;
				desc.DebugName = name;
				desc.VertexData = Vertices;
				desc.Indices = Indices;
				desc.Layout = Mesh::MakeStandardLayout();
				desc.VertexLayoutId = Mesh::kVertexLayoutStandard;
				return Mesh::Create(desc);
			}
		};

		struct SourceVertex
		{
			glm::vec3 Position { 0.0f };
			glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
			glm::vec2 Uv { 0.0f };
		};

		// 读取标准布局(32B stride)的顶点;不是标准布局时返回空表(调用方跳过派生网格)。
		std::vector<SourceVertex> ReadStandardVertices(const Mesh& mesh)
		{
			std::vector<SourceVertex> vertices;
			if (mesh.GetVertexLayoutId() != Mesh::kVertexLayoutStandard)
				return vertices;
			const std::vector<uint8_t>& data = mesh.GetDesc().VertexData;
			constexpr size_t stride = 32;
			const size_t count = data.size() / stride;
			vertices.reserve(count);
			for (size_t index = 0; index < count; ++index)
			{
				const float* values = reinterpret_cast<const float*>(data.data() + index * stride);
				SourceVertex vertex;
				vertex.Position = { values[0], values[1], values[2] };
				vertex.Normal = { values[3], values[4], values[5] };
				vertex.Uv = { values[6], values[7] };
				vertices.push_back(vertex);
			}
			return vertices;
		}

		glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback = { 0.0f, 1.0f, 0.0f })
		{
			const float length = glm::length(value);
			return length > 1e-6f ? value / length : fallback;
		}

		void OrthonormalBasis(const glm::vec3& direction, glm::vec3* u, glm::vec3* v)
		{
			const glm::vec3 axis = std::abs(direction.y) < 0.9f ? glm::vec3 { 0.0f, 1.0f, 0.0f }
				: glm::vec3 { 1.0f, 0.0f, 0.0f };
			*u = SafeNormalize(glm::cross(axis, direction), { 1.0f, 0.0f, 0.0f });
			*v = SafeNormalize(glm::cross(direction, *u), { 0.0f, 0.0f, 1.0f });
		}

		// 线框:把每个三角形的三条边做成"贴着表面的细带",沿法线抬起一点点避免 z-fighting。
		// 为什么不用多边形线模式:3D 管线是冻结的(World/** 不在本批边界内),RHI 也没有
		// FillMode=Line;用几何表示线框不需要改任何共享接口。
		Ref<Mesh> BuildWireMesh(const Mesh& source, float thickness, float offset)
		{
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return nullptr;
			const std::vector<uint32_t>& indices = source.GetDesc().Indices;
			MeshBuilder builder;
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const SourceVertex& a = vertices[indices[index + 0]];
				const SourceVertex& b = vertices[indices[index + 1]];
				const SourceVertex& c = vertices[indices[index + 2]];
				const SourceVertex* triangle[3] = { &a, &b, &c };
				const glm::vec3 faceNormal = SafeNormalize(glm::cross(b.Position - a.Position,
					c.Position - a.Position));
				for (int edge = 0; edge < 3; ++edge)
				{
					const SourceVertex& start = *triangle[edge];
					const SourceVertex& end = *triangle[(edge + 1) % 3];
					const glm::vec3 edgeDirection = end.Position - start.Position;
					if (glm::length(edgeDirection) < 1e-6f)
						continue;
					const glm::vec3 side = SafeNormalize(glm::cross(SafeNormalize(edgeDirection), faceNormal),
						{ 1.0f, 0.0f, 0.0f }) * (thickness * 0.5f);
					const glm::vec3 normal = SafeNormalize(start.Normal + end.Normal + faceNormal);
					const glm::vec3 startPoint = start.Position + normal * offset;
					const glm::vec3 endPoint = end.Position + normal * offset;
					const uint32_t i0 = builder.Add(startPoint - side, normal, start.Uv);
					const uint32_t i1 = builder.Add(startPoint + side, normal, start.Uv);
					const uint32_t i2 = builder.Add(endPoint + side, normal, end.Uv);
					const uint32_t i3 = builder.Add(endPoint - side, normal, end.Uv);
					builder.Quad(i0, i1, i2, i3);
				}
			}
			return builder.Build("Material.Preview.Wire");
		}

		// 法线可视化:每个顶点一根"三棱柱"细柱(比单面片更抗侧视,不用改着色器)。
		Ref<Mesh> BuildNormalMesh(const Mesh& source, float length, float radius, float offset)
		{
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return nullptr;
			MeshBuilder builder;
			for (const SourceVertex& vertex : vertices)
			{
				const glm::vec3 normal = SafeNormalize(vertex.Normal);
				glm::vec3 u;
				glm::vec3 v;
				OrthonormalBasis(normal, &u, &v);
				const glm::vec3 base = vertex.Position + normal * offset;
				const glm::vec3 tip = base + normal * length;
				glm::vec3 ring[3];
				glm::vec3 tipRing[3];
				for (int corner = 0; corner < 3; ++corner)
				{
					const float angle = static_cast<float>(corner) * (2.0f * kPi / 3.0f);
					const glm::vec3 offsetDir = u * std::cos(angle) + v * std::sin(angle);
					ring[corner] = base + offsetDir * radius;
					tipRing[corner] = tip + offsetDir * radius * 0.15f;
				}
				for (int corner = 0; corner < 3; ++corner)
				{
					const int next = (corner + 1) % 3;
					const uint32_t i0 = builder.Add(ring[corner], normal, vertex.Uv);
					const uint32_t i1 = builder.Add(ring[next], normal, vertex.Uv);
					const uint32_t i2 = builder.Add(tipRing[next], normal, vertex.Uv);
					const uint32_t i3 = builder.Add(tipRing[corner], normal, vertex.Uv);
					builder.Quad(i0, i1, i2, i3);
				}
			}
			return builder.Build("Material.Preview.Normals");
		}

		// UV 棋盘格:按 UV 把每个三角形细分,按格子奇偶分成两张网格(亮格 / 暗格)。
		// 细分步长取"半个格子",所以低模(立方体/平面)也能得到 8×8 的棋盘。
		void BuildCheckerMeshes(const Mesh& source, Ref<Mesh>* light, Ref<Mesh>* dark)
		{
			const std::vector<SourceVertex> vertices = ReadStandardVertices(source);
			if (vertices.empty())
				return;
			const std::vector<uint32_t>& indices = source.GetDesc().Indices;
			MeshBuilder lightBuilder;
			MeshBuilder darkBuilder;
			const float stepSize = 1.0f / (kUvCheckerCells * 2.0f);
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const SourceVertex& a = vertices[indices[index + 0]];
				const SourceVertex& b = vertices[indices[index + 1]];
				const SourceVertex& c = vertices[indices[index + 2]];
				const glm::vec2 minUv = glm::min(glm::min(a.Uv, b.Uv), c.Uv);
				const glm::vec2 maxUv = glm::max(glm::max(a.Uv, b.Uv), c.Uv);
				const int stepsU = std::clamp(
					static_cast<int>(std::ceil((maxUv.x - minUv.x) / stepSize)), 1, 24);
				const int stepsV = std::clamp(
					static_cast<int>(std::ceil((maxUv.y - minUv.y) / stepSize)), 1, 24);
				// UV 域重心坐标(带符号,不做 abs —— 镜像 UV 也要能正确求值);
				// 返回 false = 该点在 UV 三角形之外(不发射,避免"折到边上"的斜向条纹)。
				const float denominator = (b.Uv.y - c.Uv.y) * (a.Uv.x - c.Uv.x)
					+ (c.Uv.x - b.Uv.x) * (a.Uv.y - c.Uv.y);
				if (std::abs(denominator) < 1e-9f)
					continue;   // UV 退化(整条三角形挤成一点):没有棋盘格可看
				const auto barycentric = [&](const glm::vec2& uv, bool* inside, SourceVertex* out)
				{
					const float wA = ((b.Uv.y - c.Uv.y) * (uv.x - c.Uv.x)
						+ (c.Uv.x - b.Uv.x) * (uv.y - c.Uv.y)) / denominator;
					const float wB = ((c.Uv.y - a.Uv.y) * (uv.x - c.Uv.x)
						+ (a.Uv.x - c.Uv.x) * (uv.y - c.Uv.y)) / denominator;
					const float wC = 1.0f - wA - wB;
					if (inside)
						*inside = wA >= -1e-4f && wB >= -1e-4f && wC >= -1e-4f;
					if (!out)
						return;
					out->Position = a.Position * wA + b.Position * wB + c.Position * wC;
					out->Normal = SafeNormalize(a.Normal * wA + b.Normal * wB + c.Normal * wC);
					out->Uv = a.Uv * wA + b.Uv * wB + c.Uv * wC;
				};
				for (int cellV = 0; cellV < stepsV; ++cellV)
				{
					for (int cellU = 0; cellU < stepsU; ++cellU)
					{
						const float u0 = static_cast<float>(cellU) / static_cast<float>(stepsU);
						const float u1 = static_cast<float>(cellU + 1) / static_cast<float>(stepsU);
						const float v0 = static_cast<float>(cellV) / static_cast<float>(stepsV);
						const float v1 = static_cast<float>(cellV + 1) / static_cast<float>(stepsV);
						// 四个角都要落在 UV 三角形内才发射(边界损失 ≤ 一个细分格,肉眼不可见)。
						const glm::vec2 uv[4] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
						SourceVertex corner[4];
						bool allInside = true;
						for (int index = 0; index < 4; ++index)
						{
							bool inside = false;
							barycentric(uv[index], &inside, &corner[index]);
							allInside = allInside && inside;
						}
						if (!allInside)
							continue;
						// 两个子三角形各自按**自己的重心 UV** 取格子奇偶(镜像 UV 也正确)。
						const int triangles[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
						for (const int* triangle : triangles)
						{
							const glm::vec2 centroid = (uv[triangle[0]] + uv[triangle[1]] + uv[triangle[2]])
								/ 3.0f;
							const int cellX = static_cast<int>(std::floor(centroid.x * kUvCheckerCells));
							const int cellY = static_cast<int>(std::floor(centroid.y * kUvCheckerCells));
							MeshBuilder& builder = (((cellX + cellY) & 1) == 0) ? lightBuilder : darkBuilder;
							uint32_t ids[3];
							for (int index = 0; index < 3; ++index)
							{
								const SourceVertex& source = corner[triangle[index]];
								ids[index] = builder.Add(source.Position, source.Normal, source.Uv);
							}
							builder.Triangle(ids[0], ids[1], ids[2]);
						}
					}
				}
			}
			if (light)
				*light = lightBuilder.Build("Material.Preview.CheckerLight");
			if (dark)
				*dark = darkBuilder.Build("Material.Preview.CheckerDark");
		}

		// 预览灯光预设 → LightUniforms(走 Renderer3D::BuildLightRig,与场景同一条打包口径)。
		// 为什么补光/轮廓光用点光:方向光容量 = 2 且 BuildLightRig 只取前 1 个方向光,
		// 三点光只能"1 个主方向光 + 2 个点光"。
		LightUniforms BuildPreviewLightUniforms(int preset, float intensity, float azimuthDeg, float elevationDeg)
		{
			const float azimuth = glm::radians(azimuthDeg);
			const float elevation = glm::radians(elevationDeg);
			const glm::vec3 toLight { std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
				std::cos(elevation) * std::cos(azimuth) };
			std::vector<DirectionalLightData> directional;
			std::vector<PointLightData> point;
			AmbientLightData ambient;
			if (preset == 0)
			{
				directional.push_back({ glm::vec3 { 1.0f }, intensity, -toLight, false });
				point.push_back({ glm::vec3 { 1.0f }, intensity * 0.35f, { -1.9f, 0.7f, 1.5f }, 9.0f });
				point.push_back({ glm::vec3 { 1.0f }, intensity * 0.45f, { 0.5f, 1.3f, -2.1f }, 9.0f });
				ambient.Intensity = 0.22f;
			}
			else if (preset == 1)
			{
				directional.push_back({ glm::vec3 { 1.0f }, intensity, -toLight, false });
				ambient.Intensity = 0.12f;
			}
			else
			{
				// 无光:只看自发光(环境光也置 0)。
				ambient.Intensity = 0.0f;
			}
			const bool glDepthConvention = Renderer::GetBackendName() != "vulkan";
			return Renderer3D::BuildLightRig(directional, point, &ambient, glDepthConvention).Uniforms;
		}

		// ---- 无障碍辅助:补标签/悬停说明(值/矩形仍以控件自己登记的为准)----
		void AnnotateNode(Wui::WuiContext& ctx, Wui::WuiId id, const std::string& label,
			const std::string& tooltip)
		{
			const Wui::WuiAccessNode* live = Wui::WuiAccessibility::Get().Find(id);
			Wui::WuiAccessNode node;
			if (live)
				node = *live;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			if (live && !live->Label.empty())
				node.Label = live->Label;
			else
				node.Label = label;
			node.Tooltip = tooltip;
			if (!live)
			{
				node.Kind = "text";
				node.Value = label;
			}
			node.Focused = ctx.Focus() == id;
			Wui::WuiAccessibility::Get().Register(node);
		}

		void RegisterReadOnlyNode(Wui::WuiId id, const std::string& label, const std::string& value,
			const Wui::WuiRect& rect, const std::string& tooltip)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = label;
			node.Value = value;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
	}

	MaterialEditorPanel::MaterialEditorPanel()
		: MaterialEditorPanel(std::string())
	{
	}

	MaterialEditorPanel::MaterialEditorPanel(std::string materialPath)
	{
		// 预览资源属于当前设备:设备释放(后端切换/关闭)前必须把句柄放掉,
		// 否则会在设备之后析构(独立窗口崩溃那次的同类问题)。
		Renderer::RegisterDeviceReleaseHook(this, [this]
		{
			ReleaseGpuResources();
			// 设备重建后注册表整表清空:句柄与注册槽位一起作废。
			m_PreviewTextureId = 0;
			m_UiTextureGeneration = 0;
		});
		SetMaterialPathForPanel(materialPath);
	}

	void MaterialEditorPanel::SetMaterialPathForPanel(const std::string& path)
	{
		const std::string key = path.empty() ? std::string("(unsaved)") : MaterialLibrary::NormalizePath(path);
		m_PanelId = "material:" + key;
		// U2d:换文档(打开/首次保存)后重置"路径校验已被触发"状态 —— 面板一进来不该标红空路径。
		m_NewPathAttempted = false;
		std::filesystem::path file(key);
		std::string name = file.stem().string();
		if (name.empty())
			name = "Material";
		m_PanelTitle = "Material - " + name;
		m_Search.clear();
		m_ScrollY = 0.0f;
		m_RevealField.clear();
		m_RevealFrames = 0;
		m_SyncNameBuffer = true;
	}

	MaterialEditorPanel::~MaterialEditorPanel()
	{
		Renderer::UnregisterDeviceReleaseHook(this);
		// 面板关闭时设备与帧循环通常还活着:延迟释放,避免销毁在飞命令引用着的资源。
		ReleaseGpuResources(/*defer*/ true);
	}

	void MaterialEditorPanel::OpenMaterial(const std::string& path)
	{
		std::string error;
		Ref<Material> material = MaterialLibrary::Get().Load(path, &error);
		if (!material)
		{
			m_Status = Wui::Tr("panel.material.status.load_failed", "Load failed: ") + error;
			m_StatusIsError = true;
			return;
		}
		// 已有未保存改动时换目标:提示并保留原材质(不自动丢弃用户改动)。
		if (m_Material && m_Material->IsDirty() && m_Material->GetPath() != path)
		{
			m_Status = Wui::Tr("panel.material.status.unsaved_switch",
				"Current material has unsaved changes (switched to the new material; the old edits are kept in memory)");
			m_StatusIsError = true;
		}
		else
		{
			m_Status = Wui::Tr("panel.material.status.opened", "Opened ") + path;
			m_StatusIsError = false;
		}
		m_Material = material;
		m_Path = material->GetPath();
		SetMaterialPathForPanel(m_Path);
		// 校验缓存按路径/Revision 失效,换文档要立刻重算。
		m_ValidationRevision = 0;
		m_ValidationPath.clear();
		RefreshCatalog();
		RefreshPickerIndices();
		WLD_CORE_INFO("[material-ui] opened material '{0}'", m_Path);
	}

	void MaterialEditorPanel::RefreshCatalog()
	{
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (now - m_CatalogRefreshTime < 1.5)
			return;
		m_CatalogRefreshTime = now;
		m_MaterialPaths = MaterialLibrary::Get().ScanMaterials();
		m_TexturePaths = ScanTextureCatalog();
		RefreshPickerIndices();
	}

	void MaterialEditorPanel::RefreshPickerIndices()
	{
		if (!m_Material)
			return;
		const MaterialDesc& desc = m_Material->GetDesc();
		m_MaterialPickIndex = -1;
		for (size_t i = 0; i < m_MaterialPaths.size(); ++i)
			if (m_MaterialPaths[i] == m_Path)
				m_MaterialPickIndex = static_cast<int>(i);
		// 贴图下拉的选项是 "(无)" + m_TexturePaths,所以选中索引要 **+1** 对齐;
		// 之前少了这个偏移,选了一张贴图后下一帧索引回算成 0/错位 → 显示/生效成另一张。
		m_AlbedoPickIndex = 0;
		for (size_t i = 0; i < m_TexturePaths.size(); ++i)
			if (m_TexturePaths[i] == desc.AlbedoTexture)
				m_AlbedoPickIndex = static_cast<int>(i) + 1;
		m_NormalPickIndex = 0;
		for (size_t i = 0; i < m_TexturePaths.size(); ++i)
			if (m_TexturePaths[i] == desc.NormalTexture)
				m_NormalPickIndex = static_cast<int>(i) + 1;
	}

	bool MaterialEditorPanel::FieldModified(const std::string& key) const
	{
		if (!m_Material)
			return false;
		const MaterialDesc defaults;
		return FieldDiffersFromDefault(key, m_Material->GetDesc(), defaults);
	}

	int MaterialEditorPanel::GroupModifiedCount(const std::string& groupKey) const
	{
		int count = 0;
		for (int index = 0; index < kRowSpecCount; ++index)
		{
			const RowSpec& spec = kRowSpecs[index];
			if (groupKey != spec.Group || !spec.HasReset || spec.ReadOnly)
				continue;
			if (groupKey == std::string("preview"))
			{
				if (PreviewOptionModified(spec.Key))
					++count;
				continue;
			}
			if (FieldModified(spec.Key))
				++count;
		}
		return count;
	}

	bool MaterialEditorPanel::PreviewOptionModified(const std::string& key) const
	{
		if (key == "preview.mesh")
			return m_PreviewMesh != PreviewMesh::Sphere;
		if (key == "preview.bg")
			return m_PreviewBackground != PreviewBackground::Solid;
		if (key == "preview.light")
			return m_PreviewLighting != PreviewLighting::ThreePoint;
		if (key == "preview.light.intensity")
			return m_LightIntensity != 1.0f;
		if (key == "preview.light.azimuth")
			return m_LightAzimuth != 35.0f;
		if (key == "preview.light.elevation")
			return m_LightElevation != 45.0f;
		if (key == "preview.wireframe")
			return m_ShowWireframe;
		if (key == "preview.normals")
			return m_ShowNormals;
		if (key == "preview.uvchecker")
			return m_ShowUvChecker;
		return false;
	}

	void MaterialEditorPanel::SetFieldToDefault(const std::string& key)
	{
		if (!m_Material)
			return;
		// 预览选项不写进 .wmat:复位只动面板状态。
		if (key == "preview.mesh")
			m_PreviewMesh = PreviewMesh::Sphere;
		else if (key == "preview.bg")
			m_PreviewBackground = PreviewBackground::Solid;
		else if (key == "preview.light")
			m_PreviewLighting = PreviewLighting::ThreePoint;
		else if (key == "preview.light.intensity")
			m_LightIntensity = 1.0f;
		else if (key == "preview.light.azimuth")
			m_LightAzimuth = 35.0f;
		else if (key == "preview.light.elevation")
			m_LightElevation = 45.0f;
		else if (key == "preview.wireframe")
			m_ShowWireframe = false;
		else if (key == "preview.normals")
			m_ShowNormals = false;
		else if (key == "preview.uvchecker")
			m_ShowUvChecker = false;
		else
		{
			// 材质字段:走 Material 的 setter(Revision 自增 → 渲染侧缓存失效),与手动编辑同一条路径。
			const MaterialDesc defaults;
			const uint32_t revision = m_Material->GetRevision();
			if (key == "base")
				m_Material->SetBaseColor(defaults.BaseColor);
			else if (key == "metallic")
				m_Material->SetMetallic(defaults.Metallic);
			else if (key == "roughness")
				m_Material->SetRoughness(defaults.Roughness);
			else if (key == "emissive")
				m_Material->SetEmissive(defaults.Emissive);
			else if (key == "blend")
				m_Material->SetBlendMode(defaults.BlendMode);
			else if (key == "doublesided")
				m_Material->SetDoubleSided(defaults.DoubleSided);
			else if (key == "albedo")
				m_Material->SetAlbedoTexture(defaults.AlbedoTexture);
			else if (key == "normal")
				m_Material->SetNormalTexture(defaults.NormalTexture);
			else
				return;
			// 与手动编辑同一条 dirty 口径:真的改了值才标脏(已等于默认值时不产生"假脏")。
			if (m_Material->GetRevision() != revision)
				m_Material->MarkDirty(true);
		}
	}

	void MaterialEditorPanel::ResetAllMaterialFields()
	{
		if (!m_Material)
			return;
		for (const RowSpec& spec : kRowSpecs)
		{
			if (!spec.HasReset || spec.ReadOnly || std::string(spec.Group) == "preview")
				continue;
			SetFieldToDefault(spec.Key);
		}
		m_Status = Wui::Tr("panel.material.status.reset_defaults",
			"Restored every material parameter to its default (not saved yet; press Save to write the .wmat)");
		m_StatusIsError = false;
	}

	void MaterialEditorPanel::RefreshValidation(double now)
	{
		m_Validation.clear();
		if (!m_Material)
			return;
		const MaterialDesc& desc = m_Material->GetDesc();
		const auto checkTexture = [&](const std::string& logical, const char* field,
			const std::string& label)
		{
			if (logical.empty())
				return;
			if (!TextureAssetExists(logical))
			{
				m_Validation.push_back({ field, label + ": " + logical, "missing" });
				return;
			}
			if (!TextureInsideContentRoot(logical))
				m_Validation.push_back({ field, label + ": " + logical, "unreferenced" });
		};
		checkTexture(desc.AlbedoTexture, "albedo", Wui::Tr("panel.material.validation.missing_albedo",
			"Albedo texture not found"));
		checkTexture(desc.NormalTexture, "normal", Wui::Tr("panel.material.validation.missing_normal",
			"Normal texture not found"));

		const auto checkRange = [&](const char* field, const char* label, float value, float minimum,
			float maximum)
		{
			if (value >= minimum && value <= maximum)
				return;
			char buffer[128] = {};
			std::snprintf(buffer, sizeof(buffer), "%s out of range [%g, %g]: %g", label,
				static_cast<double>(minimum), static_cast<double>(maximum), static_cast<double>(value));
			m_Validation.push_back({ field, buffer, "range" });
		};
		checkRange("base", "BaseColor R", desc.BaseColor.r, 0.0f, 1.0f);
		checkRange("base", "BaseColor G", desc.BaseColor.g, 0.0f, 1.0f);
		checkRange("base", "BaseColor B", desc.BaseColor.b, 0.0f, 1.0f);
		checkRange("base", "BaseColor A", desc.BaseColor.a, 0.0f, 1.0f);
		checkRange("metallic", "Metallic", desc.Metallic, 0.0f, 1.0f);
		checkRange("roughness", "Roughness", desc.Roughness, 0.0f, 1.0f);
		checkRange("emissive", "Emissive R", desc.Emissive.r, 0.0f, 8.0f);
		checkRange("emissive", "Emissive G", desc.Emissive.g, 0.0f, 8.0f);
		checkRange("emissive", "Emissive B", desc.Emissive.b, 0.0f, 8.0f);
		m_ValidationRevision = m_Material->GetRevision();
		m_ValidationPath = m_Path;
		m_ValidationTime = now;
	}

	void MaterialEditorPanel::ReleaseGpuResources(bool defer)
	{
		if (defer)
		{
			// 帧在飞:把旧句柄搬到延迟释放队列(与 MaterialTextureCache::Invalidate /
			// SceneRenderer::OnResize 同一条路径)。GL 立即执行,Vulkan 等帧栅栏。
			std::array<Rhi::Handle<Rhi::CommandBuffer>, Renderer::FramesInFlight> commands {};
			for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
				commands[slot] = m_PreviewCommands[slot];
			Renderer::QueueRelease([pass = m_PreviewPass, framebuffer = m_PreviewFramebuffer,
				color = m_PreviewColor, entityId = m_PreviewEntityId, depth = m_PreviewDepth,
				colorMsaa = m_PreviewColorMsaa, entityMsaa = m_PreviewEntityMsaa,
				depthMsaa = m_PreviewDepthMsaa, commands, cameraBuffer = m_PreviewCameraBuffer,
				lightBuffer = m_PreviewLightBuffer, cameraSet = m_PreviewCameraSet]() {});
		}
		m_PreviewPass = nullptr;
		m_PreviewFramebuffer = nullptr;
		m_PreviewColor = nullptr;
		m_PreviewEntityId = nullptr;
		m_PreviewDepth = nullptr;
		m_PreviewColorMsaa = nullptr;
		m_PreviewEntityMsaa = nullptr;
		m_PreviewDepthMsaa = nullptr;
		for (Rhi::Handle<Rhi::CommandBuffer>& command : m_PreviewCommands)
			command = nullptr;
		m_PreviewCameraBuffer = nullptr;
		m_PreviewLightBuffer = nullptr;
		m_PreviewCameraSet = nullptr;
		m_GpuDevice = nullptr;
		m_GpuTargetW = 0;
		m_GpuTargetH = 0;
	}

	void MaterialEditorPanel::EnsureGpuResources()
	{
		Rhi::Handle<Rhi::Device> device = Renderer::GetDevice();
		if (!device)
			return;
		if (m_GpuDevice == device.get() && m_PreviewFramebuffer
			&& m_GpuTargetW == m_PreviewTargetW && m_GpuTargetH == m_PreviewTargetH)
			return;
		// 尺寸变了:只重建 GPU 资源,**保留注册表槽位**(id 不变;Update 会推进内容代,
		// WUI 后端的描述符集缓存随之失效 —— 不会采样到已销毁贴图)。
		// 旧资源走延迟释放:本帧之前提交的命令可能还在引用它们(见 ReleaseGpuResources 注释)。
		const uint64_t registryId = m_PreviewTextureId;
		ReleaseGpuResources(/*defer*/ true);
		m_PreviewTextureId = registryId;
		m_GpuDevice = device.get();
		m_GpuTargetW = m_PreviewTargetW;
		m_GpuTargetH = m_PreviewTargetH;

		// 预览目标:与 SceneRenderer 同样的结构(颜色 + entity id + 深度),
		// 这样 Renderer3D 的管线(它就是按这个结构建的)可以直接用。
		// P4-4b:rendering.msaa>1 时升级为与场景通道**完全相同**的五附件结构 ——
		// 颜色 / 实体 id / 深度多采样,再把颜色 / 实体 id resolve 到各自的单采样纹理;
		// WUI 采样与抓图读的仍是单采样 m_PreviewColor,语义不变。
		const Rhi::SampleCount previewSamples = static_cast<Rhi::SampleCount>(RenderSettings::Msaa());
		const bool multisampled = previewSamples != Rhi::SampleCount::Count1;

		Rhi::TextureDesc colorDesc;
		colorDesc.Type = Rhi::TextureType::Texture2D;
		colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		colorDesc.Extent = { m_PreviewTargetW, m_PreviewTargetH, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		colorDesc.DebugName = "Material.Preview.Color";
		m_PreviewColor = device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc = colorDesc;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		entityDesc.DebugName = "Material.Preview.EntityId";
		m_PreviewEntityId = device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc = colorDesc;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
		depthDesc.DebugName = "Material.Preview.Depth";
		if (multisampled)
		{
			// 多采样附件(只做绘制附件,内容由通道末的 resolve 写进上面的单采样纹理)。
			Rhi::TextureDesc msaaColorDesc = colorDesc;
			msaaColorDesc.Samples = previewSamples;
			msaaColorDesc.Usage = Rhi::TextureUsageColorAttachment;
			msaaColorDesc.DebugName = "Material.Preview.ColorMSAA";
			m_PreviewColorMsaa = device->CreateTexture(msaaColorDesc);
			Rhi::TextureDesc msaaEntityDesc = msaaColorDesc;
			msaaEntityDesc.Format = Rhi::Format::R32_SINT;
			msaaEntityDesc.DebugName = "Material.Preview.EntityIdMSAA";
			m_PreviewEntityMsaa = device->CreateTexture(msaaEntityDesc);
			Rhi::TextureDesc msaaDepthDesc = msaaColorDesc;
			msaaDepthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
			msaaDepthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
			msaaDepthDesc.DebugName = "Material.Preview.DepthMSAA";
			m_PreviewDepthMsaa = device->CreateTexture(msaaDepthDesc);
		}
		else
		{
			// msaa==1:单采样深度就是附件(与旧代码逐字节一致)。
			m_PreviewDepth = device->CreateTexture(depthDesc);
		}

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = previewSamples;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::Undefined;
		// 预览纹理在本通道结束后立刻被 WUI 当采样贴图使用,所以"可采样"的那张附件
		// FinalLayout 直接声明为 ShaderReadOnly:渲染通道会做隐式转换。之前声明的是
		// ColorAttachment,文本又要手动转一次 ShaderReadOnly,而后端只发隐式转换
		// (手动屏障因布局一致被跳过),于是 UI 采样到"布局未就绪"的纹理 → 预览闪烁
		// (且抓图读到垃圾数据)。msaa>1 时被采样的是 resolve 目标(附件 3),多采样
		// 颜色附件只做绘制,因此停在 ColorAttachment。
		color.FinalLayout = multisampled ? Rhi::AttachmentLayout::ColorAttachment
			: Rhi::AttachmentLayout::ShaderReadOnly;
		color.Clear.Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = previewSamples;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::Undefined;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		const int minusOne = -1;
		std::memcpy(&entityId.Clear.Color, &minusOne, sizeof(int));
		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = previewSamples;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::Undefined;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.Clear.IsDepthStencil = true;
		depth.Clear.DepthStencil.Depth = 1.0f;
		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = { { 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment } };
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		if (multisampled)
		{
			// 3 = 单采样颜色 resolve(即 m_PreviewColor,离场隐式转 ShaderReadOnly 供 WUI 采样),
			// 4 = 单采样实体 id resolve;与场景通道 / Renderer2D / Renderer3D 兼容通道同结构。
			Rhi::RenderPassAttachment colorResolve;
			colorResolve.Format = Rhi::Format::R8G8B8A8_UNORM;
			colorResolve.Samples = Rhi::SampleCount::Count1;
			colorResolve.Load = Rhi::LoadOp::DontCare;   // resolve 会整体覆盖
			colorResolve.Store = Rhi::StoreOp::Store;
			colorResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			colorResolve.FinalLayout = Rhi::AttachmentLayout::ShaderReadOnly;
			Rhi::RenderPassAttachment entityResolve;
			entityResolve.Format = Rhi::Format::R32_SINT;
			entityResolve.Samples = Rhi::SampleCount::Count1;
			entityResolve.Load = Rhi::LoadOp::DontCare;
			entityResolve.Store = Rhi::StoreOp::Store;
			entityResolve.InitialLayout = Rhi::AttachmentLayout::Undefined;
			entityResolve.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
			passDesc.Attachments.push_back(colorResolve);    // 3
			passDesc.Attachments.push_back(entityResolve);   // 4
			subpass.ResolveAttachments = { 3, 4 };
		}
		passDesc.Subpasses = { subpass };
		passDesc.DebugName = "Material.PreviewPass";
		m_PreviewPass = device->CreateRenderPass(passDesc);

		Rhi::FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = m_PreviewPass;
		framebufferDesc.Extent = { m_PreviewTargetW, m_PreviewTargetH };
		// 顺序必须与渲染通道附件表 1:1(Vulkan 要求 framebuffer 附件数/顺序与通道一致)。
		if (multisampled)
			framebufferDesc.Attachments = { m_PreviewColorMsaa, m_PreviewEntityMsaa, m_PreviewDepthMsaa,
				m_PreviewColor, m_PreviewEntityId };
		else
			framebufferDesc.Attachments = { m_PreviewColor, m_PreviewEntityId, m_PreviewDepth };
		framebufferDesc.DebugName = "Material.PreviewFramebuffer";
		m_PreviewFramebuffer = device->CreateFramebuffer(framebufferDesc);

		// P4-UX16b:每条帧槽位一条(见头文件说明)。调试名带槽位号,取证时能看出是哪一份。
		for (uint32_t slot = 0; slot < Renderer::FramesInFlight; ++slot)
			m_PreviewCommands[slot] = device->CreateCommandBuffer("Material.Preview#" + std::to_string(slot));

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		cameraDesc.DebugName = "Material.Preview.Camera";
		m_PreviewCameraBuffer = device->CreateBuffer(cameraDesc);

		// U21:预览灯光 UBO(set0 binding 2)。自建 set0 的调用方必须把相机(0)/灯光(2)/
		// 阴影贴图(3)写在**同一次 Update** 里(GL 后端是整体替换语义)。
		Rhi::BufferDesc lightDesc;
		lightDesc.Size = sizeof(LightUniforms);
		lightDesc.Usage = Rhi::BufferUsageUniform;
		lightDesc.Memory = Rhi::MemoryHint::HostVisible;
		lightDesc.DebugName = "Material.Preview.Light";
		m_PreviewLightBuffer = device->CreateBuffer(lightDesc);

		m_PreviewCameraSet = device->CreateDescriptorSet(Renderer::GetGlobalDescriptorSetLayout());
		if (m_PreviewCameraSet)
		{
			std::vector<Rhi::DescriptorWrite> writes;
			Rhi::DescriptorWrite camera;
			camera.Binding = 0;
			camera.Type = Rhi::DescriptorType::UniformBuffer;
			camera.Buffer = m_PreviewCameraBuffer;
			writes.push_back(camera);
			for (Rhi::DescriptorWrite& lighting : Renderer3D::MakeGlobalLightingWrites(m_PreviewLightBuffer))
				writes.push_back(std::move(lighting));
			m_PreviewCameraSet->Update(writes);
		}
	}

	const Ref<Mesh>& MaterialEditorPanel::PreviewMeshFor(PreviewMesh kind)
	{
		const int index = std::clamp(static_cast<int>(kind), 0, 2);
		if (!m_PreviewMeshes[index])
		{
			switch (static_cast<PreviewMesh>(index))
			{
				case PreviewMesh::Cube:
					m_PreviewMeshes[index] = Mesh::CreateUnitCube(1.7f);
					break;
				case PreviewMesh::Plane:
					m_PreviewMeshes[index] = Mesh::CreateUnitPlane(1.8f);
					break;
				case PreviewMesh::Sphere:
				default:
					m_PreviewMeshes[index] = Mesh::CreateUnitSphere(2.0f, 48, 24);
					break;
			}
		}
		return m_PreviewMeshes[index];
	}

	void MaterialEditorPanel::BuildDerivedMeshes()
	{
		const int index = std::clamp(static_cast<int>(m_PreviewMesh), 0, 2);
		if (m_DerivedMeshFor == index && m_WireMesh && m_NormalMesh && m_CheckLightMesh && m_CheckDarkMesh)
			return;
		const Ref<Mesh>& source = PreviewMeshFor(m_PreviewMesh);
		if (!source)
			return;
		m_CheckLightMesh = nullptr;
		m_CheckDarkMesh = nullptr;
		m_WireMesh = nullptr;
		m_NormalMesh = nullptr;
		// 尺寸系数按包围半径算,球/立方/平面三种网格的观感一致。
		const float radius = std::max(0.2f, source->GetBounds().GetRadius());
		BuildCheckerMeshes(*source, &m_CheckLightMesh, &m_CheckDarkMesh);
		m_WireMesh = BuildWireMesh(*source, radius * 0.016f, radius * 0.0025f);
		// 法线柱:长度 25% 半径、截面半径 1.2% 半径(在 400px 预览上约 2-3px 宽,看得见)。
		m_NormalMesh = BuildNormalMesh(*source, radius * 0.25f, radius * 0.012f, radius * 0.004f);
		m_DerivedMeshFor = index;
	}

	void MaterialEditorPanel::EnsureOverrideMaterials()
	{
		// 预览专用纯色材质:不进资产库缓存(不落盘、不出现在内容浏览器),
		// 只被本面板的覆盖绘制(线框/法线/UV 棋盘格)引用。
		if (!m_OverrideLight)
		{
			m_OverrideLight = MaterialLibrary::Get().CreateDefault("Material Preview Cell");
			m_OverrideLight->SetBaseColor({ 0.87f, 0.87f, 0.87f, 1.0f });
			m_OverrideLight->SetRoughness(0.95f);
			// 双面:覆盖层的四边形是按 UV 参数域拼的,UV 镜像的三角形上绕序会反转,
			// 单面材质会把它们当背面剔掉(实测 UV 棋盘格出现大片黑洞)。
			m_OverrideLight->SetDoubleSided(true);
		}
		if (!m_OverrideDark)
		{
			m_OverrideDark = MaterialLibrary::Get().CreateDefault("Material Preview Cell Dark");
			m_OverrideDark->SetBaseColor({ 0.18f, 0.20f, 0.24f, 1.0f });
			m_OverrideDark->SetRoughness(0.95f);
			m_OverrideDark->SetDoubleSided(true);
		}
		if (!m_OverrideWire)
		{
			m_OverrideWire = MaterialLibrary::Get().CreateDefault("Material Preview Wire");
			m_OverrideWire->SetBaseColor({ 1.0f, 0.62f, 0.18f, 1.0f });
			m_OverrideWire->SetRoughness(1.0f);
			m_OverrideWire->SetDoubleSided(true);
		}
		if (!m_OverrideNormal)
		{
			m_OverrideNormal = MaterialLibrary::Get().CreateDefault("Material Preview Normals");
			m_OverrideNormal->SetBaseColor({ 0.25f, 0.82f, 1.0f, 1.0f });
			m_OverrideNormal->SetRoughness(1.0f);
			m_OverrideNormal->SetDoubleSided(true);
		}
	}

	// U21+P4-U13f 口径:目标尺寸 = 预览区物理像素(设计单位 × UiScale),长边 [128, 2048] 等比 clamp。
	// render_scale **不参与**:材质预览自建 framebuffer(不走 SceneRenderer::OnResize),
	// 读数里把它回显出来,供探针断言"改 render_scale 不影响预览目标"。
	void MaterialEditorPanel::UpdatePreviewTargetSize(const Wui::WuiRect& view)
	{
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const float pixelW = std::max(1.0f, view.W * uiScale);
		const float pixelH = std::max(1.0f, view.H * uiScale);
		const float longSide = std::max(pixelW, pixelH);
		float clampScale = 1.0f;
		if (longSide > kPreviewTargetMaxSide)
			clampScale = kPreviewTargetMaxSide / longSide;
		else if (longSide < kPreviewTargetMinSide)
			clampScale = kPreviewTargetMinSide / longSide;
		const uint32_t targetW = static_cast<uint32_t>(std::max(1.0f, std::round(pixelW * clampScale)));
		const uint32_t targetH = static_cast<uint32_t>(std::max(1.0f, std::round(pixelH * clampScale)));
		m_PreviewUiScale = uiScale;
		m_PreviewViewW = view.W;
		m_PreviewViewH = view.H;
		if (targetW == m_PreviewTargetW && targetH == m_PreviewTargetH)
			return;
		m_PreviewTargetW = targetW;
		m_PreviewTargetH = targetH;
		WLD_CORE_INFO("[material-ui] preview target {0}x{1} (view {2:.0f}x{3:.0f} design, uiScale={4:.2f})",
			m_PreviewTargetW, m_PreviewTargetH, view.W, view.H, uiScale);
	}

	uint64_t MaterialEditorPanel::RenderPreview()
	{
		if (!m_Material)
			return 0;
		EnsureGpuResources();
		// P4-UX16b:本帧槽位专属的命令缓冲(见头文件:单缓冲会在上一帧还没跑完时重录)。
		Rhi::Handle<Rhi::CommandBuffer>& command =
			m_PreviewCommands[Renderer::FrameSlot() % Renderer::FramesInFlight];
		const Ref<Mesh>& mesh = PreviewMeshFor(m_PreviewMesh);
		if (!m_PreviewFramebuffer || !m_PreviewCameraSet || !mesh || !command)
			return 0;

		EnsureOverrideMaterials();
		BuildDerivedMeshes();

		const float distance = m_CameraDistance;
		const glm::vec3 eye {
			distance * std::cos(m_OrbitPitch) * std::sin(m_OrbitYaw),
			distance * std::sin(m_OrbitPitch),
			distance * std::cos(m_OrbitPitch) * std::cos(m_OrbitYaw) };
		const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		// 宽高比 = 目标宽高比(非方形预览区不会被拉伸)。
		const float aspect = m_PreviewTargetH > 0
			? static_cast<float>(m_PreviewTargetW) / static_cast<float>(m_PreviewTargetH)
			: 1.0f;
		const glm::mat4 projection = glm::perspective(glm::radians(35.0f), aspect,
			std::max(0.01f, distance * 0.01f), distance * 8.0f + 10.0f);
		// 与场景同一条投影适配(离屏不做 Y 翻转,只补 Vulkan 的深度范围)。
		const glm::mat4 viewProjection = AdaptViewProjectionForOffscreen(projection * view,
			Renderer::GetBackendName() == "vulkan");
		m_PreviewCameraBuffer->SetData(&viewProjection, sizeof(glm::mat4));
		const LightUniforms lights = BuildPreviewLightUniforms(static_cast<int>(m_PreviewLighting),
			m_LightIntensity, m_LightAzimuth, m_LightElevation);
		if (m_PreviewLightBuffer)
			m_PreviewLightBuffer->SetData(&lights, sizeof(lights));

		std::vector<Rhi::ClearValue> clears(3);
		// 纯色背景 = 清屏色;渐变背景清成**透明**,由 WUI 在贴图下面画渐变(见 DrawPreview)。
		const bool gradientBackground = m_PreviewBackground == PreviewBackground::Gradient;
		if (gradientBackground)
			clears[0].Color = { 0.0f, 0.0f, 0.0f, 0.0f };
		else
			clears[0].Color = { 0.12f, 0.13f, 0.15f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		command->Begin();
		command->BeginRenderPass(m_PreviewPass, m_PreviewFramebuffer, clears);
		command->SetViewport({ 0, 0, static_cast<float>(m_PreviewTargetW),
			static_cast<float>(m_PreviewTargetH) });
		command->SetScissor({ 0, 0, m_PreviewTargetW, m_PreviewTargetH });
		command->BindDescriptorSet(m_PreviewCameraSet, 0);
		Renderer3D::BeginScene(viewProjection, command);
		// 预览用**固定槽位区**(按面板身份映射):否则每个面板/主场景都从序号 0 开始分配,
		// 会争用同一份对象 UBO 与材质描述符集,两个内容不同的材质面板就会逐帧互相覆盖
		// (用户实测:预览一直闪烁)。span=5:底材质 + 棋盘格两格 + 线框 + 法线。
		const uint32_t slotBase = Renderer3D::ReserveSlotBase(
			static_cast<uint32_t>(Wui::HashId(m_PanelId.c_str()) ^ 0x9E37u), 5);
		const glm::mat4 identity { 1.0f };
		// 覆盖层先画(它们贴在表面外侧,深度写让底材质不会盖掉细线)。
		if (m_ShowUvChecker && m_CheckLightMesh)
			Renderer3D::SubmitAtSlot(slotBase + 1, m_CheckLightMesh, m_OverrideLight, identity, -1);
		if (m_ShowUvChecker && m_CheckDarkMesh)
			Renderer3D::SubmitAtSlot(slotBase + 2, m_CheckDarkMesh, m_OverrideDark, identity, -1);
		if (m_ShowWireframe && m_WireMesh)
			Renderer3D::SubmitAtSlot(slotBase + 3, m_WireMesh, m_OverrideWire, identity, -1);
		if (m_ShowNormals && m_NormalMesh)
			Renderer3D::SubmitAtSlot(slotBase + 4, m_NormalMesh, m_OverrideNormal, identity, -1);
		// 底材质:UV 棋盘格打开时不画(棋盘格的意义是看 UV,不是看材质)。
		if (!m_ShowUvChecker)
			Renderer3D::SubmitAtSlot(slotBase, mesh, m_Material, identity, -1);
		// 诊断钩子(用户复现):WLD_MATERIAL_SWITCH_TEXTURE=<贴图路径> 在第 30 帧把指定面板的
		// Albedo 切到该贴图;WLD_MATERIAL_SWITCH_PANEL 指定面板(空 = 第一个面板)。
		if (const char* switchTo = std::getenv("WLD_MATERIAL_SWITCH_TEXTURE"))
		{
			const char* targetPanel = std::getenv("WLD_MATERIAL_SWITCH_PANEL");
			const bool matchesPanel = targetPanel == nullptr || *targetPanel == 0 || m_PanelId == targetPanel;
			if (matchesPanel)
			{
				static int switchCountdown = 30;
				if (switchCountdown > 0 && --switchCountdown == 0)
				{
					m_Material->SetAlbedoTexture(switchTo);
					WLD_CORE_INFO("[material-ui] diag switch albedo -> '{0}' panel='{1}' rev={2}",
						switchTo, m_PanelId, m_Material->GetRevision());
				}
			}
		}
		Renderer3D::EndScene();
		command->EndRenderPass();
		// 颜色附件已在 EndRenderPass 由渲染通道隐式转换为 FinalLayout(ShaderReadOnly),
		// 这里不再需要额外的 PipelineBarrier。
		command->End();
		const bool checkGlErrors = std::getenv("WLD_GL_ERRORS") != nullptr;
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-before-submit");
		Renderer::SubmitScene(command, m_PreviewColor);
		if (checkGlErrors)
			Renderer::DrainGLErrors("preview-after-submit");
		// AI 控制通道的一次性抓图请求(与 WLD_PREVIEW_TEX_CAPTURE 同一条读回路径)。
		if (!m_PendingPreviewCapture.empty())
		{
			if (Renderer::CaptureTexture(m_PendingPreviewCapture, m_PreviewColor,
				m_PreviewTargetW, m_PreviewTargetH))
				WLD_CORE_INFO("[ai] preview capture written: {0}", m_PendingPreviewCapture);
			m_PendingPreviewCapture.clear();
		}
		CapturePreviewTextureSequence();
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (m_PreviewTextureId == 0)
		{
			m_UiTextureGeneration = registry.Generation();
			m_PreviewTextureId = registry.Register(m_PreviewColor);
		}
		else if (registry.Generation() != m_UiTextureGeneration)
		{
			// 注册表整表清空(设备重建)→ 同一槽位换成新句柄。
			m_UiTextureGeneration = registry.Generation();
			registry.Update(m_PreviewTextureId, m_PreviewColor);
		}
		else
		{
			// 尺寸变化只是换了纹理对象:槽位 id 不变,Update 推进内容代让 WUI 后端丢掉旧 set。
			registry.Update(m_PreviewTextureId, m_PreviewColor);
		}
		return m_PreviewTextureId;
	}

	void MaterialEditorPanel::CapturePreviewTextureSequence()
	{
		// 无障碍诊断:把**预览纹理本身**连续写成 PPM(后端无关的 RHI 读回)。
		// 主窗口/独立窗口的整窗抓图在 Vulkan 下抓不到(glReadPixels 路径),而材质预览恰好
		// 只在 Vulkan 下有内容,所以"预览闪不闪"必须能直接读预览纹理:
		//   WLD_PREVIEW_TEX_CAPTURE=<目录>  + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		const char* dir = std::getenv("WLD_PREVIEW_TEX_CAPTURE");
		if (!dir || !*dir)
			return;
		static const int every = std::getenv("WLD_SCREEN_CAPTURE_EVERY")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_EVERY")) : 1;
		static const int start = std::getenv("WLD_SCREEN_CAPTURE_START")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_START")) : 0;
		static const int count = std::getenv("WLD_SCREEN_CAPTURE_COUNT")
			? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_COUNT")) : 60;
		const int step = every > 0 ? every : 1;
		++m_PreviewCaptureFrame;
		if (m_PreviewCaptureFrame < start || m_PreviewCaptureWritten >= count
			|| (m_PreviewCaptureFrame - start) % step != 0)
			return;
		std::string stem;
		for (char c : m_PanelId)
			stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
		const std::string path = std::string(dir) + "/preview-" + stem + "-"
			+ std::to_string(m_PreviewCaptureWritten) + ".ppm";
		Renderer::CaptureTexture(path, m_PreviewColor, m_PreviewTargetW, m_PreviewTargetH);
		++m_PreviewCaptureWritten;
	}

	void MaterialEditorPanel::SaveCurrent()
	{
		if (!m_Material)
			return;
		// 新建材质走内容浏览器(Create → New Material);这里只保存已落盘的材质。
		std::string path = m_Path.empty() ? m_NewPathBuffer : m_Path;
		// U2d:未落盘材质的"另存为"路径先过行内校验(空 / 非法字符 / 目标已存在)→ 拒绝保存。
		if (m_Path.empty() && !path.empty())
		{
			const std::string pathError = NewMaterialPathError(path);
			if (!pathError.empty())
			{
				m_Status = Wui::Tr("panel.material.status.save_failed", "Save failed: ") + pathError;
				m_StatusIsError = true;
				return;
			}
		}
		if (path.empty())
		{
			m_NewPathAttempted = true;   // 让行内也标出"路径不能为空"
			m_Status = Wui::Tr("panel.material.status.no_path",
				"Save failed: no path (create new materials as .wmat in the Content Browser)");
			m_StatusIsError = true;
			return;
		}
		std::string error;
		if (!MaterialLibrary::Get().Save(m_Material, path, &error))
		{
			m_Status = Wui::Tr("panel.material.status.save_failed", "Save failed: ") + error;
			m_StatusIsError = true;
			return;
		}
		m_Path = m_Material->GetPath();
		SetMaterialPathForPanel(m_Path);
		RefreshPickerIndices();
		// 保存 = 磁盘与内存一致:校验缓存要重算(缺贴图可能刚补上)。
		m_ValidationRevision = 0;
		m_Status = Wui::Tr("panel.material.status.saved", "Saved ") + m_Path;
		m_StatusIsError = false;
	}

	void MaterialEditorPanel::FramePreview()
	{
		const Ref<Mesh>& mesh = PreviewMeshFor(m_PreviewMesh);
		const float radius = mesh ? std::max(0.2f, mesh->GetBounds().GetRadius()) : 1.0f;
		m_FocusDistance = radius * 3.0f;
		m_CameraMinDistance = radius * 0.6f;
		m_CameraMaxDistance = radius * 30.0f;
		m_CameraDistance = std::clamp(m_FocusDistance, m_CameraMinDistance, m_CameraMaxDistance);
	}

	void MaterialEditorPanel::RevealMaterialOnDisk()
	{
		if (m_Path.empty())
		{
			m_Status = Wui::Tr("panel.material.status.reveal_failed",
				"Reveal failed: this material has never been saved to disk");
			m_StatusIsError = true;
			return;
		}
		const std::filesystem::path diskPath = ContentRootPath() / m_Path;
		std::error_code ec;
		const bool exists = std::filesystem::exists(diskPath, ec);
		if (!exists)
		{
			m_Status = Wui::Tr("panel.material.status.reveal_missing",
				"Reveal failed: the .wmat is not on disk yet (save it first)");
			m_StatusIsError = true;
			return;
		}
#ifdef _WIN32
		// 与内容浏览器"Show in Explorer"同一条系统调用(/select 打开所在文件夹并选中该项)。
		const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(diskPath).wstring() + L"\"";
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(),
			nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) <= 32)
		{
			m_Status = Wui::Tr("panel.material.status.reveal_failed", "Reveal failed");
			m_StatusIsError = true;
			return;
		}
		m_Status = Wui::Tr("panel.material.status.revealed", "Revealed in Explorer: ") + m_Path;
		m_StatusIsError = false;
#else
		m_Status = Wui::Tr("panel.material.status.reveal_unsupported",
			"Reveal is only implemented on Windows");
		m_StatusIsError = true;
#endif
	}

	// ---- 头部:材质名 + 来源逻辑路径 + 脏标记 + Save / Reveal / Revert ----
	float MaterialEditorPanel::DrawHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		// U23(用户 2026-09-22「材质编辑器上面有空行,太丑了」):头部高度**按内容算**,
		// 行间距全部取主题令牌(不再出现 kHeaderBaseHeight 这种固定占位造成的空白带)。
		const float gap = theme.PadSmall;
		const float x = rect.X;
		const float y = rect.Y + gap;
		const float buttonH = theme.ControlHeight;
		const float actionGap = 6.0f;   // 动作按钮之间的间距(控件间距,与容器留白无关)
		const bool dirty = m_Material && m_Material->IsDirty();
		const bool readOnly = host.IsReadOnlyMode();

		// ---- U25-M2:头部动作表(唯一落点)----
		// Save / Save As… / Assign to Selection / Undo Assign / Reveal / Revert。
		// 按钮宽度按文案量出来(**不硬编码 76**):中文与英文都放得下;一行放不下就换行,
		// 窄窗因此不会把动作挤到面板之外(它们仍有稳定 a11y id,ui.invoke 永远点得到)。
		struct HeaderAction
		{
			const char* Id = "";
			std::string Label;
			std::string Doc;
			bool Enabled = true;
			bool Primary = false;
		};
		const std::string assignBlockedDoc = Wui::Tr("panel.material.assign.blocked",
			"Assign to Selection: unavailable while Play/Simulate runs (the scene is read-only).");
		const std::string assignDoc = readOnly ? assignBlockedDoc
			: Wui::Tr("panel.material.assign.tooltip",
				"Assign to Selection: write this material into the MeshRenderer of the entity selected in "
				"the Scene Hierarchy (the same field the Properties panel edits).");
		const std::string undoDoc = !m_AssignUndoValid
			? Wui::Tr("panel.material.assign.undo.none",
				"Undo Assign: nothing to undo — this panel has not assigned a material yet.")
			: (readOnly ? assignBlockedDoc
				: Wui::Tr("panel.material.assign.undo.tooltip",
					"Undo Assign: write the previous material path back into the entity this panel "
					"assigned just now (one level: only that write)."));
		const std::vector<HeaderAction> headerActions {
			{ "material.save", Wui::Tr("panel.material.save", "Save"),
				Wui::Tr("panel.material.save.tooltip",
					"Save: write the current values back to the .wmat on disk."), true, true },
			{ "material.saveas", Wui::Tr("panel.material.saveas", "Save As…"),
				Wui::Tr("panel.material.saveas.tooltip",
					"Save As…: write a variant (default name <name>_variant) in a folder under the content "
					"root, then keep editing the variant. The source file is never overwritten."), true, false },
			{ "material.assign", Wui::Tr("panel.material.assign", "Assign to Selection"), assignDoc,
				!readOnly, false },
			{ "material.assign.undo", Wui::Tr("panel.material.assign.undo", "Undo Assign"), undoDoc,
				m_AssignUndoValid && !readOnly, false },
			{ "material.extract", Wui::Tr("panel.material.extract", "Extract from Selection"),
				(!readOnly && host.GetSelectedEntity().IsValid())
					? Wui::Tr("panel.material.extract.tooltip",
						"Extract from Selection: start a new material from the selected entity's current "
						"render setup (its material, or its colour), then assign the result back to it.")
					: (readOnly
						? Wui::Tr("panel.material.extract.blocked_play",
							"Extract from Selection: unavailable while Play/Simulate runs (the scene is read-only).")
						: Wui::Tr("panel.material.extract.blocked_none",
							"Extract from Selection: select an entity in the Scene Hierarchy first.")),
				!readOnly && host.GetSelectedEntity().IsValid(), false },
			{ "material.reveal", Wui::Tr("panel.material.reveal", "Reveal"),
				Wui::Tr("panel.material.reveal.tooltip",
					"Reveal: select the .wmat file in Windows Explorer (needs a saved file)."), true, false },
			{ "material.revert", Wui::Tr("panel.material.revert", "Revert"),
				Wui::Tr("panel.material.revert.tooltip",
					"Revert: drop unsaved edits and read the .wmat from disk again."), true, false },
		};
		float actionX = x;
		float actionY = y;
		float actionsHeight = buttonH;
		for (const HeaderAction& action : headerActions)
		{
			const float measured = ctx.MeasureTextWidth(action.Label, 13.0f) + 18.0f;
			const float width = std::min(std::max(kActionMinWidth, measured), std::max(24.0f, rect.W - 2.0f));
			if (actionX > x && actionX + width > x + rect.W - 2.0f)
			{
				actionX = x;
				actionY += buttonH + actionGap;
			}
			const Wui::WuiRect actionRect { actionX, actionY, width, buttonH };
			const std::string actionId = action.Id;
			if (ActionButton(ctx, Wui::HashId(action.Id), actionRect, action.Label, action.Doc,
				action.Enabled, action.Primary, theme))
			{
				if (actionId == "material.save")
					SaveCurrent();
				else if (actionId == "material.saveas")
					OpenSaveAsModal(ctx, host);
				else if (actionId == "material.assign")
					AssignToSelection(host);
				else if (actionId == "material.assign.undo")
					UndoAssign(host);
				else if (actionId == "material.extract")
				{
					// 向导住在内容浏览器面板(模板/名称/目录/落点控件都在那边):
					// 面板只负责把"从选中对象提取"的意图传过去,并回显结果。
					std::string message;
					if (host.OpenNewMaterialWizard(true, &message))
					{
						m_Status = Wui::Tr("panel.material.status.extract_opened",
							"Extract from Selection: the new-material wizard is open in the Content Browser "
							"(it will assign the result back to the selected entity)");
						m_StatusIsError = false;
					}
					else
					{
						m_Status = Wui::Tr("panel.material.status.extract_failed",
							"Extract from Selection failed: ")
							+ (message.empty() ? std::string("no selection") : message);
						m_StatusIsError = true;
					}
				}
				else if (actionId == "material.reveal")
					RevealMaterialOnDisk();
				else if (actionId == "material.revert")
				{
					if (m_Path.empty())
					{
						m_Status = Wui::Tr("panel.material.status.revert_failed",
							"Revert failed: this material has never been saved to disk");
						m_StatusIsError = true;
					}
					else
					{
						std::string error;
						if (MaterialLibrary::Get().Reload(m_Path, &error))
						{
							RefreshPickerIndices();
							m_ValidationRevision = 0;
							m_SyncNameBuffer = true;
							m_Status = Wui::Tr("panel.material.status.reloaded", "Reloaded from disk ") + m_Path;
							m_StatusIsError = false;
						}
						else
						{
							m_Status = Wui::Tr("panel.material.status.reload_failed", "Reload failed: ") + error;
							m_StatusIsError = true;
						}
					}
				}
			}
			actionsHeight = (actionY - y) + buttonH;
			actionX += actionRect.W + actionGap;
		}

		// 脏标记(状态节点,不是按钮):* = 内存与磁盘不一致。
		const std::string dirtyText = dirty ? Wui::Tr("panel.material.dirty", "* Unsaved changes")
			: Wui::Tr("panel.material.clean", "Saved");
		const float dirtyWidth = ctx.MeasureTextWidth(dirtyText, 11.0f);
		const Wui::WuiRect dirtyRect { x + rect.W - dirtyWidth - 6.0f, y + 5.0f, dirtyWidth, 16.0f };
		Wui::Label(ctx, { dirtyRect.X, dirtyRect.Y }, dirtyText,
			dirty ? theme.Warning : theme.TextDisabled, 11.0f);
		const std::string dirtyDoc = Wui::Tr("panel.material.dirty.tooltip",
			"Unsaved changes are kept in memory until you press Save; the file on disk is unchanged.");
		Wui::Tooltip(ctx, dirtyRect, dirtyDoc);
		RegisterReadOnlyNode(Wui::HashId("material.dirty"),
			Wui::Tr("panel.material.dirty.label", "Unsaved changes"), dirty ? "true" : "false",
			dirtyRect, dirtyDoc);

		// 第二行:材质名 + 来源逻辑路径(紧贴动作行下方一个 PadSmall)。
		const float lineY = y + actionsHeight + gap;
		const MaterialDesc& desc = m_Material->GetDesc();
		std::filesystem::path file(m_Path.empty() ? std::string() : m_Path);
		std::string fallbackName = file.stem().string();
		if (fallbackName.empty())
			fallbackName = "Material";
		const std::string name = desc.Name.empty() ? fallbackName : desc.Name;
		const float nameWidth = std::max(40.0f,
			std::min(rect.W * 0.5f, ctx.MeasureTextWidth(name, 13.0f)));
		Wui::Label(ctx, { x, lineY }, EllipsizeToWidth(ctx, name, nameWidth, 13.0f), theme.Text, 13.0f);
		const std::string nameDoc = Wui::Tr("panel.material.name.tooltip",
			"Material display name (the .wmat Name field). Asset identity is the file path; rename the file "
			"in the Content Browser to change where it lives.");
		Wui::Tooltip(ctx, { x, lineY, nameWidth, kHeaderTextHeight }, nameDoc);
		RegisterReadOnlyNode(Wui::HashId("material.name"),
			Wui::Tr("panel.material.name", "Material name"), name,
			{ x, lineY, nameWidth, kHeaderTextHeight }, nameDoc);
		const float pathX = x + nameWidth + 10.0f;
		if (pathX < x + rect.W - 60.0f)
		{
			const float pathWidth = rect.W - (pathX - x) - 6.0f;
			const std::string pathText = m_Path.empty()
				? Wui::Tr("panel.material.unsaved_new", "(unsaved new material)")
				: ShortenPath(m_Path);
			Wui::Label(ctx, { pathX, lineY + 2.0f },
				EllipsizeToWidth(ctx, pathText, pathWidth, 11.0f), theme.TextMuted, 11.0f);
			const std::string pathDoc = Wui::Tr("panel.material.path.tooltip",
				"Source asset path, relative to the content root (Game/assets).");
			Wui::Tooltip(ctx, { pathX, lineY, pathWidth, kHeaderTextHeight }, pathDoc);
			RegisterReadOnlyNode(Wui::HashId("material.path"),
				Wui::Tr("panel.material.path", "Source path"), pathText,
				{ pathX, lineY, pathWidth, kHeaderTextHeight }, pathDoc);
		}

		// U25-M2 B:拖 .wmat 到**标题/头部件** = 在本窗口打开该材质(有未保存改动先确认)。
		// 落点 = 名称/路径这一行(用户眼里的"标题");贴图等其它类型到这里给可读反馈,不静默改。
		const Wui::WuiRect titleRect { x, lineY, std::max(60.0f, rect.W - 2.0f), kHeaderTextHeight };
		RegisterHeaderDrop(ctx, host, titleRect);
		TakeHeaderDrop(ctx, host);

		// 头部真实高度 = 最后一行文字的底边(名字/路径行),不留空白带。
		float used = (lineY - rect.Y) + kHeaderTextHeight;
		if (m_Path.empty())
		{
			// 未落盘材质:给出"另存为"路径输入 + 行内校验。
			const Wui::WuiRect field { x, rect.Y + used + gap, rect.W - 8.0f, 22.0f };
			const std::string pathError = NewMaterialPathError(m_NewPathBuffer);
			const std::string shownError = m_NewPathBuffer.empty()
				? (m_NewPathAttempted ? pathError : std::string()) : pathError;
			Wui::TextFieldEx(ctx, Wui::HashId("material.newpath"), field, m_NewPathBuffer, theme, shownError);
			used += gap + 22.0f;
		}
		if (!m_Status.empty())
		{
			const Wui::WuiRect statusRect { x, rect.Y + used + gap,
				std::max(20.0f, rect.W - 8.0f), kHeaderStatusHeight };
			Wui::Label(ctx, { x, statusRect.Y }, EllipsizeToWidth(ctx, m_Status, rect.W - 8.0f, 11.0f),
				m_StatusIsError ? theme.Danger : theme.TextMuted, 11.0f);
			// U23:状态行也登记无障碍节点 —— 它是头部的一部分,"标题底"要按它算
			// (顶部间距的验收口径:标题/路径/动作条整块 → 首个内容控件)。
			RegisterReadOnlyNode(Wui::HashId("material.status"),
				Wui::Tr("panel.material.status", "Status"), m_Status, statusRect,
				Wui::Tr("panel.material.status.tooltip",
					"Result of the most recent material action (open / save / reload / reveal)."));
			used += gap + kHeaderStatusHeight;
		}
		return used;
	}

	// ---- 一行参数:标签 + 控件 + 恢复默认 + 悬停说明 + 无障碍 ----
	void MaterialEditorPanel::DrawParameterRow(Wui::WuiContext& ctx, PanelHost& host,
		const Wui::WuiTheme& theme, const RowPlan& row, float x, float y, float width, bool stacked)
	{
		const MaterialDesc& desc = m_Material->GetDesc();
		const Wui::WuiId controlId = Wui::HashId(row.ControlId.c_str());
		const float labelWidth = stacked ? width - 8.0f
			: std::min(kLabelColumnMax, std::max(84.0f, width * 0.36f));
		const float labelY = stacked ? y + 1.0f : y + 4.0f;
		const float controlY = stacked ? y + 20.0f : y;
		const float controlHeight = stacked ? 20.0f : 22.0f;
		// U24:恢复默认 = **固定占位**(偏离默认与等于默认都占同一个槽位);控件区宽度与行高
		// 在两种状态下逐像素相同 —— 旧实现"只在偏离时出现"会把控件挤窄(用户反馈②)。
		const RowControl control = RowControlFor(row.Key);
		const bool hasResetSlot = row.HasReset && !row.ReadOnly && control != RowControl::Button;
		const float controlX = stacked ? x : x + labelWidth + 8.0f;
		const float reserved = hasResetSlot ? kResetWidth + 6.0f : 0.0f;
		const float controlWidth = std::max(60.0f, width - (controlX - x) - reserved);
		const Wui::WuiRect controlRect { controlX, controlY, controlWidth, controlHeight };
		const Wui::WuiRect rowRect { x, y, width, row.Height };
		// 数值行的悬停说明补一句"条体拖动 / 值区输入 / ↑↓ 步进"(分类规则落在 DragBar 上的行)。
		const std::string rowDoc = control == RowControl::DragBar
			? row.Doc + "\n" + Wui::Tr("panel.material.numeric.tooltip",
				"Drag the bar to change the value, or click the value box to type it "
				"(Enter commits, Esc cancels); Up/Down step by 1% of the range.")
			: row.Doc;
		Wui::Tooltip(ctx, rowRect, rowDoc);

		const auto applyEdit = [this](auto&& setter)
		{
			const uint32_t revision = m_Material->GetRevision();
			setter();
			if (m_Material->GetRevision() != revision)
				m_Material->MarkDirty(true);
		};

		if (row.ReadOnly)
		{
			// 只读信息行:标签 + 值(不画控件、不给复位)。
			Wui::Label(ctx, { x, labelY }, EllipsizeToWidth(ctx, row.Label, labelWidth, 12.0f),
				theme.TextMuted, 12.0f);
			Wui::Label(ctx, { controlX, controlY + 3.0f },
				EllipsizeToWidth(ctx, row.Value, controlWidth, 12.0f), theme.Text, 12.0f);
			RegisterReadOnlyNode(controlId, row.Label, row.Value,
				{ controlX, controlY, controlWidth, controlHeight }, row.Doc);
			return;
		}
		Wui::Label(ctx, { x, labelY }, EllipsizeToWidth(ctx, row.Label, labelWidth, 12.0f),
			theme.Text, 12.0f);

		// ---- 动作行:恢复整个材质的默认值 ----
		if (row.Key == std::string("reset_all"))
		{
			const Wui::WuiRect buttonRect { controlX, controlY, std::min(controlWidth, 220.0f), controlHeight };
			if (Wui::Button(ctx, controlId, buttonRect, row.Label, theme))
				ResetAllMaterialFields();
			Wui::Tooltip(ctx, buttonRect, row.Doc);
			AnnotateNode(ctx, controlId, row.Label, row.Doc);
			return;
		}

		if (row.Key == std::string("base"))
		{
			glm::vec4 color = desc.BaseColor;   // 量化在下面写回时做(见 QuantizeMaterialValue)
			if (Wui::ColorField(ctx, controlId, controlRect, color, theme))
				applyEdit([&] { m_Material->SetBaseColor(QuantizeMaterialValue(color)); });
		}
		else if (row.Key == std::string("metallic"))
		{
			// 感知型 0..1 比例(分类规则 → DragBarFloat):值区常显当前值、点值区可输入。
			float value = desc.Metallic;
			Wui::DragBarFloat(ctx, controlId, controlRect, value, 0.0f, 1.0f, theme);
			if (value != desc.Metallic)
				applyEdit([&] { m_Material->SetMetallic(QuantizeMaterialValue(value)); });
		}
		else if (row.Key == std::string("roughness"))
		{
			// 感知型 0..1 比例(下限 0.02 = 镜面高光仍可见)。
			float value = desc.Roughness;
			Wui::DragBarFloat(ctx, controlId, controlRect, value, 0.02f, 1.0f, theme);
			if (value != desc.Roughness)
				applyEdit([&] { m_Material->SetRoughness(QuantizeMaterialValue(value)); });
		}
		else if (row.Key == std::string("emissive"))
		{
			glm::vec3 value = desc.Emissive;
			if (Wui::Vec3Field(ctx, controlId, controlRect, value, 0.02f, 0.0f, 8.0f, theme, 0))
				applyEdit([&] { m_Material->SetEmissive(QuantizeMaterialValue(value)); });
		}
		else if (row.Key == std::string("blend"))
		{
			std::vector<std::string> options { Wui::Tr("material.blend.opaque", "Opaque"),
				Wui::Tr("material.blend.transparent", "Transparent") };
			int selected = desc.BlendMode == MaterialBlendMode::Transparent ? 1 : 0;
			if (Wui::Combo(ctx, controlId, controlRect, row.Label, options, selected, theme))
				applyEdit([&]
				{
					m_Material->SetBlendMode(selected == 1 ? MaterialBlendMode::Transparent
						: MaterialBlendMode::Opaque);
				});
		}
		else if (row.Key == std::string("doublesided"))
		{
			bool value = desc.DoubleSided;
			if (Wui::Checkbox(ctx, controlId, controlRect, row.Label, value, theme))
				applyEdit([&] { m_Material->SetDoubleSided(value); });
		}
		else if (row.Key == std::string("albedo"))
		{
			// U25-M2 B:贴图槽 = 跨窗口资产拖放的落点(内容浏览器 → 本面板)。
			RegisterSlotDrop(ctx, host, row.Key, controlRect);
			std::vector<std::string> options = m_TexturePaths;
			options.insert(options.begin(), Wui::Tr("panel.material.texture_none", "(none)"));
			if (Wui::SearchableCombo(ctx, controlId, controlRect, row.Label, options,
				m_AlbedoPickIndex, theme))
			{
				// 只在**真的换了一张**时才写回材质:否则每帧调用会让 Revision 每帧 +1,
				// 渲染侧每帧重建材质描述符集 → 预览逐帧闪。
				const std::string chosen = m_AlbedoPickIndex <= 0 ? std::string()
					: options[static_cast<size_t>(m_AlbedoPickIndex)];
				if (chosen != desc.AlbedoTexture)
					applyEdit([&] { m_Material->SetAlbedoTexture(chosen); });
			}
			TakeSlotDrop(host, row.Key);
		}
		else if (row.Key == std::string("normal"))
		{
			RegisterSlotDrop(ctx, host, row.Key, controlRect);
			std::vector<std::string> options = m_TexturePaths;
			options.insert(options.begin(), Wui::Tr("panel.material.texture_none", "(none)"));
			if (Wui::SearchableCombo(ctx, controlId, controlRect, row.Label, options,
				m_NormalPickIndex, theme))
			{
				const std::string chosen = m_NormalPickIndex <= 0 ? std::string()
					: options[static_cast<size_t>(m_NormalPickIndex)];
				if (chosen != desc.NormalTexture)
					applyEdit([&] { m_Material->SetNormalTexture(chosen); });
			}
			TakeSlotDrop(host, row.Key);
		}
		else if (row.Key == std::string("name"))
		{
			if (m_SyncNameBuffer)
			{
				m_NameBuffer = desc.Name;
				m_SyncNameBuffer = false;
			}
			Wui::TextFieldA11y a11y;
			a11y.Label = row.Label;
			a11y.Placeholder = row.Label;
			if (Wui::TextField(ctx, controlId, controlRect, m_NameBuffer, theme, nullptr, &a11y))
			{
				MaterialDesc updated = desc;
				updated.Name = m_NameBuffer;
				m_Material->SetDesc(updated);
				m_Material->MarkDirty(true);
			}
		}
		else if (row.Key == std::string("preview.mesh"))
		{
			std::vector<std::string> options { Wui::Tr("material.mesh.sphere", "Sphere"),
				Wui::Tr("material.mesh.cube", "Cube"), Wui::Tr("material.mesh.plane", "Plane") };
			int selected = static_cast<int>(m_PreviewMesh);
			if (Wui::Segmented(ctx, controlId, controlRect, options, selected, theme))
			{
				m_PreviewMesh = static_cast<PreviewMesh>(selected);
				FramePreview();
			}
		}
		else if (row.Key == std::string("preview.bg"))
		{
			std::vector<std::string> options { Wui::Tr("material.bg.solid", "Solid"),
				Wui::Tr("material.bg.gradient", "Gradient") };
			int selected = static_cast<int>(m_PreviewBackground);
			if (Wui::Segmented(ctx, controlId, controlRect, options, selected, theme))
				m_PreviewBackground = static_cast<PreviewBackground>(selected);
		}
		else if (row.Key == std::string("preview.light"))
		{
			std::vector<std::string> options { Wui::Tr("material.light.three_point", "Three-Point"),
				Wui::Tr("material.light.single", "Single"), Wui::Tr("material.light.none", "None") };
			int selected = static_cast<int>(m_PreviewLighting);
			if (Wui::Segmented(ctx, controlId, controlRect, options, selected, theme))
				m_PreviewLighting = static_cast<PreviewLighting>(selected);
		}
		else if (row.Key == std::string("preview.light.intensity"))
		{
			// 感知型强度(分类规则 → DragBarFloat);两位小数足够读出主光倍率。
			Wui::WuiNumberStyle style;
			style.Decimals = 2;
			Wui::DragBarFloat(ctx, controlId, controlRect, m_LightIntensity, 0.0f, 4.0f, theme, style);
		}
		else if (row.Key == std::string("preview.light.azimuth"))
		{
			// 角度(分类规则 → DragBarFloat):带 ° 单位、一位小数 —— 单位与值区右对齐由控件负责。
			Wui::WuiNumberStyle style;
			style.Decimals = 1;
			style.Unit = "°";
			style.ValueWidth = 68.0f;   // "-180.0°" 也能完整显示(值区宽度是**每个字段的常量**)
			Wui::DragBarFloat(ctx, controlId, controlRect, m_LightAzimuth, -180.0f, 180.0f, theme, style);
		}
		else if (row.Key == std::string("preview.light.elevation"))
		{
			Wui::WuiNumberStyle style;
			style.Decimals = 1;
			style.Unit = "°";
			style.ValueWidth = 68.0f;
			Wui::DragBarFloat(ctx, controlId, controlRect, m_LightElevation, -85.0f, 85.0f, theme, style);
		}
		else if (row.Key == std::string("preview.wireframe"))
		{
			Wui::Checkbox(ctx, controlId, controlRect, row.Label, m_ShowWireframe, theme);
		}
		else if (row.Key == std::string("preview.normals"))
		{
			Wui::Checkbox(ctx, controlId, controlRect, row.Label, m_ShowNormals, theme);
		}
		else if (row.Key == std::string("preview.uvchecker"))
		{
			Wui::Checkbox(ctx, controlId, controlRect, row.Label, m_ShowUvChecker, theme);
		}

		// ---- 恢复默认(固定占位:两个状态同一个 rect,行布局零位移)----
		if (hasResetSlot)
		{
			const Wui::WuiRect resetRect { x + width - kResetWidth, controlY, kResetWidth, controlHeight };
			const std::string resetId = ResetIdFor(row.Key);
			const std::string resetDoc = Wui::Tr("panel.material.reset.tooltip",
				"Restore this parameter to its default value.") + " "
				+ Wui::Tr("panel.material.reset.placeholder",
					"Always occupies this slot: it stays dimmed while the value already equals the default.");
			// 控件自己登记 a11y(kind="reset-default"、value="modified"/"default"、enabled 跟随),
			// 两个状态只改高亮不改几何 —— 探测口径:前后行矩形逐像素相同。
			const Wui::WuiId resetWuiId = Wui::HashId(resetId.c_str());
			if (Wui::ResetDefaultButton(ctx, resetWuiId, resetRect, row.Modified,
				theme, Wui::Tr("panel.material.reset_row", "Reset"), resetDoc))
				SetFieldToDefault(row.Key);
			// 控件的 a11y 节点自带 kind/value/enabled,但**不带**悬停说明:两态都补齐
			// (AnnotateNode 保留 live 的 value/kind,只补 label/tooltip)。
			AnnotateNode(ctx, resetWuiId, Wui::Tr("panel.material.reset_row", "Reset"), resetDoc);
		}
		// 控件自己登记过节点:补上参数名与悬停说明(值/矩形仍以控件为准)。
		AnnotateNode(ctx, controlId, row.Label, rowDoc);
	}

	// ---- 参数区:搜索 + 分组折叠 + 每字段控件 + 校验区 ----
	float MaterialEditorPanel::DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		if (!m_Material)
		{
			Wui::Label(ctx, { rect.X + 8.0f, rect.Y + 8.0f },
				Wui::Tr("panel.material.none_open",
					"No material open: double-click a .wmat file in the Content Browser"),
				theme.TextMuted, 13.0f);
			return rect.H;
		}
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (m_ValidationPath != m_Path || m_ValidationRevision != m_Material->GetRevision()
			|| now - m_ValidationTime > 1.0)
			RefreshValidation(now);

		// ---- 搜索框(与设置面板同一控件与密度)----
		Wui::TextFieldA11y searchA11y;
		searchA11y.Label = Wui::Tr("panel.material.search", "Search parameters");
		searchA11y.Placeholder = Wui::Tr("panel.material.search.hint", "Search parameters…");
		// U23:参数区第一行就是搜索框(顶到分配区域的上沿),它下面只留一个 PadSmall —— 参数区
		// 顶部的空白带一并去掉(用户 2026-09-22「上面有空行」)。
		const float searchHeight = theme.ControlHeight;
		const Wui::WuiRect searchRect { rect.X, rect.Y, rect.W, searchHeight };
		if (Wui::TextField(ctx, Wui::HashId("material.search"), searchRect, m_Search, theme,
			nullptr, &searchA11y))
			m_ScrollY = 0.0f;
		if (m_Search.empty())
			Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f }, searchA11y.Placeholder,
				theme.TextDisabled, 12.0f);
		const std::string searchDoc = Wui::Tr("panel.material.search.tooltip",
			"Filter parameters by name, group or description. Clear the field to show everything again.");
		Wui::Tooltip(ctx, searchRect, searchDoc);
		AnnotateNode(ctx, Wui::HashId("material.search"), searchA11y.Label, searchDoc);

		// ---- 底部两块(常驻顺序:参数内容 → 校验区 → 引用者条)----
		// 引用者条常驻一行("被 N 处引用"/"无引用"):它放在参数列的**底部**而不是头部 ——
		// 头部与首个内容控件之间的间距口径(U23:标题底 → 首个内容控件 = PadSmall)因此不变。
		if (m_RefsPath != m_Path || now - m_RefsTime > kRefsTtlSeconds)
			RefreshReferences(now, false);
		const float refsHeight = kRefsRowHeight
			+ (m_RefsOpen ? static_cast<float>(std::min(m_Refs.size(), kRefsMaxShown)) * kRefsItemHeight + 2.0f : 0.0f);
		// 校验区高度(只在有问题时占位)
		float validationHeight = 0.0f;
		const size_t shownIssues = std::min<size_t>(m_Validation.size(), 3);
		if (!m_Validation.empty())
			validationHeight = 20.0f + static_cast<float>(shownIssues) * 18.0f;
		const float contentTop = searchHeight + theme.PadSmall;
		const Wui::WuiRect contentRect { rect.X, rect.Y + contentTop, rect.W,
			std::max(40.0f, rect.H - contentTop - validationHeight - refsHeight) };

		// ---- 行集合(搜索过滤;行高按窄列与否)----
		const bool stacked = rect.W < kStackedThreshold;
		const std::string needle = ToLowerAscii(m_Search);
		std::vector<RowPlan> plans;
		for (int index = 0; index < kRowSpecCount; ++index)
		{
			const RowSpec& spec = kRowSpecs[index];
			// U23:预览设置不在这张列表里 —— 它们在预览区自己的标签条里(DrawPreview)。
			if (std::string(spec.Group) == std::string("preview"))
				continue;
			RowPlan plan;
			plan.Group = spec.Group;
			plan.Key = spec.Key;
			plan.ControlId = std::string("material.") + spec.Key;
			// 两个"不与字段同名"的 id:头部已占用 material.name(材质名回显),
			// 动作行 material.prop.reset_all 与 material.prop.<key>.reset 同一命名族。
			if (plan.Key == std::string("name"))
				plan.ControlId = "material.prop.name";
			else if (plan.Key == std::string("reset_all"))
				plan.ControlId = "material.prop.reset_all";
			plan.Label = Wui::Tr(spec.LabelKey, spec.LabelEn);
			plan.Doc = Wui::Tr(spec.DocKey, spec.DocEn);
			plan.ReadOnly = spec.ReadOnly != 0;
			plan.HasReset = spec.HasReset != 0;
			plan.Modified = plan.HasReset && !plan.ReadOnly && FieldModified(plan.Key);
			// 只读行的显示值(采样/高级诊断)。
			if (plan.Key == std::string("normal.space"))
				plan.Value = Wui::Tr("material.value.linear", "Linear (UNORM, no sRGB decode)");
			else if (plan.Key == std::string("albedo.space"))
				plan.Value = Wui::Tr("material.value.srgb", "sRGB (hardware decode)");
			else if (plan.Key == std::string("sampler"))
			{
				char samplerText[128] = {};
				std::snprintf(samplerText, sizeof(samplerText), "%s, anisotropy %.0f",
					Wui::Tr("material.value.sampler", "Linear filter, repeat wrap").c_str(),
					static_cast<double>(RenderSettings::Get().Anisotropy));
				plan.Value = samplerText;
			}
			else if (plan.Key == std::string("format"))
				plan.Value = "FormatVersion " + std::to_string(MaterialIO::kFormatVersion);
			else if (plan.Key == std::string("revision"))
				plan.Value = "Revision " + std::to_string(m_Material->GetRevision());
			else if (plan.Key == std::string("disk"))
				plan.Value = m_Material->IsDirty()
					? Wui::Tr("material.value.dirty", "modified (unsaved edits in memory)")
					: Wui::Tr("material.value.clean", "clean (matches the file on disk)");
			if (!needle.empty())
			{
				const GroupDesc* group = FindGroup(spec.Group);
				const std::string haystack = ToLowerAscii(std::string(spec.Key) + " " + spec.LabelEn + " "
					+ spec.DocEn + " " + plan.Label + " " + plan.Doc + " " + plan.Value + " "
					+ (group ? group->Key : "") + " " + (group ? GroupLabel(*group) : std::string()));
				if (haystack.find(needle) == std::string::npos)
					continue;
			}
			plan.Height = stacked ? (plan.ReadOnly ? 34.0f : kRowHeightStacked) : (plan.ReadOnly ? 20.0f : kRowHeight);
			plans.push_back(std::move(plan));
		}

		// ---- 内容高度(折叠的组只占组头;展开的组 = 容器块,含子项)----
		float contentHeight = 6.0f;
		for (const GroupDesc& group : kGroups)
		{
			bool any = false;
			for (const RowPlan& plan : plans)
				if (plan.Group == group.Key)
					any = true;
			if (!any)
				continue;
			contentHeight += kGroupHeaderHeight + kGroupGap;
			if (!m_SectionOpen[group.Index] && needle.empty())
				continue;
			for (const RowPlan& plan : plans)
				if (plan.Group == group.Key)
					contentHeight += plan.Height;
		}
		const float maxScroll = std::max(0.0f, contentHeight - contentRect.H);
		m_ScrollY = std::clamp(m_ScrollY, 0.0f, maxScroll);

		Wui::BeginScrollArea(ctx, contentRect, contentHeight, m_ScrollY, theme);
		float y = contentRect.Y + 4.0f - m_ScrollY;
		int drawnRows = 0;
		for (const GroupDesc& group : kGroups)
		{
			std::vector<const RowPlan*> rows;
			for (const RowPlan& plan : plans)
				if (plan.Group == group.Key)
					rows.push_back(&plan);
			if (rows.empty())
				continue;
			const bool open = m_SectionOpen[group.Index] || !needle.empty();
			const int modified = GroupModifiedCount(group.Key);
			const int rowCount = static_cast<int>(rows.size());
			// U22(用户 §5.2「折叠设计的怪怪的,分别区分不出来折叠内容属于哪里」):
			// 组 = **容器**:底 + 圆角描边 + 左侧归属竖条 + 子项缩进 + 组间留白。
			// 先把整块尺寸算出来(组头 + 标签条 + 属于本标签的行),再一次性画底。
			float blockHeight = kGroupHeaderHeight;
			if (open)
			{
				for (const RowPlan* plan : rows)
					blockHeight += plan->Height;
			}
			const Wui::WuiRect blockRect { contentRect.X + 2.0f, y - 3.0f,
				std::max(40.0f, contentRect.W - 12.0f), blockHeight + 4.0f };
			// 滚动裁剪由 BeginScrollArea(ClipPush)负责,这里只跳过"整块在视口之外"的情况。
			const bool blockVisible = (blockRect.Y + blockRect.H > contentRect.Y)
				&& (blockRect.Y < contentRect.Y + contentRect.H);
			if (blockVisible)
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, blockRect, theme.ContentBg, 6.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, blockRect, theme.Border,
					6.0f, 1.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
					{ blockRect.X + 1.0f, blockRect.Y + 5.0f, 2.0f,
						std::max(6.0f, blockRect.H - 10.0f) },
					modified > 0 ? theme.Warning : theme.BorderStrong, 1.0f });
				// 容器节点(只读):脚本按它断言"组头与本组子项都落在这块容器里"(归属关系),
				// 也方便无障碍读屏讲清"这些行属于哪一组"。
				Wui::WuiAccessNode groupNode;
				groupNode.Id = Wui::HashId((std::string("material.group.") + group.Key).c_str());
				groupNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				groupNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				groupNode.Kind = "group";
				groupNode.Label = GroupLabel(group);
				groupNode.Value = std::to_string(rowCount) + " items, "
					+ std::to_string(modified) + " modified";
				groupNode.Tooltip = Wui::Tr("panel.material.group.tooltip",
					"Group container: its header and every parameter row of this group are drawn "
					"inside this rectangle (rows are indented to show ownership).");
				groupNode.Rect = blockRect;
				groupNode.Enabled = true;
				groupNode.Interactive = false;
				groupNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(groupNode);
			}
			const std::string headerId = std::string("material.section.") + group.Key;
			const Wui::WuiRect headerRect { blockRect.X + 4.0f, y, std::max(40.0f, blockRect.W - 8.0f),
				kGroupHeaderHeight - 4.0f };
			// 与行同口径:滚出视口的组头既不画也不登记(否则树里会出现"用户看不见的节点")。
			const bool headerVisible = (headerRect.Y + headerRect.H > contentRect.Y)
				&& (headerRect.Y < contentRect.Y + contentRect.H);
			if (headerVisible)
			{
				const bool hovered = ctx.IsHovered(headerRect);
				Wui::HoverRow(ctx, headerRect, hovered, false, theme, 4.0f);
				Wui::Label(ctx, { headerRect.X + 8.0f, headerRect.Y + 4.0f }, open ? "v" : ">",
					theme.TextMuted, 12.0f);
				Wui::Label(ctx, { headerRect.X + 22.0f, headerRect.Y + 4.0f }, GroupLabel(group),
					theme.Text, 13.0f);
				// 折叠后也要能看出这一组里有多少项、多少项已改(用户 §5.2)。
				const std::string countText = std::to_string(rowCount) + " "
					+ Wui::Tr("panel.material.group.items", "items")
					+ (modified > 0
						? std::string(" · ") + std::to_string(modified) + " "
							+ Wui::Tr("panel.material.group.modified", "modified")
						: std::string(" · ") + Wui::Tr("panel.material.group.defaults", "defaults"));
				const float countWidth = ctx.MeasureTextWidth(countText, 11.0f);
				Wui::Label(ctx, { headerRect.X + std::max(24.0f, headerRect.W - countWidth - 8.0f),
					headerRect.Y + 6.0f }, countText,
					modified > 0 ? theme.Warning : theme.TextDisabled, 11.0f);
				if (hovered)
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (ctx.IsClicked(headerRect))
					m_SectionOpen[group.Index] = !m_SectionOpen[group.Index];
				const std::string sectionDoc = Wui::Tr("panel.material.section.tooltip",
					"Click to expand or collapse this group.");
				{
					Wui::WuiAccessNode node;
					node.Id = Wui::HashId(headerId.c_str());
					node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					node.Kind = "section";
					node.Label = GroupLabel(group);
					node.Value = (open ? std::string("expanded") : std::string("collapsed")) + ", "
						+ std::to_string(rowCount) + " items, " + std::to_string(modified) + " modified";
					node.Tooltip = sectionDoc;
					node.Rect = headerRect;
					node.Enabled = true;
					node.Interactive = true;
					node.Visible = true;
					Wui::WuiAccessibility::Get().Register(node);
				}
				Wui::Tooltip(ctx, headerRect, sectionDoc);
			}
			y += kGroupHeaderHeight;
			if (!open)
			{
				y += kGroupGap;
				continue;
			}
			for (const RowPlan* plan : rows)
			{
				// 校验条目点击后的定位:在**可见性判断之前**处理 —— 目标行通常正在视口外,
				// 那正是要滚过去的情况(下一帧生效,行高是本帧算出来的)。
				if (!m_RevealField.empty() && plan->Key == m_RevealField)
				{
					if (y < contentRect.Y + 2.0f)
						m_ScrollY = std::max(0.0f, m_ScrollY - (contentRect.Y + 2.0f - y));
					else if (y + plan->Height > contentRect.Y + contentRect.H - 2.0f)
						m_ScrollY = std::min(maxScroll,
							m_ScrollY + (y + plan->Height - (contentRect.Y + contentRect.H - 2.0f)));
					if (--m_RevealFrames <= 0)
						m_RevealField.clear();
				}
				const bool visible = (y + plan->Height > contentRect.Y)
					&& (y < contentRect.Y + contentRect.H);
				if (!visible)
				{
					y += plan->Height;
					continue;
				}
				++drawnRows;
				// 子项缩进:与组头左侧竖条对齐,让"这些行属于上面那一组"一眼可见。
				DrawParameterRow(ctx, host, theme, *plan, contentRect.X + kGroupIndent, y,
					std::max(80.0f, contentRect.W - kGroupIndent - 10.0f), stacked);
				y += plan->Height;
			}
			y += kGroupGap;
		}
		Wui::EndScrollArea(ctx);
		// 滚动指示条(纯视觉,不参与命中):参数比视口长时给一个位置/比例读数 ——
		// 共享的 BeginScrollArea 没有滚动条,"下面还有高级组"必须能被看见。
		if (contentHeight > contentRect.H + 1.0f)
		{
			const float trackHeight = contentRect.H - 8.0f;
			const float thumbHeight = std::max(24.0f, trackHeight * (contentRect.H / contentHeight));
			const float offset = maxScroll > 0.0f ? (m_ScrollY / maxScroll) * (trackHeight - thumbHeight) : 0.0f;
			const Wui::WuiRect track { contentRect.X + contentRect.W - 3.0f, contentRect.Y + 4.0f,
				2.0f, trackHeight };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, track,
				Wui::WuiColor { 0.169f, 0.192f, 0.220f, 1.0f }, 1.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
				{ track.X, track.Y + offset, 2.0f, thumbHeight },
				Wui::WuiColor { 0.298f, 0.553f, 1.0f, 0.55f }, 1.0f });
		}
		if (drawnRows == 0)
		{
			const std::string message = m_Search.empty()
				? Wui::Tr("panel.material.empty", "No parameters in this material.")
				: Wui::Tr("panel.material.empty.filtered", "No parameter matches the search");
			Wui::Label(ctx, { contentRect.X + 8.0f, contentRect.Y + 12.0f },
				EllipsizeToWidth(ctx, message, contentRect.W - 16.0f, 13.0f), theme.TextMuted, 13.0f);
		}
		// ---- 校验区(参数区底部,只在有问题时占位)----
		if (!m_Validation.empty())
		{
			const float blockY = rect.Y + rect.H - validationHeight - refsHeight;
			const Wui::WuiRect blockRect { rect.X, blockY, rect.W, validationHeight };
			Wui::PanelBackground(ctx, blockRect, { 0.20f, 0.14f, 0.06f, 1.0f }, 4.0f);
			const std::string summary = std::to_string(m_Validation.size()) + " "
				+ Wui::Tr("panel.material.validation.issues", "issue(s)");
			Wui::Label(ctx, { blockRect.X + 6.0f, blockRect.Y + 3.0f }, summary, theme.Warning, 12.0f);
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.validation");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "status";
			node.Label = Wui::Tr("panel.material.validation", "Validation");
			for (const ValidationEntry& entry : m_Validation)
				node.Value += (node.Value.empty() ? "" : "; ") + entry.Text;
			node.Tooltip = Wui::Tr("panel.material.validation.tooltip",
				"Click an entry to jump to the parameter that causes it.");
			node.Rect = blockRect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			for (size_t index = 0; index < shownIssues; ++index)
			{
				const ValidationEntry& entry = m_Validation[index];
				const Wui::WuiRect rowRect { blockRect.X + 4.0f,
					blockRect.Y + 18.0f + static_cast<float>(index) * 18.0f, blockRect.W - 8.0f, 17.0f };
				const bool hovered = ctx.IsHovered(rowRect);
				Wui::HoverRow(ctx, rowRect, hovered, false, theme, 2.0f);
				Wui::Label(ctx, { rowRect.X + 4.0f, rowRect.Y + 2.0f },
					EllipsizeToWidth(ctx, entry.Text, rowRect.W - 8.0f, 11.0f), theme.Warning, 11.0f);
				const std::string itemId = "material.validation." + std::to_string(index);
				const std::string itemDoc = Wui::Tr("panel.material.validation.item.tooltip",
					"Click to jump to this parameter.");
				Wui::WuiAccessNode entryNode;
				entryNode.Id = Wui::HashId(itemId.c_str());
				entryNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				entryNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				entryNode.Kind = "validation-item";
				entryNode.Label = entry.Text;
				entryNode.Value = entry.Severity;
				entryNode.Tooltip = itemDoc;
				entryNode.Rect = rowRect;
				entryNode.Enabled = true;
				entryNode.Interactive = true;
				entryNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(entryNode);
				if (hovered)
				{
					ctx.SetCursor(Wui::WuiCursor::Hand);
					Wui::Tooltip(ctx, rowRect, itemDoc);
				}
				if (ctx.IsClicked(rowRect))
				{
					// 定位:清搜索、展开目标组、把该行滚进视野。
					m_Search.clear();
					for (int specIndex = 0; specIndex < kRowSpecCount; ++specIndex)
					{
						if (entry.Field != kRowSpecs[specIndex].Key)
							continue;
						const GroupDesc* target = FindGroup(kRowSpecs[specIndex].Group);
						if (target)
							m_SectionOpen[target->Index] = true;
						// U22:目标行在预览组的非当前标签里时,连标签一起切过去 ——
						// 否则"点击定位"会落到一个当前不显示的行上。
						if (std::string(kRowSpecs[specIndex].Group) == std::string("preview"))
							m_PreviewTab = PreviewTabFor(kRowSpecs[specIndex].Key);
					}
					m_RevealField = entry.Field;
					m_RevealFrames = 6;
				}
			}
		}
		// ---- 引用者条(常驻一行):"被 N 处引用" / "无引用";点开列出引用者,条目可在内容
		// 浏览器里定位(复用 SelectCreated/Reveal 那条选中通道)。----
		DrawReferences(ctx, { rect.X, rect.Y + rect.H - refsHeight, rect.W, refsHeight }, host);
		return rect.H;
	}

	// ---- 预览区:卡片(图像 + 预览设置标签/控件 + "仅影响预览显示"标注 + 读数) ----
	//
	// U23(用户 2026-09-22):「预览这个大分类应该和其他的分开。因为预览只是为了看,它并不影响
	// 实际效果呀。」→ 预览设置(网格/背景/光照/显示)搬进**预览区自己的卡片与标签条**,
	// 参数列只剩会影响材质本身的字段;卡片有底色 + 描边,与参数列明确分隔。
	float MaterialEditorPanel::DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float pad = theme.Pad;
		const float gap = theme.PadSmall;
		const float innerW = std::max(80.0f, rect.W - 2.0f * pad);
		// 预览行:与参数区同一条"窄列换行"口径(宽窗左列只有 220..340 宽)。
		// U23:预览列比参数列窄得多,单行的"标签 + 控件"在 240 设计单位以上都能放下
		// (再窄才改成上下两行),这样图像能拿到更多高度。
		const bool stacked = innerW < kPreviewInlineMinWidth;
		const float rowH = stacked ? kRowHeightStacked : kRowHeight;
		// 标签条高度按**当前最高的标签**预留(光照 4 项 / 显示 3 项 / 网格·背景 1 项):
		// 切标签时预览画面与卡片尺寸都不变 → 离屏目标不重建、切标签不闪。
		float tabRowsH[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int index = 0; index < kRowSpecCount; ++index)
		{
			const RowSpec& spec = kRowSpecs[index];
			if (std::string(spec.Group) != std::string("preview"))
				continue;
			tabRowsH[std::clamp(PreviewTabFor(spec.Key), 0, 3)] += rowH;
		}
		const float rowsReserveH = std::max(std::max(tabRowsH[0], tabRowsH[1]),
			std::max(tabRowsH[2], tabRowsH[3]));
		const float tabRowH = kPreviewTabHeight - 6.0f;
		const float stripH = (tabRowH + gap) + (kPreviewNoteHeight + gap) + rowsReserveH;
		// 图像(正方形)占满可用宽,但给下面的"标签 + 标注 + 控件"留出空间:空间不足时图像先变小。
		const float side = std::clamp(innerW, kPreviewImageMinSide,
			std::max(kPreviewImageMinSide, rect.H - 2.0f * pad - stripH));
		const float cardH = std::min(rect.H, 2.0f * pad + side + stripH);
		const Wui::WuiRect card { rect.X, rect.Y, rect.W, cardH };
		m_PreviewZoneRect = card;
		// 预览区 = 一张卡片(ContentBg 底 + BorderStrong 描边):与右侧参数列"明确分隔"。
		Wui::PanelBackground(ctx, card, theme.ContentBg, theme.Radius + 2.0f);
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, card, theme.BorderStrong,
			theme.Radius + 2.0f, 1.0f });

		const Wui::WuiRect view { card.X + pad, card.Y + pad, side, side };
		m_PreviewRect = view;
		UpdatePreviewTargetSize(view);
		const uint64_t textureId = RenderPreview();
		const std::string hint = Wui::Tr("panel.material.preview.hint",
			"Drag = orbit, wheel = zoom, double-click or F = frame");
		if (textureId != 0)
		{
			// 贴图按**物理像素网格**对齐:目标尺寸 = 这段物理尺寸时,每个屏幕像素正好采样
			// 一个纹素(1:1),线性过滤也不会糊。
			const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
			const Wui::WuiRect pixelView {
				std::round(view.X * uiScale) / uiScale,
				std::round(view.Y * uiScale) / uiScale,
				static_cast<float>(std::lround(view.W * uiScale)) / uiScale,
				static_cast<float>(std::lround(view.H * uiScale)) / uiScale };
			// 渐变背景:预览目标清成透明,先在下面画一层 WUI 渐变(2D 通道,不需要改 3D)。
			if (m_PreviewBackground == PreviewBackground::Gradient)
			{
				Wui::WuiDrawCommand gradient;
				gradient.Kind = Wui::WuiDrawKind::Gradient;
				gradient.Rect = pixelView;
				gradient.Corners = { Wui::WuiColor { 0.24f, 0.27f, 0.33f, 1.0f },
					Wui::WuiColor { 0.24f, 0.27f, 0.33f, 1.0f },
					Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f },
					Wui::WuiColor { 0.05f, 0.06f, 0.08f, 1.0f } };
				ctx.Commands().push_back(std::move(gradient));
			}
			// U22:离屏预览按引擎统一口径贴({0,1,1,-1},与主视口同一行序约定)。
			// 之前用 {0,0,1,1} 会上下镜像:主光(仰角 +45°)会画到球的下方,
			// 用户看到的就是"上下反、左右反"(实测:与 capture.texture 的 flipV 相关度 0.98)。
			Wui::Image(ctx, pixelView, textureId, ViewChrome::kPreviewImageUv, theme);
			// 相机:左键轨道旋转 / 滚轮推拉 / 双击或 F 取景(与模型/预制体面板同一手感)。
			const bool hovered = ctx.IsHovered(pixelView);
			if (ctx.Input().Wheel != 0.0f && hovered)
			{
				const float step = std::max(0.05f, m_CameraDistance * 0.1f);
				m_CameraDistance = std::clamp(m_CameraDistance - ctx.Input().Wheel * step,
					m_CameraMinDistance, m_CameraMaxDistance);
			}
			if (hovered && ctx.Input().MouseClicked[0])
			{
				m_Orbiting = true;
				m_LastMouse = ctx.Input().MousePos;
			}
			if (m_Orbiting && ctx.Input().MouseDown[0] && !ctx.IsDoubleClicked(pixelView))
			{
				const glm::vec2 delta = ctx.Input().MousePos - m_LastMouse;
				m_LastMouse = ctx.Input().MousePos;
				// U22:符号只有一处定义(三个预览面板 + 主视口共用 ViewChrome::ApplyOrbitDrag)。
				// 2026-09-22 用户实测材质预览"上下反、左右反"的真因是**画面上下镜像**
				// (离屏纹理按 {0,1,1,-1} 才正立,见上面 Image 的 UV),不是拖拽符号本身。
				ViewChrome::ApplyOrbitDrag(m_OrbitYaw, m_OrbitPitch, delta, kPitchLimit);
			}
			if (m_Orbiting && !ctx.Input().MouseDown[0])
				m_Orbiting = false;
			if (ctx.IsDoubleClicked(pixelView) || (hovered && ctx.WasKeyPressed(KeyCodes::F)))
				FramePreview();
			if (hovered)
				ctx.SetCursor(m_Orbiting ? Wui::WuiCursor::Hand : Wui::WuiCursor::Arrow);
			// U22:坐标系指示器(方案 §5.5)—— 右下角 48px,跟随预览相机朝向。
			// 画在这里 = 画在预览图像之后,天然压在图像之上;轴顶点用与渲染**同一份**
			// 基向量算(OrbitBasis),不另写一套投影,避免"图正了、轴标还是反的"。
			glm::vec3 axisRight { 1.0f, 0.0f, 0.0f };
			glm::vec3 axisUp { 0.0f, 1.0f, 0.0f };
			ViewChrome::OrbitBasis(m_OrbitYaw, m_OrbitPitch, &axisRight, &axisUp, nullptr);
			m_AxisRect = ViewChrome::DrawAxisIndicator(ctx, pixelView, axisRight, axisUp);
			// 角标:实际离屏目标分辨率(与预制体面板同一口径);移到右上,给轴标让位。
			const std::string targetLabel = std::to_string(m_PreviewTargetW) + "×"
				+ std::to_string(m_PreviewTargetH) + "px";
			const float labelWidth = ctx.MeasureTextWidth(targetLabel, 10.0f);
			Wui::Label(ctx, { pixelView.X + std::max(4.0f, pixelView.W - labelWidth - 6.0f),
				pixelView.Y + 4.0f }, targetLabel, theme.TextDisabled, 10.0f);
			Wui::Label(ctx, { pixelView.X + 6.0f, pixelView.Y + pixelView.H - 14.0f },
				EllipsizeToWidth(ctx, hint, std::max(40.0f, pixelView.W - 90.0f), 11.0f),
				theme.TextDisabled, 11.0f);
			Wui::Tooltip(ctx, pixelView, hint);
		}
		else
		{
			Wui::Label(ctx, { view.X + pad, view.Y + pad },
				Wui::Tr("panel.material.preview_unavailable", "Preview unavailable (RHI device not ready)"),
				theme.TextMuted, 12.0f);
		}
		// ---- 预览设置:分段标签(网格|背景|光照|显示)+ "仅影响预览显示"标注 + 当前标签的控件 ----
		const char* tabKeys[4] = { "mesh", "bg", "light", "display" };
		const std::vector<std::string> tabLabels {
			Wui::Tr("material.preview.tab.mesh", "Mesh"),
			Wui::Tr("material.preview.tab.bg", "Background"),
			Wui::Tr("material.preview.tab.light", "Lighting"),
			Wui::Tr("material.preview.tab.display", "Display") };
		const int activeTab = std::clamp(m_PreviewTab, 0, 3);
		const Wui::WuiRect tabRect { card.X + pad, view.Y + view.H + gap, innerW, tabRowH };
		int tabSelection = activeTab;
		if (Wui::TabBar(ctx, Wui::HashId("material.preview.tabs"), tabRect, tabLabels,
			tabSelection, theme))
			m_PreviewTab = tabSelection;
		// 额外登记**稳定 id**(material.preview.tab.<key>):脚本按 id 点击/断言,
		// 不依赖本地化文案,也不用去猜 TabBar 的派生 id。
		const float tabWidth = tabRect.W / 4.0f;
		for (int index = 0; index < 4; ++index)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId((std::string("material.preview.tab.") + tabKeys[index]).c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "tab";
			node.Label = tabLabels[static_cast<size_t>(index)];
			node.Value = index == activeTab ? "true" : "false";
			node.Tooltip = Wui::Tr("panel.material.preview.tab.tooltip",
				"Preview settings are split into tabs: the preview scene never changes, "
				"only which options you are looking at.");
			node.Rect = { tabRect.X + tabWidth * static_cast<float>(index), tabRect.Y, tabWidth,
				tabRect.H };
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// 归属标注:预览区是"看"的设置,不写进 .wmat —— 用户 2026-09-22 的原话就是这个分界线。
		const std::string previewNote = Wui::Tr("panel.material.preview.note",
			"Preview display only — never written into the material");
		const std::string previewNoteDoc = Wui::Tr("panel.material.preview.note.tooltip",
			"Preview settings change how you look at the material (mesh, background, lighting, "
			"display overlays). They are never saved into the .wmat and never change the material "
			"that objects render with.");
		const Wui::WuiRect noteRect { card.X + pad, tabRect.Y + tabRect.H + gap, innerW,
			kPreviewNoteHeight };
		Wui::Label(ctx, { noteRect.X, noteRect.Y },
			EllipsizeToWidth(ctx, previewNote, noteRect.W, 11.0f), theme.TextDisabled, 11.0f);
		Wui::Tooltip(ctx, noteRect, previewNoteDoc);
		RegisterReadOnlyNode(Wui::HashId("material.preview.note"), previewNote, "preview-only",
			noteRect, previewNoteDoc);
		// 当前标签的控件:与参数行同一个绘制函数(同一套密度与"恢复默认"口径)。
		float rowY = noteRect.Y + noteRect.H + gap;
		for (int index = 0; index < kRowSpecCount; ++index)
		{
			const RowSpec& spec = kRowSpecs[index];
			if (std::string(spec.Group) != std::string("preview"))
				continue;
			if (PreviewTabFor(spec.Key) != activeTab)
				continue;
			// 窗口高度极小时宁可少画一行,也不让控件越出卡片(卡片外面是参数列)。
			if (rowY + rowH > card.Y + cardH + 0.5f)
				break;
			RowPlan plan;
			plan.Group = spec.Group;
			plan.Key = spec.Key;
			plan.ControlId = std::string("material.") + spec.Key;
			plan.Label = Wui::Tr(spec.LabelKey, spec.LabelEn);
			plan.Doc = Wui::Tr(spec.DocKey, spec.DocEn);
			plan.ReadOnly = spec.ReadOnly != 0;
			plan.HasReset = spec.HasReset != 0;
			plan.Modified = plan.HasReset && !plan.ReadOnly && PreviewOptionModified(plan.Key);
			plan.Height = rowH;
			DrawParameterRow(ctx, host, theme, plan, card.X + pad, rowY, innerW, stacked);
			rowY += rowH;
		}
		// 无障碍:预览区节点 + 两条可断言的读数(target / camera)。
		{
			Wui::WuiAccessNode zone;
			zone.Id = Wui::HashId("material.preview.zone");
			zone.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			zone.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			zone.Kind = "group";
			zone.Label = Wui::Tr("panel.material.preview.zone", "Preview");
			zone.Value = "preview-only";
			zone.Tooltip = previewNoteDoc;
			zone.Rect = card;
			zone.Enabled = true;
			zone.Interactive = false;
			zone.Visible = true;
			Wui::WuiAccessibility::Get().Register(zone);

			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "image";
			node.Label = Wui::Tr("panel.material.preview", "Preview");
			node.Value = textureId != 0
				? ("preview of " + m_Path + " (" + std::to_string(m_PreviewTargetW) + "x"
					+ std::to_string(m_PreviewTargetH) + ")")
				: Wui::Tr("panel.material.preview_unavailable", "Preview unavailable (RHI device not ready)");
			node.Tooltip = hint;
			node.Rect = view;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		char targetText[256] = {};
		std::snprintf(targetText, sizeof(targetText),
			"target=%ux%u view=%.0fx%.0f uiScale=%.2f renderScale=%.2f (echo only)",
			m_PreviewTargetW, m_PreviewTargetH, static_cast<double>(m_PreviewViewW),
			static_cast<double>(m_PreviewViewH), static_cast<double>(m_PreviewUiScale),
			static_cast<double>(RenderSettings::RenderScale()));
		RegisterReadOnlyNode(Wui::HashId("material.preview.target"),
			Wui::Tr("panel.material.preview.target", "Preview render target"), targetText,
			{ view.X, view.Y, std::max(20.0f, view.W), 14.0f },
			Wui::Tr("material.prop.preview.target.doc",
				"Preview render target = preview rect in physical pixels, long side clamped to [128, 2048]. "
				"Independent of rendering.render_scale."));
		char cameraText[256] = {};
		std::snprintf(cameraText, sizeof(cameraText),
			"yaw=%.2f pitch=%.2f pitchLimitDeg=89 dist=%.3f minDist=%.3f maxDist=%.3f mesh=%d",
			static_cast<double>(m_OrbitYaw), static_cast<double>(m_OrbitPitch),
			static_cast<double>(m_CameraDistance), static_cast<double>(m_CameraMinDistance),
			static_cast<double>(m_CameraMaxDistance), static_cast<int>(m_PreviewMesh));
		RegisterReadOnlyNode(Wui::HashId("material.preview.camera"),
			Wui::Tr("panel.material.preview.camera", "Preview camera"), cameraText,
			{ view.X, view.Y + 14.0f, std::max(20.0f, view.W), 14.0f }, hint);
		// U22:坐标系指示器读数(探针按它断言"拖拽后 yaw/pitch 的符号"与方向一致性)。
		ViewChrome::RegisterAxisNode(m_AxisRect, "material.axis",
			Wui::Tr("panel.material.axis", "Preview axes"),
			ViewChrome::AxisReadout(glm::degrees(m_OrbitYaw), glm::degrees(m_OrbitPitch), false),
			Wui::Tr("panel.material.axis.tooltip",
				"World axes drawn in the preview's corner: X red, Y green, Z blue. "
				"They follow the preview camera; yaw/pitch of that camera are in the value."));
		return cardH;
	}

	// ---- U25-M2:C 引用者(谁在用这个材质)----
	//
	// 口径(方案 §C):扫内容根里的 .wd / .wprefab / .wmodel,**直接文本匹配**本材质的逻辑路径
	// (归一化分隔符 + 大小写不敏感),不解析字段语义;带 2s TTL,不每帧读盘。
	// 零引用 = 明确写"无引用"(不是空白);扫描失败 = 把可读原因显示出来。
	void MaterialEditorPanel::RefreshReferences(double now, bool force)
	{
		(void)force;
		m_RefsTime = now;
		m_RefsPath = m_Path;
		m_Refs.clear();
		m_RefsError.clear();
		if (m_Path.empty())
			return;   // 未落盘的材质没有资产路径,别人引用不到它
		const std::filesystem::path root = ContentRootPath();
		std::error_code rootError;
		if (!std::filesystem::is_directory(root, rootError))
		{
			m_RefsError = Wui::Tr("panel.material.refs.error.root", "content root not found: ")
				+ root.generic_string();
			return;
		}
		const std::string needle = ToLowerAscii(m_Path);
		std::error_code walkError;
		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root,
			std::filesystem::directory_options::skip_permission_denied, walkError))
		{
			std::error_code typeError;
			if (!entry.is_regular_file(typeError))
				continue;
			const std::string extension = LowerExtension(entry.path().string());
			const char* type = nullptr;
			if (extension == ".wd")
				type = "scene";
			else if (extension == ".wprefab")
				type = "prefab";
			else if (extension == ".wmodel")
				type = "model";
			if (!type)
				continue;
			std::error_code sizeError;
			const uintmax_t size = std::filesystem::file_size(entry.path(), sizeError);
			if (sizeError || size > 16u * 1024u * 1024u)
				continue;   // 超大文件不当文本读(引用一定写在文本头/场景行里)
			std::ifstream file(entry.path(), std::ios::binary);
			if (!file)
				continue;
			std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			if (text.empty())
				continue;
			std::replace(text.begin(), text.end(), '\\', '/');
			if (ToLowerAscii(text).find(needle) == std::string::npos)
				continue;
			std::error_code relativeError;
			const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, relativeError);
			if (relativeError)
				continue;
			m_Refs.push_back({ relative.generic_string(), type });
		}
		if (walkError)
			m_RefsError = Wui::Tr("panel.material.refs.error.scan", "reference scan failed: ")
				+ walkError.message();
		std::sort(m_Refs.begin(), m_Refs.end(),
			[](const RefEntry& left, const RefEntry& right) { return left.Path < right.Path; });
	}

	float MaterialEditorPanel::DrawReferences(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (rect.H <= 0.0f || rect.W <= 0.0f)
			return 0.0f;
		const Wui::WuiTheme& theme = host.Theme();
		const size_t shown = std::min(m_Refs.size(), kRefsMaxShown);
		char counted[192] = {};
		std::snprintf(counted, sizeof(counted),
			Wui::Tr("panel.material.refs.count", "Referenced by %zu asset(s)").c_str(), m_Refs.size());
		std::string label;
		std::string tooltip;
		if (!m_RefsError.empty())
		{
			label = Wui::Tr("panel.material.refs.error.label", "Reference scan failed");
			tooltip = m_RefsError;
		}
		else if (m_Refs.empty())
		{
			// 零引用必须写出来(方案 §C:不是空白)。
			label = Wui::Tr("panel.material.refs.none", "No references");
			tooltip = Wui::Tr("panel.material.refs.none.tooltip",
				"No scene / prefab / model under the content root mentions this material path yet.");
		}
		else
		{
			label = counted;
			tooltip = Wui::Tr("panel.material.refs.tooltip",
				"Click to list the scenes / prefabs / models that mention this material, then click an entry "
				"to select it in the Content Browser. The scan is direct text matching, refreshed every 2s.");
		}
		const std::string display = std::string(m_RefsOpen ? "v  " : "▶  ") + label;
		const Wui::WuiRect rowRect { rect.X, rect.Y, rect.W, kRefsRowHeight };
		const bool hovered = ctx.IsHovered(rowRect);
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
			{ rect.X, rect.Y, rect.W, 1.0f }, theme.BorderStrong, 0.0f });
		if (hovered)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect,
				{ rect.X + 2.0f, rect.Y + 2.0f, std::max(20.0f, rect.W - 4.0f), kRefsRowHeight - 2.0f },
				theme.HoverBg, theme.Radius });
		Wui::Label(ctx, { rect.X + 6.0f, rect.Y + (kRefsRowHeight - 12.0f) * 0.5f },
			EllipsizeToWidth(ctx, display, std::max(20.0f, rect.W - 12.0f), 12.0f),
			m_RefsError.empty() ? theme.Text : theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.refs");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			// value = 纯计数(脚本按它断言"引用者 +1");类型分解放在 tooltip 里。
			node.Value = std::to_string(m_Refs.size());
			node.Tooltip = tooltip;
			node.Rect = rowRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		Wui::DrawFocusRing(ctx, rowRect, Wui::HashId("material.refs"), theme);
		ctx.RegisterFocusable(Wui::HashId("material.refs"), rowRect);
		if (hovered)
		{
			ctx.SetCursor(Wui::WuiCursor::Hand);
			ctx.SetTooltip(tooltip);
		}
		if (ctx.IsClicked(rowRect))
		{
			m_RefsOpen = !m_RefsOpen;
			ctx.RecordOp("material", m_RefsOpen ? "refs-open" : "refs-close", m_Path,
				std::to_string(m_Refs.size()));
		}
		if (!m_RefsOpen)
			return rect.H;
		// 展开:逐条列出引用者(逻辑路径 + 类型);点一条 = 在内容浏览器里选中并定位它。
		for (size_t index = 0; index < shown; ++index)
		{
			const RefEntry& entry = m_Refs[index];
			const Wui::WuiRect itemRect { rect.X + 6.0f,
				rect.Y + kRefsRowHeight + static_cast<float>(index) * kRefsItemHeight,
				std::max(40.0f, rect.W - 12.0f), kRefsItemHeight - 1.0f };
			const bool itemHovered = ctx.IsHovered(itemRect);
			if (itemHovered)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, itemRect, theme.HoverBg, 2.0f });
			const std::string typeLabel = Wui::Tr(("panel.material.refs.type." + entry.Type).c_str(),
				entry.Type == "prefab" ? "Prefab" : (entry.Type == "model" ? "Model" : "Scene"));
			const float typeWidth = std::min(64.0f, ctx.MeasureTextWidth(typeLabel, 11.0f) + 8.0f);
			Wui::Label(ctx, { itemRect.X + 4.0f, itemRect.Y + 3.0f },
				EllipsizeToWidth(ctx, entry.Path, itemRect.W - typeWidth - 12.0f, 12.0f), theme.Text, 12.0f);
			Wui::Label(ctx, { itemRect.X + itemRect.W - typeWidth, itemRect.Y + 4.0f }, typeLabel,
				theme.TextMuted, 11.0f);
			const std::string itemId = "material.refs.item." + entry.Path;
			const std::string itemTooltip = Wui::Tr("panel.material.refs.item.tooltip",
				"Select this asset in the Content Browser (and navigate to its folder).");
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(itemId.c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = entry.Path;
			node.Value = typeLabel;
			node.Tooltip = itemTooltip;
			node.Rect = itemRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			ctx.RegisterFocusable(node.Id, itemRect);
			if (itemHovered)
			{
				ctx.SetCursor(Wui::WuiCursor::Hand);
				ctx.SetTooltip(itemTooltip);
			}
			if (ctx.IsClicked(itemRect))
			{
				if (host.SelectContentAsset(entry.Path, "material-ref"))
				{
					m_Status = Wui::Tr("panel.material.status.ref_located", "Selected in the Content Browser: ")
						+ entry.Path;
					m_StatusIsError = false;
				}
				else
				{
					m_Status = Wui::Tr("panel.material.status.ref_locate_failed",
						"Could not select that asset in the Content Browser: ") + entry.Path;
					m_StatusIsError = true;
				}
			}
		}
		if (m_Refs.size() > shown)
		{
			const std::string more = "+" + std::to_string(m_Refs.size() - shown) + " …";
			Wui::Label(ctx, { rect.X + 10.0f,
				rect.Y + kRefsRowHeight + static_cast<float>(shown) * kRefsItemHeight }, more,
				theme.TextMuted, 11.0f);
		}
		return rect.H;
	}

	// ---- U25-M2:A 把材质接回场景(Assign to Selection / 撤销本次赋值)----
	void MaterialEditorPanel::AssignToSelection(PanelHost& host)
	{
		if (!m_Material)
			return;
		if (m_Path.empty())
		{
			m_Status = Wui::Tr("panel.material.status.assign_nosave",
				"Assign failed: this material has never been saved, so it has no asset path to reference");
			m_StatusIsError = true;
			return;
		}
		Entity target;
		std::string previous;
		std::string message;
		if (!host.AssignMaterialToSelection(m_Path, &target, &previous, &message))
		{
			m_Status = Wui::Tr("panel.material.status.assign_failed", "Assign failed: ")
				+ (message.empty() ? std::string("no selection") : message);
			m_StatusIsError = true;
			return;
		}
		m_AssignUndoValid = target.IsValid();
		m_AssignUndoEntity = target;
		m_AssignUndoPath = previous;
		m_AssignUndoTarget = message;
		m_Status = Wui::Tr("panel.material.status.assigned", "Assign to Selection: ") + message;
		m_StatusIsError = false;
	}

	void MaterialEditorPanel::UndoAssign(PanelHost& host)
	{
		if (!m_AssignUndoValid)
		{
			m_Status = Wui::Tr("panel.material.status.undo_none",
				"Undo Assign: nothing to undo in this window");
			m_StatusIsError = true;
			return;
		}
		std::string message;
		if (!host.SetEntityMaterialPath(m_AssignUndoEntity, m_AssignUndoPath, &message))
		{
			m_Status = Wui::Tr("panel.material.status.undo_failed", "Undo Assign failed: ")
				+ (message.empty() ? std::string("the entity is gone") : message);
			m_StatusIsError = true;
			// 实体已失效(被删除/换场景):不再提供撤销,避免反复失败。
			m_AssignUndoValid = false;
			m_AssignUndoEntity = Entity {};
			return;
		}
		m_Status = Wui::Tr("panel.material.status.undone", "Undo Assign: ") + message;
		m_StatusIsError = false;
		m_AssignUndoValid = false;
		m_AssignUndoEntity = Entity {};
		m_AssignUndoPath.clear();
		m_AssignUndoTarget.clear();
	}

	// ---- U25-M2:D Save As…(变体的入口)----
	std::string MaterialEditorPanel::SaveAsTarget() const
	{
		const std::string name = MaterialBaseName(m_SaveAsName);
		std::string folder;
		if (m_SaveAsFolderIndex >= 0 && m_SaveAsFolderIndex < static_cast<int>(m_SaveAsFolders.size()))
			folder = m_SaveAsFolders[static_cast<size_t>(m_SaveAsFolderIndex)];
		std::string target = folder.empty() ? std::string() : (folder + "/");
		target += name.empty() ? std::string("(name)") : name;
		target += ".wmat";
		return target;
	}

	std::string MaterialEditorPanel::SaveAsNameError() const
	{
		const std::string name = MaterialBaseName(m_SaveAsName);
		if (name.empty())
			return Wui::Tr("panel.material.saveas.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("panel.material.saveas.name.dot", "Name cannot be '.' or '..'");
		for (const char character : name)
			if (character == '\\' || character == '/' || character == ':' || character == '*'
				|| character == '?' || character == '"' || character == '<' || character == '>'
				|| character == '|')
				return Wui::Tr("panel.material.saveas.name.illegal",
					"Name cannot contain \\ / : * ? \" < > |");
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("panel.material.saveas.name.trailing", "Name cannot end with a dot or a space");
		// 方案 §D:不覆盖源文件 —— 目标就是源文件时禁用并说明。
		if (!m_Path.empty()
			&& MaterialLibrary::NormalizePath(SaveAsTarget()) == MaterialLibrary::NormalizePath(m_Path))
			return Wui::Tr("panel.material.saveas.name.source",
				"That is the source file — write the variant under another name (the source is never overwritten)");
		return {};
	}

	void MaterialEditorPanel::OpenSaveAsModal(Wui::WuiContext& ctx, PanelHost& host)
	{
		if (!m_Material)
			return;
		m_SaveAsOpen = true;
		m_OpenConfirmOpen = false;
		m_SaveAsOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_SaveAsFailure.clear();
		m_SaveAsFailureFor.clear();
		// 默认名 = <原名>_variant;默认目录 = 当前材质所在目录(未落盘时 materials/)。
		const std::filesystem::path source(m_Path);
		std::string stem = source.stem().string();
		if (stem.empty())
			stem = "material";
		m_SaveAsName = stem + "_variant";
		std::string folder = source.parent_path().generic_string();
		if (folder.empty())
			folder = "materials";
		m_SaveAsFolders = Editor::AssetCatalog::Dirs();
		auto found = std::find(m_SaveAsFolders.begin(), m_SaveAsFolders.end(), folder);
		if (found == m_SaveAsFolders.end())
		{
			m_SaveAsFolders.push_back(folder);
			std::sort(m_SaveAsFolders.begin(), m_SaveAsFolders.end());
			found = std::find(m_SaveAsFolders.begin(), m_SaveAsFolders.end(), folder);
		}
		m_SaveAsFolderIndex = found == m_SaveAsFolders.end()
			? 0 : static_cast<int>(found - m_SaveAsFolders.begin());
		ctx.SetModal(Wui::HashId("material.saveas.modal"));
		ctx.SetFocus(Wui::HashId("material.saveas.name"));
		ctx.RecordOp("material", "saveas-ask", m_Path, m_SaveAsName);
	}

	void MaterialEditorPanel::CloseSaveAsModal(Wui::WuiContext& ctx, PanelHost& host)
	{
		(void)host;
		m_SaveAsOpen = false;
		m_SaveAsFailure.clear();
		m_SaveAsFailureFor.clear();
		m_SaveAsFolders.clear();
		if (ctx.Modal() == Wui::HashId("material.saveas.modal"))
			ctx.ClearModal();
		ctx.ClosePopup(Wui::HashId("material.saveas.folder"));
	}

	void MaterialEditorPanel::DrawSaveAsModal(Wui::WuiContext& ctx, PanelHost& host)
	{
		if (!m_SaveAsOpen || !m_Material)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.saveas.modal");
		const Wui::WuiTheme& theme = host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.material.saveas.title", "Save Material As…");
		frameDesc.Size = { 560.0f, 350.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			// 模态被别处清掉:收回状态,不留悬空(与 Create Prefab 同一条规则)。
			CloseSaveAsModal(ctx, host);
			return;
		}
		const Wui::WuiId nameId = Wui::HashId("material.saveas.name");
		const Wui::WuiId folderId = Wui::HashId("material.saveas.folder");
		const bool folderPopupWasOpen = ctx.IsPopupOpen(folderId);
		const bool justOpened = ctx.Frame() == m_SaveAsOpenedFrame;
		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 110.0f;
		const float fieldW = frame.W - 126.0f - 72.0f;

		// ---- 名称(后缀 .wmat 自动补,常显在右侧)----
		Wui::Label(ctx, { labelX, frame.Y + 49.0f },
			Wui::Tr("panel.material.saveas.name", "Name"), theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, frame.Y + 44.0f, fieldW, 24.0f };
		const bool nameFocused = ctx.Focus() == nameId;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = Wui::Tr("panel.material.saveas.name", "Name");
		nameA11y.Placeholder = Wui::Tr("panel.material.saveas.name.placeholder", "Variant name");
		Wui::TextField(ctx, nameId, nameRect, m_SaveAsName, theme, nullptr, &nameA11y);
		const bool nameSubmitted = nameFocused && ctx.IsKeyPressed(KeyCodes::Enter);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, frame.Y + 50.0f }, ".wmat", theme.TextMuted, 13.0f);
		const std::string nameError = SaveAsNameError();
		if (!nameError.empty())
			Wui::Label(ctx, { fieldX, frame.Y + 71.0f }, nameError, theme.Danger, 12.0f);

		// ---- 目录(内容根下的目录,可搜索;缺目录会自动新建)----
		const std::string folderLabel = Wui::Tr("panel.material.saveas.folder", "Folder");
		Wui::Label(ctx, { labelX, frame.Y + 99.0f }, folderLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect folderRect { fieldX, frame.Y + 94.0f, fieldW, 24.0f };
		Wui::SearchableCombo(ctx, folderId, folderRect, folderLabel, m_SaveAsFolders,
			m_SaveAsFolderIndex, theme);
		const std::string folderText = (m_SaveAsFolderIndex >= 0
			&& m_SaveAsFolderIndex < static_cast<int>(m_SaveAsFolders.size()))
			? m_SaveAsFolders[static_cast<size_t>(m_SaveAsFolderIndex)] : std::string();
		{
			Wui::WuiAccessNode node;
			node.Id = folderId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = folderLabel;
			node.Value = folderText;
			node.Tooltip = Wui::Tr("panel.material.saveas.folder.tooltip",
				"Folder under the content root (searchable); a missing folder is created on confirm.");
			node.Rect = folderRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const std::filesystem::path folderAbsolute = ContentRootPath() / std::filesystem::path(folderText);
		if (!folderText.empty() && !std::filesystem::is_directory(folderAbsolute))
			Wui::Label(ctx, { fieldX, frame.Y + 121.0f },
				Wui::Tr("panel.material.saveas.folder.note",
					"Folder does not exist yet — it will be created"), theme.Warning, 12.0f);

		// ---- 实时落点回显 ----
		const std::string target = SaveAsTarget();
		if (!m_SaveAsFailure.empty() && m_SaveAsFailureFor != target)
		{
			m_SaveAsFailure.clear();
			m_SaveAsFailureFor.clear();
		}
		const std::string previewLabel = Wui::Tr("panel.material.saveas.preview.label", "Will create");
		Wui::Label(ctx, { labelX, frame.Y + 152.0f }, previewLabel, theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, frame.Y + 150.0f }, target, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.saveas.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = previewLabel;
			node.Value = target;
			node.Tooltip = Wui::Tr("panel.material.saveas.preview.tooltip",
				"Logical path of the file that will be written (folder + name + .wmat).");
			node.Rect = { fieldX, frame.Y + 146.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 目标已存在 = 红字 + 主按钮变 Overwrite(源文件永远不覆盖)----
		std::error_code existsError;
		const bool targetExists = nameError.empty() && std::filesystem::exists(
			ContentRootPath() / std::filesystem::path(target), existsError);
		std::string warningText;
		if (!m_SaveAsFailure.empty())
			warningText = m_SaveAsFailure;
		else if (targetExists)
			warningText = Wui::Tr("panel.material.saveas.exists", "Already exists — overwriting: ") + target;
		if (!warningText.empty())
			Wui::Label(ctx, { fieldX, frame.Y + 176.0f }, warningText, theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.saveas.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.saveas.warning.label", "Warning");
			node.Value = warningText;
			node.Tooltip = Wui::Tr("panel.material.saveas.warning.tooltip",
				"Red line = the target exists and would be overwritten; empty = no conflict.");
			node.Rect = { fieldX, frame.Y + 172.0f, fieldW, 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 底部按钮条:主按钮(Create Variant / Overwrite)+ Cancel ----
		const bool canCreate = nameError.empty();
		const float footerY = frame.Y + frame.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const Wui::WuiRect okRect { frame.X + frame.W - 16.0f - 150.0f, footerY, 150.0f,
			Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { okRect.X - 8.0f - 96.0f, footerY, 96.0f, Wui::ModalFooterHeight };
		const std::string okLabel = targetExists
			? Wui::Tr("panel.material.saveas.overwrite", "Overwrite")
			: Wui::Tr("panel.material.saveas.create", "Create Variant");
		const std::string okTooltip = !canCreate
			? (nameError + " — " + Wui::Tr("panel.material.saveas.ok.disabled",
				"fix the name to enable this button"))
			: (targetExists
				? Wui::Tr("panel.material.saveas.overwrite.tooltip",
					"The file exists: overwrite it with the current material as a variant")
				: Wui::Tr("panel.material.saveas.create.tooltip",
					"Write the variant and keep editing it in this window (the source .wmat stays untouched)"));
		const bool okClicked = ActionButton(ctx, Wui::HashId("material.saveas.ok"), okRect, okLabel,
			okTooltip, canCreate, true, theme);
		const bool cancelClicked = ActionButton(ctx, Wui::HashId("material.saveas.cancel"), cancelRect,
			Wui::Tr("panel.material.saveas.cancel", "Cancel"),
			Wui::Tr("panel.material.saveas.cancel.tooltip", "Close without writing anything (Esc)"),
			true, false, theme);

		bool closeRequested = false;
		if ((okClicked || (nameSubmitted && !justOpened)) && canCreate)
		{
			const std::string base = MaterialBaseName(m_SaveAsName);
			// 变体 = 源材质的**副本**:新实例 + 写盘,源实例与源文件都不动(方案 §D)。
			Ref<Material> variant = MaterialLibrary::Get().CreateDefault(base);
			MaterialDesc desc = m_Material->GetDesc();
			desc.Name = base;
			variant->SetDesc(desc);
			std::string error;
			if (!MaterialLibrary::Get().Save(variant, target, &error))
			{
				m_SaveAsFailure = error.empty() ? std::string("Save As failed") : error;
				m_SaveAsFailureFor = target;
				host.Notify(m_SaveAsFailure);
				WLD_CORE_WARN("Save As failed: {0}", m_SaveAsFailure);
			}
			else
			{
				const std::string savedPath = variant->GetPath();
				ctx.RecordOp("material", "saveas", savedPath, m_Path);
				// 当前面板切到新材质(标题/路径/校验都跟着新文档走)。
				OpenMaterial(savedPath);
				host.SelectContentAsset(savedPath, "material-saveas");
				m_Status = Wui::Tr("panel.material.status.saved_as", "Saved as ") + savedPath
					+ Wui::Tr("panel.material.status.saved_as.note",
						" (source .wmat unchanged; this window now edits the variant)");
				m_StatusIsError = false;
				closeRequested = true;
			}
		}
		else if (cancelClicked)
			closeRequested = true;
		else if (escapePressed)
		{
			// Esc 分层:目录下拉展开时先关下拉,第二次才关模态。
			if (folderPopupWasOpen)
				ctx.ClosePopup(folderId);
			else
				closeRequested = true;
		}
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == modalId)
			CloseSaveAsModal(ctx, host);
	}

	// ---- U25-M2:B 拖放(内容浏览器 ↔ 材质编辑器)----
	//
	// 核心 WUI 的拖拽态是**每个窗口一份**,而内容浏览器(主窗口停靠面板)与材质编辑器
	// (独立窗口/附加标签)不可能同时渲染 —— 所以释放由**源窗口**用全局光标命中这里登记的
	// 落点矩形(屏幕物理像素),投递一次 drop;本面板在渲染时取走。见 Editor::AssetDropBridge。
	bool MaterialEditorPanel::PayloadToLogical(const std::string& payload, std::string* logical)
	{
		if (payload.rfind("file:", 0) != 0)
			return false;
		std::string path = payload.substr(5);
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
			return false;
		if (logical)
			*logical = path;
		return true;
	}

	void MaterialEditorPanel::RegisterSlotDrop(const Wui::WuiContext& ctx, PanelHost& host,
		const std::string& key, const Wui::WuiRect& rect)
	{
		float originX = 0.0f, originY = 0.0f;
		if (!host.PanelWindowScreenOrigin(ctx, &originX, &originY))
			return;
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		Editor::AssetDropBridge::Target target;
		target.Owner = m_PanelId;
		target.Sink = "texture-slot";
		target.Key = key;
		target.PayloadPrefix = "file:";
		target.X = originX + rect.X * scale;
		target.Y = originY + rect.Y * scale;
		target.W = rect.W * scale;
		target.H = rect.H * scale;
		Editor::AssetDropBridge::Get().Register(target);
	}

	bool MaterialEditorPanel::TakeSlotDrop(PanelHost& host, const std::string& key)
	{
		(void)host;
		Editor::AssetDropBridge::Drop drop;
		if (!Editor::AssetDropBridge::Get().TakeDrop(m_PanelId, "texture-slot", key, &drop))
			return false;
		std::string logical;
		if (!PayloadToLogical(drop.Payload, &logical) || !m_Material)
			return false;
		const std::string extension = LowerExtension(logical);
		if (extension == ".wmat")
		{
			// 方案 §B:.wmat 拖到槽位不做特殊处理,只给可读反馈。
			m_Status = Wui::Tr("panel.material.status.drop_wmat_on_slot",
				"This row is a texture slot: drop .png/.jpg/.tga here, or drop the .wmat on the title to open it");
			m_StatusIsError = true;
			return true;
		}
		if (!IsTextureExtension(extension))
		{
			m_Status = Wui::Tr("panel.material.status.drop_type", "Unsupported drop type: ") + logical;
			m_StatusIsError = true;
			return true;
		}
		const MaterialDesc& desc = m_Material->GetDesc();
		const std::string& current = key == "normal" ? desc.NormalTexture : desc.AlbedoTexture;
		if (current == logical)
		{
			m_Status = Wui::Tr("panel.material.status.drop_same", "Already uses this texture: ") + logical;
			m_StatusIsError = false;
			return true;
		}
		// 与下拉选择**同一条写入口**(Material 的 setter → Revision 自增 → 渲染侧重建)。
		const uint32_t revision = m_Material->GetRevision();
		if (key == "normal")
			m_Material->SetNormalTexture(logical);
		else
			m_Material->SetAlbedoTexture(logical);
		if (m_Material->GetRevision() != revision)
			m_Material->MarkDirty(true);
		// 下拉索引与目录缓存跟上(否则回显会落在"(none)")。
		m_CatalogRefreshTime = 0.0;
		RefreshCatalog();
		m_ValidationRevision = 0;
		WLD_CORE_INFO("[material-ui] texture drop '{0}' -> slot '{1}'", logical, key);
		m_Status = Wui::Tr("panel.material.status.texture_dropped", "Texture assigned by drop: ") + logical;
		m_StatusIsError = false;
		return true;
	}

	void MaterialEditorPanel::RegisterHeaderDrop(const Wui::WuiContext& ctx, PanelHost& host,
		const Wui::WuiRect& rect)
	{
		float originX = 0.0f, originY = 0.0f;
		if (!host.PanelWindowScreenOrigin(ctx, &originX, &originY))
			return;
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		Editor::AssetDropBridge::Target target;
		target.Owner = m_PanelId;
		target.Sink = "header";
		target.PayloadPrefix = "file:";
		target.X = originX + rect.X * scale;
		target.Y = originY + rect.Y * scale;
		target.W = rect.W * scale;
		target.H = rect.H * scale;
		Editor::AssetDropBridge::Get().Register(target);
	}

	bool MaterialEditorPanel::TakeHeaderDrop(Wui::WuiContext& ctx, PanelHost& host)
	{
		Editor::AssetDropBridge::Drop drop;
		if (!Editor::AssetDropBridge::Get().TakeDrop(m_PanelId, "header", std::string(), &drop))
			return false;
		std::string logical;
		if (!PayloadToLogical(drop.Payload, &logical))
			return false;
		if (LowerExtension(logical) != ".wmat")
		{
			m_Status = Wui::Tr("panel.material.status.drop_header_type",
				"The title opens .wmat files: drop the texture on a texture slot row instead");
			m_StatusIsError = true;
			return true;
		}
		RequestOpenMaterial(ctx, logical, host);
		return true;
	}

	void MaterialEditorPanel::RequestOpenMaterial(Wui::WuiContext& ctx, const std::string& path,
		PanelHost& host)
	{
		(void)host;
		const std::string normalized = MaterialLibrary::NormalizePath(path);
		if (!m_Path.empty() && normalized == MaterialLibrary::NormalizePath(m_Path))
		{
			m_Status = Wui::Tr("panel.material.status.already_open",
				"Already open in this window: ") + normalized;
			m_StatusIsError = false;
			return;
		}
		if (m_Material && m_Material->IsDirty())
		{
			// 有未保存改动:先确认(不静默丢弃),确认后走同一条 OpenMaterial。
			m_PendingOpenPath = normalized;
			m_OpenConfirmOpen = true;
			ctx.SetModal(Wui::HashId("material.openconfirm.modal"));
			ctx.RecordOp("material", "open-ask", normalized, m_Path);
			return;
		}
		ctx.RecordOp("material", "open-drop", normalized, m_Path);
		OpenMaterial(normalized);
	}

	void MaterialEditorPanel::DrawOpenConfirmModal(Wui::WuiContext& ctx, PanelHost& host)
	{
		if (!m_OpenConfirmOpen)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.openconfirm.modal");
		const Wui::WuiTheme& theme = host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.material.openconfirm.title", "Open another material?");
		frameDesc.Size = { 480.0f, 190.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			m_OpenConfirmOpen = false;
			m_PendingOpenPath.clear();
			return;
		}
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 52.0f },
			Wui::Tr("panel.material.openconfirm.body", "This window has unsaved changes."), theme.Text, 13.0f);
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 74.0f },
			EllipsizeToWidth(ctx, m_PendingOpenPath, frame.W - 32.0f, 12.0f), theme.TextMuted, 12.0f);
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 94.0f },
			Wui::Tr("panel.material.openconfirm.body2",
				"Opening it drops those edits (the file on disk is unchanged)."),
			theme.TextMuted, 12.0f);
		const Wui::ModalResult result = Wui::ModalFooter(ctx, frame,
			Wui::Tr("panel.material.openconfirm.discard", "Discard & Open"),
			Wui::Tr("panel.material.openconfirm.cancel", "Cancel"),
			Wui::HashId("material.openconfirm.ok"), Wui::HashId("material.openconfirm.cancel"),
			true, theme);
		const std::string pending = m_PendingOpenPath;
		const bool openPending = result == Wui::ModalResult::Confirm;
		if (openPending || result == Wui::ModalResult::Cancel || escapePressed)
		{
			m_OpenConfirmOpen = false;
			m_PendingOpenPath.clear();
		}
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (openPending)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			ctx.RecordOp("material", "open-confirmed", pending, m_Path);
			OpenMaterial(pending);
		}
		else if (ctx.Modal() == modalId && !m_OpenConfirmOpen)
			ctx.ClearModal();
	}

	void MaterialEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		if (std::getenv("WLD_TRACE_3D"))
		{
			static int tracedPanel = 0;
			if (tracedPanel < 16)
			{
				tracedPanel++;
				WLD_CORE_INFO("[material-ui] panel OnRender panel='{0}' rect=({1},{2},{3},{4}) material={5}",
					m_PanelId, rect.X, rect.Y, rect.W, rect.H, static_cast<int>(m_Material ? 1 : 0));
			}
		}
		const Wui::WuiTheme& theme = host.Theme();
		// U25-M2:面板级模态(Save As… / 丢弃未保存改动再打开)按"自己的输入封锁"处理 ——
		// 材质面板可能是**独立窗口**(外壳的 m_PanelModalOwner 只作用于主窗口的停靠树),
		// 所以在本面板内容之前登记整窗遮挡,画模态本体之前解开(与 WuiModal 的三段式一致)。
		const bool panelModal = HasPanelModal();
		if (panelModal)
			Wui::BeginModalInputBlock(ctx);
		if (!m_Material)
		{
			Wui::Label(ctx, { rect.X + 10.0f, rect.Y + 10.0f },
				Wui::Tr("panel.material.none_open",
					"No material open: double-click a .wmat file in the Content Browser"),
				theme.TextMuted, 13.0f);
			if (panelModal)
				Wui::EndModalInputBlock(ctx);
			return;
		}
		const float headerHeight = DrawHeader(ctx, { rect.X, rect.Y, rect.W, kHeaderBaseHeight }, host);
		// U23(用户 2026-09-22「材质编辑器上面有空行,太丑了」):标题/路径/动作条与第一块内容
		// 之间只留一个 **PadSmall**(主题令牌,不是魔法数字),不再有空白带;宽窗/窄窗同一条。
		const float pad = theme.Pad;
		const float gap = theme.PadSmall;
		const Wui::WuiRect body { rect.X, rect.Y + headerHeight, rect.W,
			std::max(60.0f, rect.H - headerHeight) };
		const bool wide = rect.W >= kTwoColumnMinWidth;
		Wui::WuiRect previewZone;
		Wui::WuiRect parameterRect;
		if (wide)
		{
			// 宽窗:左 = 预览卡片(图像 + 预览设置标签),右 = 可搜索的材质参数区。
			const float previewColumn = std::clamp(body.W * 0.40f, 220.0f, 340.0f);
			previewZone = { body.X + pad, body.Y + gap, previewColumn,
				std::max(80.0f, body.H - gap - pad) };
			parameterRect = { body.X + previewColumn + 2.0f * pad, body.Y + gap,
				std::max(160.0f, body.W - previewColumn - 3.0f * pad),
				std::max(80.0f, body.H - gap - pad) };
			DrawPreview(ctx, previewZone, host);
		}
		else
		{
			// 窄窗单列:头部 → 预览(含预览设置标签) → 参数,三段不重叠(方案 §B)。
			const float width = std::max(80.0f, body.W - 2.0f * pad);
			// 预览卡片先按内容要高度,但给参数区留出至少 kParamsMinHeight(否则材质参数一行都看不到)。
			const float cardMax = std::max(kPreviewImageMinSide + 2.0f * gap,
				body.H - gap - kParamsMinHeight - pad);
			previewZone = { body.X + pad, body.Y + gap, width, cardMax };
			const float previewUsed = DrawPreview(ctx, previewZone, host);
			parameterRect = { body.X + pad, previewZone.Y + previewUsed + pad, width,
				std::max(60.0f, body.H - gap - previewUsed - 2.0f * pad) };
		}
		DrawParameters(ctx, parameterRect, host);
		if (panelModal)
		{
			Wui::EndModalInputBlock(ctx);
			DrawSaveAsModal(ctx, host);
			DrawOpenConfirmModal(ctx, host);
		}
	}
}
