#include "wldpch.h"
#include "WUI/Panels/ContentBrowserPanel.h"
#include "WUI/Common/EditorAssetCatalog.h"
#include "WUI/Common/EditorAssetTypes.h"
#include "WUI/Shell/EditorShell.h"
#include "Core/EditorResources.h"

#include "World/Core/KeyCodes.h"
#include "World/Core/Application.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
// M4-S2/Slang-B1:Material Shader(`.slang`)的起始代码取自内核的 MaterialSurfaceCompiler
// (不在这里再抄一份表面函数骨架);契约注释头由 ShaderContractHeader() 提供。
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/Texture/TextureCompiler.h"
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
namespace ContentBrowserPanelDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace ContentBrowserPanelDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace ContentBrowserPanelDetail
	{
bool IsPluginManifestPath(const std::filesystem::path& path);

void RegisterDisabledMenuItem(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& reason);

std::string FormatBytes(size_t bytes);

std::string TextureArtifactBadgeText(Editor::TextureArtifactState state);

const char* TextureArtifactBadgeCode(Editor::TextureArtifactState state);

bool IsWithinOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& root);

bool IsHiddenContentArtifact(const std::filesystem::path& path);

std::filesystem::path MakeUniqueFileName(const std::filesystem::path& dir, const std::string& fileName);

bool IsTextureSourcePath(const std::filesystem::path& path);

void AppendInjectedDroppedFiles(std::vector<std::string>& dropped);

std::string RenameNameWithExtension(const std::filesystem::path& target, const std::string& stemEdit);

bool EqualsNoCaseAscii(const std::string& left, const std::string& right);

std::string RenameErrorFor(const std::filesystem::path& target, const std::string& newName);


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

		// ---- M4-S2 / Slang-B1:材质着色器(`.slang`)在切片里的**独立标识** ----
		// 引擎的通用文件图标只有"文件/目录"两枚,而着色器是"写表面函数的材质资产"、
		// 与 `.wmat` 实例同级 —— 它必须在网格里一眼可辨(用户口径:独立图标/标签)。
		// 做法:图标整体染成"代码蓝",右上角挂一枚类型徽标(与 prefab 徽标同一套画法,
		// 但用这一组颜色区分开)。不新增二进制图标资源(编辑器 scope 内只有 src/** 与文案)。
		constexpr Wui::WuiColor kShaderIconTint { 0.62f, 0.80f, 1.00f, 1.00f };
		constexpr Wui::WuiColor kShaderBadgeFill { 0.20f, 0.42f, 0.78f, 1.00f };
		constexpr Wui::WuiColor kShaderBadgeText { 0.93f, 0.96f, 1.00f, 1.00f };
Wui::WuiColor MixColor(const Wui::WuiColor& from, const Wui::WuiColor& to, float amount);

std::string LowerAscii(std::string text);

std::string EllipsizeToWidth(const Wui::WuiContext& ctx, std::string_view text, float maxWidth, float fontSize);

std::filesystem::path MakeUniqueAssetPath(const std::filesystem::path& dir, const std::string& base, const std::string& extension);

std::string SanitizeAssetName(const std::string& raw);

std::string AssetBaseName(const std::string& raw);

std::string ShaderBaseName(const std::string& raw);


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

		// M4-S2/Slang-B1:`.slang` 的起始代码模板(两种)——模板内容是**代码**,不是参数预设。
		struct NewShaderTemplate
		{
			const char* LabelKey;
			const char* LabelEn;
			const char* DocKey;
			const char* DocEn;
		};
		const NewShaderTemplate kNewShaderTemplates[] = {
			{ "panel.content_browser.new_shader.tpl.surface", "Standard surface",
				"panel.content_browser.new_shader.tpl.surface.doc",
				"Compilable starting code: Surface Evaluate(MaterialInputs input) with every field "
				"already defaulted by the engine, plus a comment header with the three hard rules "
				"(strict types, combined sampler, explicit [[vk::binding]])." },
			{ "panel.content_browser.new_shader.tpl.params", "Surface + annotated parameters",
				"panel.content_browser.new_shader.tpl.params.doc",
				"Same starting code plus a block of `//! param` declarations that show every type "
				"(float / colour / texture / bool / int) and how ranges, groups and labels are written "
				"(the hard-rules comment header is on both templates)." },
			// MAT-FN4:材质函数(库文件)—— 没有 Evaluate 入口、不单独编译,只被材质 #include。
			// 起始代码与 §9 契约一致:纯函数 + Sampler2D 形参 + 显式类型默认值。
			{ "panel.content_browser.new_shader.tpl.library", "Material function (library)",
				"panel.content_browser.new_shader.tpl.library.doc",
				"Reusable pure functions for materials to `#include`: no Evaluate entry, no material "
				"parameters, not compiled on its own. Lives under assets/shaders/lib/ and is included "
				"as `#include \"lib/<file>.slang\"`; editing it re-bakes every material that includes it." },
		};
		constexpr int kNewShaderTemplateCount =
			static_cast<int>(sizeof(kNewShaderTemplates) / sizeof(kNewShaderTemplates[0]));
		// 材质函数模板的下标(模板表最后一条):切到它时把目录默认到 shaders/lib。
		constexpr int kNewShaderLibraryTemplate = kNewShaderTemplateCount - 1;
std::pair<std::string, std::string> WrapTwoLines(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize);

bool ModalActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& tooltip, bool enabled, bool primary, const Wui::WuiTheme& theme);

	}
}
