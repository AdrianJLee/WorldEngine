#include "wldpch.h"
#include "WUI/Panels/MaterialEditorPanel.h"
#include "WUI/Language/SlangFormat.h"
#include "WUI/Common/EditorAssetCatalog.h"
// M4-TEX-P6a:`ResolveTextureSourceLogical`(资产 → 源图)与设置读盘与纹理设置面板**同一份口径**,
// 不在材质面板里再抄一遍解析规则(那边是唯一实现)。
#include "WUI/Panels/TextureSettingsPanel.h"
#include "WUI/Panels/ViewportPanel.h"
#include "Core/EditorPreferences.h"
// M4-TEX-P11:纹理引用的扫描 / 现场判定 / 徽标 / "源图 → 单文件容器"导入的唯一实现
// (概念归属表 texture-ref 的数据侧;面板不再自己维护候选表与判定)。
#include "WUI/Common/TextureRefCatalog.h"

#include "World/Core/KeyCodes.h"
#include "World/Core/Application.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/RenderSettings.h"
// M4-TEX-P6a:`IsTextureAssetPath` / `LoadTextureImportSettings`(资产引用与源图引用共用一套判定)。
#include "World/Renderer/Texture/TextureImportSettings.h"
// M4-S2:代码形态的"按需编译"走 M4-S1 的编译入口(结构化诊断 + 用户源行列号)。
#include "World/Renderer/MaterialSurface.h"
// M4-S3:编译产物装配成管线(Install 必须在渲染线程 = 本面板的 UI 帧内调用)。
#include "World/Renderer/MaterialSurfaceRuntime.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Utils/Paths.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#endif

namespace World
{
namespace MaterialEditorPanelDetail {}   // 前置声明:下面的 using 必须先见到这个名字
using namespace MaterialEditorPanelDetail;   // 等价于拆分前的文件内匿名命名空间可见性
	namespace MaterialEditorPanelDetail
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
		// ---- U27:可拖拽分隔条 + 响应式网格 ----
		// 用户 2026-09-22:「材质编辑器预览窗口能不能弄成可伸缩的. 材质编辑器右侧编辑区域
		// 占了一整块.你不觉得很不合理吗?所有简短的选项都占了一行.」
		//  - 分隔条两侧各有最小宽度(方案 §A):预览 >= 220、参数列 >= 260 设计单位;
		//  - 网格每格 >= 300 设计单位才开第二/第三格(300 是本面板实测能同时放下
		//    "标签 + 数值控件 + 固定占位"的最小宽度,与 U24 的 90px 控件下限口径一致)。
		constexpr float kSplitterMinPreviewWidth = 220.0f;
		constexpr float kSplitterMinParamsWidth = 260.0f;
		constexpr float kSplitterDefaultRatio = 0.40f;      // 双击复位 = 默认比例
		constexpr float kSplitterDefaultMaxWidth = 340.0f;  // 默认比例上限(U23 起的既有默认)
		constexpr float kGridCellMinWidth = 300.0f;
		constexpr int kGridMaxColumns = 3;
		// ---- M4-S2/Slang-B1:着色器代码形态的三列(预览 | 代码 | 参数)----
		// 宽度门槛与最小列宽按"参数列最优先"排:参数面板是本批的主交付,挤不下时先收代码列。
		constexpr float kShaderThreeColumnMinWidth = 900.0f;
		constexpr float kShaderPreviewMinWidth = 200.0f;
		constexpr float kShaderCodeMinWidth = 240.0f;
		constexpr float kShaderParamsMinWidth = 260.0f;
		constexpr float kShaderDefaultCodeRatio = 0.45f;
		constexpr float kShaderRowHeight = 26.0f;
		constexpr float kShaderGroupHeaderHeight = 20.0f;
float ShaderParamRowHeight(ParamType type, float rowHeight);


		// ---- M4-TEX-P11:纹理引用(`Wui::WuiTexturePicker`)的排版令牌 ----
		// 组件内部自己切"列表 | 徽标条 | 定位按钮(26 + 6)"(WuiTexturePicker.cpp 的
		// kMinComboWidth 64 / kRevealWidth 26 / kSlotGap 6):面板只判断"够不够放下一件完整的
		// 控件",不够就把标签换行 —— 面板不再预留/自绘定位槽与选择/清空按钮。
		constexpr float kTexturePickerMinWidth = 96.0f;
		// 控件行高(与组件默认高度一致)与行内说明的间距(高度公式与绘制共用)。
		constexpr float kTextureControlLineHeight = 22.0f;
		constexpr float kTextureLabelLineHeight = 18.0f;
		constexpr float kTextureWarningGap = 2.0f;
std::string ShaderParamRowTooltip(const MaterialParamDecl& decl, const std::string& meta);

void RegisterShaderParamDocNode(const MaterialParamDecl& decl, const std::string& label, const Wui::WuiRect& rowRect);

float& SessionPreviewColumnWidth();

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
bool ActionButton(Wui::WuiContext& ctx, Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label, const std::string& tooltip, bool enabled, bool primary, const Wui::WuiTheme& theme);

std::string LowerExtension(const std::string& path);

bool IsTextureRefExtension(const std::string& extension);

std::string MaterialBaseName(const std::string& raw);

int PreviewTabFor(const std::string& key);


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
float QuantizeMaterialValue(float value);

glm::vec4 QuantizeMaterialValue(const glm::vec4& value);

glm::vec3 QuantizeMaterialValue(const glm::vec3& value);

std::string ShortenPath(const std::string& path);

std::string ToLowerAscii(const std::string& text);

std::string EllipsizeToWidth(const Wui::WuiContext& ctx, const std::string& text, float width, float fontSize);

std::string NewMaterialPathError(const std::string& raw);

std::filesystem::path ContentRootPath();

bool IsMaterialLibraryShaderPath(const std::string& logicalPath);

std::vector<std::filesystem::path> SurfaceIncludeRoots(const std::filesystem::path& contentRoot, const std::string& shaderLogicalPath);


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
const GroupDesc* FindGroup(const std::string& key);

std::string GroupLabel(const GroupDesc& group);


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
				"Base colour texture; decoded from sRGB to linear by the hardware. Pick a texture asset "
				"(.wtex, listed by asset name) or an image source (.png/.jpg/.jpeg/.tga/.bmp); an asset "
				"row shows its source image next to it. Empty = base colour only. Default: none.",
				0, 1 },
			{ "base", "metallic", "material.prop.metallic", "Metallic", "material.prop.metallic.doc",
				"0 = dielectric, 1 = metal; shifts the diffuse/specular balance. Default: 0.", 0, 1 },
			{ "base", "roughness", "material.prop.roughness", "Roughness", "material.prop.roughness.doc",
				"0 = mirror-like highlight, 1 = fully diffuse. Default: 0.5.", 0, 1 },
			// ---- 表面细节 ----
			{ "detail", "normal", "material.prop.normal", "Normal Texture", "material.prop.normal.doc",
				"Tangent-space normal map (linear colour space). Pick a texture asset (.wtex) or an "
				"image source; empty = flat surface. Default: none.",
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
			{ "advanced", "reset_all", "material.prop.reset_all", "Revert All to Parent",
				"material.prop.reset_all.doc",
				"Drop every field this file overrides so each one inherits again from its parent "
				"(no parent = the engine default). The .wmat is not written until you press Save.",
				0, 0 },
		};
		constexpr int kRowSpecCount = static_cast<int>(sizeof(kRowSpecs) / sizeof(kRowSpecs[0]));
std::string ResetIdFor(const std::string& key);


		// ---- U24:字段的**控件分类**(唯一落点;规则原文见 Engine/src/World/WUI/WuiWidgets.h)----
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
RowControl RowControlFor(const std::string& key);

bool FieldDiffersFromDefault(const std::string& key, const MaterialDesc& desc, const MaterialDesc& defaults);


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
std::vector<SourceVertex> ReadStandardVertices(const Mesh& mesh);

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback = { 0.0f, 1.0f, 0.0f });

void OrthonormalBasis(const glm::vec3& direction, glm::vec3* u, glm::vec3* v);

Ref<Mesh> BuildWireMesh(const Mesh& source, float thickness, float offset);

Ref<Mesh> BuildNormalMesh(const Mesh& source, float length, float radius, float offset);

void BuildCheckerMeshes(const Mesh& source, Ref<Mesh>* light, Ref<Mesh>* dark);

LightUniforms BuildPreviewLightUniforms(int preset, float intensity, float azimuthDeg, float elevationDeg);

void AnnotateNode(Wui::WuiContext& ctx, Wui::WuiId id, const std::string& label, const std::string& tooltip);

void RegisterReadOnlyNode(Wui::WuiId id, const std::string& label, const std::string& value, const Wui::WuiRect& rect, const std::string& tooltip, const char* kind = "text");

	}

	// ---- M4-S3:代码态实时预览(防抖 + 后台编译 + 键分离)----
	//
	// 用户口径(2026-09-23 确认):「只有在保存后才会应用到主场景,如果没保存只是预览中的实时改动」。
	// 实现手段是**键分离**(D2):
	//   - 未保存的实时改动 → Install("<路径>#preview", artifact),预览材质按
	//     `Material::SetSurfaceKeyOverride` 指到这个键(渲染侧的管线选择只看这个键);
	//   - 保存成功       → 写盘 + Install("<路径>", artifact),预览材质指回普通键(这时场景才变);
	//   - 打开既有 `.slang` 先编译已保存内容并 Install("<路径>", …),保证场景/预览有可用基线。
	// 线程纪律:编译器只在**工作线程**跑(单飞,后来者覆盖前者);Install 与渲染状态只在主线程帧内改。
	namespace MaterialEditorPanelDetail
	{
double ShaderWallClockSeconds();

std::string FormatShaderMilliseconds(double value);

bool ShaderBackendIsVulkan();

	}

	// ---- M4-S2:代码列(复用 Wui::CodeEditor 内核)----
	// MAT-UI45:色块的 a11y 文本 —— `#RRGGBB` / `#RRGGBBAA`(与 Wui::ColorField 的 canonical 同口径)。
	namespace MaterialEditorPanelDetail
	{
std::string FormatShaderSwatchHex(const glm::vec4& rgba);

	}
}
