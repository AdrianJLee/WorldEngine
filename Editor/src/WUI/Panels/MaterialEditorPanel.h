#pragma once

#include "EditorPanel.h"
#include "SlangCompletion.h"
#include "SlangHighlight.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/MaterialParams.h"
// M4-S3:编译产物(SurfaceArtifact)按值存在面板状态里,头文件必须能看到它的完整定义。
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Renderer.h"
#include "World/RHI/Rhi.h"
#include "World/WUI/WuiTextBuffer.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace World
{
	// D3:材质编辑器(独立窗口形态,与 Widget Gallery / Input Map / 模型预览同级)。
	//
	// U21-M1(用户 2026-09-22 确认的方案 §1.A + §1.C):把"参数表"做成编辑器——
	//  - 头部:材质名 + 来源逻辑路径 + 脏标记 + Save / Reveal / Revert(全部有悬停说明);
	//  - 参数区:按物理意义分六组(基础外观/表面细节/自发光/透明度与混合/贴图采样/高级),
	//    可折叠、可搜索、每字段"恢复默认";
	//    U24(用户 2026-09-22 反馈②③④):数值字段按 `WuiWidgets.h` 的分类规则选控件 ——
	//    感知型归一化区间(金属度/粗糙度/预览光照强度与角度)用 `Wui::DragBarFloat`
	//    (值区常显、点值区可输入、↑/↓ 步进);"恢复默认"改用固定占位的
	//    `Wui::ResetDefaultButton`(偏离/等于默认两态共用同一 rect,行布局零位移);
	//  - 校验区:缺贴图 / 越界 / 引用不在内容根,每条可点击定位到字段(无问题时整块不占位);
	//  - 预览区(U23,用户 2026-09-22「预览这个大分类应该和其他的分开」):预览设置
	//    (网格/背景/光照/显示)住在**预览区自己的卡片与标签条**里,参数列只留会影响材质
	//    本身的字段 —— 预览只影响"看",不写进 .wmat。离屏目标 = **预览区物理像素**
	//    (与 render_scale 脱钩)。
	//  - U27(用户 2026-09-22「预览窗口能不能弄成可伸缩的 / 右侧编辑区域占了一整块,
	//    所有简短的选项都占了一行」):预览列与参数列之间一条可拖拽 `material.splitter`
	//    (两侧最小宽 220/260,双击回到默认比例,比例会话内跨面板记住);参数列与预览设置
	//    都走响应式网格 —— 可用宽度够时 2 格(极宽 3 格)一行,长内容仍独占整行。
	//  - M3(用户 2026-09-22 批准 `.wmat` 支持 `Parent:` + 只存覆盖字段):面板按"继承 vs
	//    覆盖"显示 —— 覆盖的字段带强调条 + 值,未覆盖的字段弱化并给出"继承自 <父>: <值>"
	//    悬停说明;每行的复位按钮语义改为"回退到父级"(无父级 = 回退引擎默认,仍用 U24 的
	//    固定占位原语,零位移);头部显示父级(引擎默认时写 `Inherits: Engine Default`)并给
	//    `打开父材质`(同窗口切文档,有未保存改动先确认);父级缺失时显示可读告警。
	//
	// 相机手感与模型/预制体面板一致:左键轨道旋转、滚轮推拉、双击或 F 取景、上下方向已翻正。
	// 每个材质一个面板/窗口(id = "material:<path>"),可同时打开多个。
	class MaterialEditorPanel final : public EditorPanel
	{
	public:
		MaterialEditorPanel();
		explicit MaterialEditorPanel(std::string materialPath);
		~MaterialEditorPanel() override;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		// M4-S2:代码形态的快捷键(第 2 层路由) —— Ctrl+S 保存源码、Ctrl+R 从磁盘重载。
		// 与脚本编辑器同口径:事件派发在 UI 帧之外,这里只置位,帧内统一消费。
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;

		// 由内容浏览器/Window 菜单调用:打开指定 .wmat(失败时面板显示错误而不是弹窗)。
		void OpenMaterial(const std::string& path);
		// M4-S2/Slang-B1:同一个编辑器的**代码形态** —— 打开 `.slang`(表面函数 + 注解参数)。
		// 双击入口与 `.wmat` 完全相同(ContentBrowserPanel::OpenItem 按扩展名分派形态)。
		void OpenShaderDocument(const std::string& path);
		bool IsShaderDocument() const { return m_ShaderMode; }
		const std::string& GetShaderPath() const { return m_ShaderPath; }
		bool HasMaterial() const { return m_Material != nullptr; }
		const std::string& GetMaterialPath() const { return m_Path; }
		void SetMaterialPathForPanel(const std::string& path);
		// AI 控制通道:请求抓一张预览纹理(下一帧写盘);材质状态供 state.dump 读取。
		void RequestPreviewCapture(const std::string& path) { m_PendingPreviewCapture = path; }
		const Ref<Material>& GetMaterial() const { return m_Material; }

	private:
		// ---- 预览选项(只影响预览,不写进 .wmat)----
		enum class PreviewMesh : uint8_t { Sphere = 0, Cube = 1, Plane = 2 };
		enum class PreviewBackground : uint8_t { Solid = 0, Gradient = 1 };
		enum class PreviewLighting : uint8_t { ThreePoint = 0, Single = 1, None = 2 };

		// 校验区的一条问题(点击 = 展开并滚到对应字段)。
		struct ValidationEntry
		{
			std::string Field;    // 参数名(base / metallic / albedo / normal / …)
			std::string Text;     // 人类可读说明
			std::string Severity; // "missing" / "range" / "unreferenced"
		};

		// 参数区的一行(可编辑字段 / 只读信息行 / 动作行)。
		// Key 决定控件 id(material.<key>)、复位 id(material.prop.<key>.reset)与校验定位。
		struct RowPlan
		{
			std::string Group;      // 组 key(preview / base / detail / emissive / blend / sampling / advanced)
			std::string Key;        // 参数名
			std::string ControlId;  // 控件无障碍 id(与 Key 不同:name 用 material.prop.name)
			std::string Label;
			std::string Doc;
			std::string Value;      // 只读行显示值
			bool ReadOnly = false;
			bool Modified = false;
			bool HasReset = false;
			float Height = 26.0f;
			// ---- M3:继承/覆盖 ----
			// HasField = 这一行是不是可继承的材质字段(只有它们有"继承 vs 覆盖"语义);
			// Override = 本文件显式写了这个字段(覆盖位);未覆盖时按父级链取值。
			bool HasField = false;
			MaterialField Field = MaterialField::Name;
			bool Override = false;
		};

		// ---- 文档状态 ----
		std::string m_PanelId = "material:";
		std::string m_PanelTitle = "Material";
		Ref<Material> m_Material;

		// ---- M4-S2/Slang-B1:着色器(`.slang`)代码形态(同一个面板的第二种形态)----
		// 形态由**打开的文件扩展名**决定;代码形态下 m_Material 是引擎默认表面材质(预览替身),
		// 参数列与代码列都走下面这套状态,不触碰 `.wmat` 的字段/继承逻辑。
		bool m_ShaderMode = false;
		std::string m_ShaderPath;              // 逻辑路径(相对内容根)
		// MAT-FN3:这份 `.slang` 是**材质函数库**(内容根下 `shaders/lib/**`)—— 不是材质资产:
		// 没有 `Evaluate` 入口,不单独编译/烘焙,只被材质 `#include` 引用
		// (docs/dev/shader-contract.md §9)。参数列因此不显示 `//! param` 表,改用统一说明;
		// 编辑 / 保存 / 磁盘热重载与普通 `.slang` 完全相同。
		bool m_ShaderIsLibrary = false;
		// `.wmat` 形态:`Shader:` 引用的参数组是否展开(与其它分组同一条"折叠也看得见计数"的规则)。
		bool m_ShaderParamsOpen = true;
		Wui::WuiTextBuffer m_ShaderBuffer;     // 源码(编辑 / 撤销 / 脏标记)
		SlangHighlightCache m_ShaderHighlight;
		// MAT-INTEL:补全/Hover 的文档表(字段表来自 MaterialSurfaceContract,文件参数来自缓冲)。
		// MAT-INTEL3:补全 + Hover 的候选/文档。SetFileSource 同时建"保守语义索引"
		// (形参 / 局部变量 / 文件级声明 / 类型名);补全用 QueryAt(带光标行)做作用域过滤,
		// 高亮用 DeclaredNames() 的名字集合着色(不看作用域)。
		SlangCompletionIndex m_ShaderCompletion;
		uint64_t m_ShaderCompletionRevision = ~0ull;   // 已喂给索引的缓冲区版本
		std::vector<MaterialParamDecl> m_ShaderParams;
		std::string m_ShaderParseError;        // `ParseMaterialParams` 的 `<行>:<列>: <原因>`
		int m_ShaderErrorLine = 0;             // 解析错误行(1 基;0 = 无错误;进 CodeEditor 红标)
		std::string m_ShaderStatus;
		bool m_ShaderStatusIsError = false;
		bool m_PendingShaderSave = false;      // OnShortcut 只置位,帧内消费(与脚本编辑器同口径)
		bool m_PendingShaderRevert = false;
		bool m_PendingShaderFormat = false;    // MAT-INTEL:格式化(Ctrl+Shift+F / 工具条按钮)
		bool m_ShaderCompileScheduled = false; // 面板自绘的"编译"按钮 → 下一帧执行(避免在绘制中调工具)
		// 会话内记住代码列宽(与预览列宽的记住口径一致;<= 0 = 还没设过)。
		float m_ShaderCodeColumnWidth = 0.0f;
		// MAT-UI6b:代码列的**会话缩放**(与脚本编辑器各一份,互不影响)。内核(WuiCodeEditor)是
		// 唯一事实源:宿主把这里的值作为 options.UiZoom 初值播种,之后每帧回读 result.UiZoom;
		// 只活在本次会话,任何缩放路径都不写偏好文件(editor-prefs.json 逐字节不变)。
		float m_ShaderZoom = 1.0f;
		// MAT-UI6b:状态行 `Font N%` 指示的保留截止时间(秒,记时器 = 本文件的 ShaderWallClockSeconds)。
		double m_ShaderZoomIndicatorUntil = 0.0;
		// ---- M4-S3:代码态实时预览(防抖 + 后台编译 + 键分离)----
		// 一条结构化诊断:Line/Column 是**用户源**的 1 基行列号(0 = 不在用户源里,非用户源诊断报的是包装模板)。
		struct ShaderDiagnostic
		{
			std::string Severity;   // "error" / "warning"
			int Line = 0;
			int Column = 0;
			std::string Message;
			bool InUserSource = false;
		};
		// Slang-B1:诊断分类(前 5 类给"这是什么 + 怎么修";其余归 Other,不给解释)。
		enum class ShaderDiagnosticClass : uint8_t
		{
			Other = 0,
			Truncation,     // E30019 / E39999:隐式截断(严格类型)
			Syntax,         // E20002:语法错
			MissingReturn,  // E41009 / E41010:非 void 函数必须返回值
			Sampler,        // 组合采样器写法(含 E31000 / E39029 的旧写法)
			Entry,          // E38000:找不到入口函数
		};
		// 后台编译的输入 / 输出:工作线程只碰传给它的副本,结果经互斥量交回主线程。
		struct ShaderCompileRequest
		{
			uint64_t Serial = 0;
			std::string Source;
			std::string PermutationKey;   // 编译缓存键 = 逻辑路径(与 M4-S2 同口径)
			// Slang-T4a:本次编译的目标 = 起请求时的设备后端(Vulkan / GL 各一份 SPIR-V;
			// 装配侧的 MaterialSurfaceRuntime::Install 会校验 artifact.Backend 与设备一致)。
			// 在主线程取,避免工作线程读渲染器状态。
			SurfaceShaderBackend Target = SurfaceShaderBackend::VulkanSpirV;
			// MAT-FN3:材质 `#include` 的解析根(绝对路径;在主线程按当前逻辑路径算好)——
			// 材质自身目录 + 内容根下的 `shaders/`(`#include "lib/pattern.slang"` 命中的那一层)。
			// 传给 MaterialSurfaceCompiler 的 includeRoots;被包含文件的内容哈希进缓存键。
			std::vector<std::filesystem::path> IncludeRoots;
		};
		struct ShaderCompileOutcome
		{
			uint64_t Serial = 0;
			bool Success = false;
			bool CacheHit = false;
			std::string Source;           // 这份结果对应的源(主线程据此判"键"与"是否已过期")
			SurfaceArtifact Artifact;
			std::vector<ShaderDiagnostic> Diagnostics;
			std::string RawToolOutput;
			double ElapsedMs = 0.0;
		};
		// 派工口径:最后一次编辑后 350ms 触发一次编译(300–500 可调)。
		static constexpr double kShaderCompileDebounceSeconds = 0.35;
		// MAT-UI2:诊断"定稿"延迟 —— 编辑后 1s 内或括号未闭合时,不把编译错误当最终结果展示
		// (用户报"代码还没写完就报错");预览仍按 350ms 消抖照常编译,只影响错误的**呈现**。
		static constexpr double kShaderDiagnosticsSettleSeconds = 1.0;
		// 着色器热重载的轮询节流(单个文件 stat,不递归目录)。
		static constexpr double kShaderDiskPollSeconds = 0.75;
		std::string m_ShaderCompileStatus;      // 状态行:成功 = 字节数 + 耗时 + 键;失败 = 第一条错误(含行列号)
		bool m_ShaderCompileFailed = false;
		std::vector<ShaderDiagnostic> m_ShaderDiagnostics;
		// 后台编译线程:单飞 + 后来者覆盖前者;主线程**绝不**跑编译器。
		std::thread m_ShaderCompileThread;
		std::mutex m_ShaderCompileMutex;
		std::condition_variable m_ShaderCompileCv;
		ShaderCompileRequest m_ShaderCompileRequest;   // 待编译请求(新请求覆盖旧的未启动请求)
		bool m_ShaderCompileRequestPending = false;
		ShaderCompileOutcome m_ShaderCompileOutcome;   // 未消费的结果(新结果覆盖未消费的旧结果)
		bool m_ShaderCompileResultReady = false;
		bool m_ShaderCompileThreadStop = false;
		uint64_t m_ShaderCompileSerial = 0;
		bool m_ShaderCompileInFlight = false;          // 主线程视角:已有请求在飞(单飞)
		uint64_t m_ShaderRequestedRevision = ~0ull;    // 已投递请求对应的缓冲区版本
		uint64_t m_ShaderSeenRevision = 0;             // 上次看到的缓冲区版本(消抖计时的起点)
		double m_ShaderEditTime = 0.0;                 // 最后一次编辑的墙钟(0 = 没有待编译的编辑)
		double m_ShaderLastEditTime = 0.0;             // 最后一次编辑的墙钟(不被投递清零;诊断定稿用)
		double m_ShaderCompileDispatchedTime = 0.0;    // 投递时刻(状态行显示耗时)
		bool m_ShaderForceCompile = false;             // 打开 / 重载 / 保存 / Compile 按钮:跳过消抖立即编译
		// 键分离(D2)的事实:磁盘内容。缓冲与它相等 = 已保存 → 路径键;不等 = 未保存 → 预览键。
		std::string m_ShaderDiskText;
		std::string m_ShaderInstalledKey;              // 预览材质当前指向的键(诊断/探针可读)
		uint64_t m_ShaderInstalledVersion = 0;         // 该键的发布版本(0 = 从未安装)
		// 最近一次**成功**编译的产物与它对应的源:保存成功时用它把路径键提升到同一份内容
		// (省掉一次重复编译;带错保存时它保留上一份可用产物,正好是"场景仍用上一份"的实现)。
		SurfaceArtifact m_ShaderLastArtifact;
		std::string m_ShaderLastArtifactSource;
		// 着色器热重载:磁盘指纹(未修改的缓冲 → 重载 + 重编译 + Install(路径键))。
		std::filesystem::file_time_type m_ShaderDiskWriteTime {};
		uintmax_t m_ShaderDiskSize = 0;
		bool m_ShaderDiskStampValid = false;
		double m_ShaderDiskPollTime = 0.0;
		bool m_ShaderDiskChangedNotice = false;        // 磁盘变了但缓冲有未保存改动 → 只提示,不覆盖
		// 分组折叠态(注解里的 group("…") 直接当分组名;空组名 = "Parameters")。
		std::map<std::string, bool> m_ShaderGroupOpen;
		// 参数的文本编辑缓冲(Vec2/Vec4 这类没有专用控件的类型用逗号文本输入;
		// 按参数名持有 —— TextField 需要一个跨帧稳定的 std::string)。
		std::map<std::string, std::string> m_ShaderParamTextBuffers;
		// 拖拽中的参数值:拖动期间每帧都在变,只在**松手那一帧**改写注解文本
		// (否则一次拖动会往撤销历史里塞几十步)。
		std::string m_ShaderPendingParamName;
		std::string m_ShaderPendingParamValue;
		// ---- MAT-UI45:代码列 `//! param Color` 行尾色块 ----
		// 逐可见行(其实按全缓冲)扫出来的 Color 注解:`//! param Color <name> = r, g, b[, a]`。
		// 只收**解析成功**的行(名字 / 4 个分量都可解析);非法值不进表 → 不画色块。
		struct ShaderColorSwatch
		{
			int Line = -1;                 // 0 基行号
			std::string Name;              // 参数名(注解里的名字)
			glm::vec4 Rgba { 1.0f };       // 解析出的 RGBA(缺 alpha 按 1)
			std::string Hex;               // `#RRGGBB` / `#RRGGBBAA`(a11y value;与 ColorField 同口径)
		};
		std::vector<ShaderColorSwatch> m_ShaderColorSwatches;
		uint64_t m_ShaderColorSwatchRevision = ~0ull;   // 已扫过的缓冲区版本
		std::string m_Path;                 // 当前材质路径(空 = 未落盘新材质)
		std::string m_NewPathBuffer;        // 未落盘时的目标路径输入
		bool m_NewPathAttempted = false;    // U2d:点过 Save 之后才把"路径不能为空"标成行内错误
		std::string m_Status;               // 最近一次操作结果
		bool m_StatusIsError = false;
		// M3:打开失败的可读原因(m_Material == nullptr 时也要显示 —— 循环引用 / 父级链坏 /
		// 文件读不到都不能只剩一句"没有材质")。
		std::string m_LoadError;
		std::vector<std::string> m_MaterialPaths;
		// ---- M4-TEX-P6a:纹理引用(源图 / `.wtex` 资产)----
		//
		// 一轮纹理引用的**唯一事实源**:`m_TexturePaths`(写进材质的逻辑路径)与
		// `m_TextureLabels`(下拉里的显示名)是两张**平行**表(同一下标 = 同一条目)。
		// 显示名口径(用户 2026-09-25「之前的材质选取纹理也得改」):
		//   * `.wtex` 资产 = 资产主名(`Icon`);
		//   * 没有同名资产的源图 = 文件名(`Icon.png`);
		//   * 同名(同目录同主名)时只留资产条目;显示名撞车时补目录(`props/Icon`)。
		struct TextureCatalogEntry
		{
			std::string Logical;      // 写进材质的逻辑路径(资产 = `.wtex`,源图 = 图片)
			std::string Label;        // 下拉显示名(短名;撞车时带目录)
			std::string Source;       // 资产解析出的源图逻辑路径(源图条目 = 自己)
			bool IsAsset = false;
			bool AssetExists = false;
			bool SourceExists = false;
			bool InContentRoot = true;   // 路径是内容根相对且不含 ".."
			bool InCatalog = true;       // false = 只为"当前引用"补的兜底条目(不在扫描结果里)
			std::string Note;            // 解析/校验的可读原因(空 = 正常)
		};
		// 一次"这条纹理引用能不能用"的判定(校验区 / 行内提示 / 工具提示共用这一份)。
		struct TextureRefInfo
		{
			bool Known = false;          // 非空引用才算 Known
			bool IsAsset = false;
			bool AssetExists = false;
			bool SourceExists = false;
			bool InContentRoot = true;
			std::string Source;          // 资产 → 源图(源图引用 = 自己)
			std::string Error;           // 解析不了的原因(空 = 没有解析错误)
		};
		std::vector<TextureCatalogEntry> m_TextureCatalog;
		std::vector<std::string> m_TextureLabels;
		std::vector<std::string> m_TexturePaths;
		int m_MaterialPickIndex = -1;
		int m_AlbedoPickIndex = 0;
		int m_NormalPickIndex = 0;
		double m_CatalogRefreshTime = 0.0;
		// 纹理选取模态(.slang 形态的 Texture2D 参数:路径框 + 选择 + 清空)。
		// Target = "albedo" / "normal"(.wmat 槽)或 "param:<名字>"(注解参数)。
		bool m_TexturePickerOpen = false;
		std::string m_TexturePickerTarget;
		std::string m_TexturePickerSearch;
		float m_TexturePickerScroll = 0.0f;
		// Texture2D 注解参数的**路径输入缓冲**(TextField 需要跨帧稳定的 std::string)。
		std::map<std::string, std::string> m_TextureParamBuffers;

		// ---- U21:参数区(搜索 / 分组折叠 / 定位)----
		std::string m_Search;               // 搜索框内容(参数名 / 分组 / 说明,不区分大小写)
		float m_ScrollY = 0.0f;             // 参数区滚动位置
		// 分组展开态(下标 = 组序 = kGroups 里材质参数分组的顺序):默认全展开,用户折叠后跨帧保持。
		// U23:预览组搬进预览区后,参数列只剩 6 个材质分组(kGroups 与这里必须同步)。
		bool m_SectionOpen[6] = { true, true, true, true, true, true };
		// 校验条目点击后要把目标字段滚进视野(行高是上一帧实测值,所以保持几帧)。
		std::string m_RevealField;
		int m_RevealFrames = 0;
		std::string m_NameBuffer;           // 高级组的"显示名"文本缓冲(回车/失焦提交)
		bool m_SyncNameBuffer = false;      // 打开/重载后把磁盘值重新灌进缓冲

		// ---- U25-M2:B 工作流(赋值 / 拖放 / 引用者 / Save As)----
		// Assign to Selection:一次"撤销本次赋值"(只回退它刚写的那一次)。
		bool m_AssignUndoValid = false;
		Entity m_AssignUndoEntity;
		std::string m_AssignUndoPath;       // 写入前的 MaterialPath
		std::string m_AssignUndoTarget;     // 当时被写入的实体名(状态行文案)
		// 引用者:扫内容根里 .wd/.wprefab/.wmodel 是否直接提到本材质的逻辑路径(2s TTL)。
		struct RefEntry
		{
			std::string Path;   // 逻辑路径
			std::string Type;   // scene / prefab / model
		};
		std::vector<RefEntry> m_Refs;
		std::string m_RefsPath;             // 这份结果属于哪个材质(换文档立刻重扫)
		std::string m_RefsError;            // 扫描失败的可读原因(空 = 成功)
		double m_RefsTime = 0.0;
		bool m_RefsOpen = false;            // "被 N 处引用"的展开态
		// Save As… 模态(与 U13d 的"创建预制体"同一套:名称 + 目录 + 实时落点 + 覆盖警告 + Enter/Esc)。
		bool m_SaveAsOpen = false;
		uint32_t m_SaveAsOpenedFrame = 0;
		std::string m_SaveAsName;
		std::vector<std::string> m_SaveAsFolders;
		int m_SaveAsFolderIndex = 0;
		std::string m_SaveAsFailure;
		std::string m_SaveAsFailureFor;
		// 拖 .wmat 到头部 = 在本窗口打开它;当前有未保存改动时先确认(不静默丢弃)。
		bool m_OpenConfirmOpen = false;
		std::string m_PendingOpenPath;

		// ---- U21:校验区 ----
		std::vector<ValidationEntry> m_Validation;
		uint32_t m_ValidationRevision = 0;
		std::string m_ValidationPath;
		double m_ValidationTime = 0.0;

		// ---- U21:预览选项与读数 ----
		PreviewMesh m_PreviewMesh = PreviewMesh::Sphere;
		PreviewBackground m_PreviewBackground = PreviewBackground::Solid;
		PreviewLighting m_PreviewLighting = PreviewLighting::ThreePoint;
		float m_LightIntensity = 1.0f;      // 主光强度倍率(0..4)
		float m_LightAzimuth = 35.0f;       // 主光方位角(度,-180..180)
		float m_LightElevation = 45.0f;     // 主光仰角(度,-85..85)
		bool m_ShowWireframe = false;
		bool m_ShowNormals = false;
		bool m_ShowUvChecker = false;
		// 离屏目标 = 预览区物理像素(设计单位 × UiScale,长边 clamp);render_scale 不参与
		// (材质预览自建 framebuffer,不走 SceneRenderer::OnResize,所以天然与它脱钩)。
		uint32_t m_PreviewTargetW = 256;
		uint32_t m_PreviewTargetH = 256;
		float m_PreviewUiScale = 1.0f;
		float m_PreviewViewW = 0.0f;
		float m_PreviewViewH = 0.0f;
		// 预览:上一帧矩形(探针按它核对"目标 = 矩形物理像素")。
		Wui::WuiRect m_PreviewRect;
		// U23:预览区卡片矩形(预览设置与参数列的**分区**依据;探针按它断言归属)。
		Wui::WuiRect m_PreviewZoneRect;
		uint64_t m_PreviewTextureId = 0;
		uint32_t m_UiTextureGeneration = 0;
		// 预览纹理连续抓图(WLD_PREVIEW_TEX_CAPTURE):计数与写出张数,按面板各记一份。
		int m_PreviewCaptureFrame = 0;
		int m_PreviewCaptureWritten = 0;
		// AI 控制通道:请求把**本面板的预览纹理**写到 path(下一帧渲染后执行,
		// 走 RHI 读回,双后端有效)。
		std::string m_PendingPreviewCapture;
		void* m_GpuDevice = nullptr;        // 记录资源所属设备,设备重建时整体失效
		uint32_t m_GpuTargetW = 0;          // 已建 framebuffer 的目标尺寸(0 = 未建)
		uint32_t m_GpuTargetH = 0;
		bool m_Orbiting = false;
		float m_OrbitYaw = 0.6f;            // 弧度:绕 Y
		float m_OrbitPitch = 0.25f;         // 弧度:绕 X
		float m_CameraDistance = 3.0f;      // 预览相机距离(滚轮推拉)
		float m_CameraMinDistance = 0.6f;
		float m_CameraMaxDistance = 30.0f;
		float m_FocusDistance = 3.0f;       // 双击/F 取景距离(按网格尺寸算)
		glm::vec2 m_LastMouse { 0.0f };
		// U22:右下角坐标系指示器的矩形(用于登记 a11y 节点;方案 §5.5)。
		Wui::WuiRect m_AxisRect;
		// U22:预览设置分段标签(0=网格 / 1=背景 / 2=光照 / 3=显示)。会话内记住选中项。
		int m_PreviewTab = 0;

		// ---- GPU 资源(预览专用,惰性创建) ----
		Rhi::Handle<Rhi::RenderPass> m_PreviewPass;
		Rhi::Handle<Rhi::Framebuffer> m_PreviewFramebuffer;
		Rhi::Handle<Rhi::Texture> m_PreviewColor;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityId;
		Rhi::Handle<Rhi::Texture> m_PreviewDepth;
		// P4-4b:rendering.msaa>1 时的多采样附件(颜色 / 实体 id / 深度)。上面三张单采样
		// 纹理保持原角色:WUI 采样纹理、抓图目标与 resolve 目标(m_PreviewColor 也是
		// WLD_PREVIEW_TEX_CAPTURE / capture.texture 读的那张)。
		Rhi::Handle<Rhi::Texture> m_PreviewColorMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewDepthMsaa;
		// P4-UX16b:预览命令缓冲按**帧槽位**各一份(索引 = Renderer::FrameSlot())。
		// 单缓冲 + 每帧重新录制会在"上一帧的提交还没完成"时 begin —— 实测开材质窗口后连续触发
		// VUID-vkBeginCommandBuffer-00049 / VUID-vkQueueSubmit-pCommandBuffers-00071,
		// 最终把设备打丢(device lost)。帧槽位复用与 Renderer::BeginFrame 的帧栅栏配套:
		// 同一槽位再次使用前,该帧的栅栏已经被等待过,缓冲必然已完成。
		Rhi::Handle<Rhi::CommandBuffer> m_PreviewCommands[Renderer::FramesInFlight];
		Rhi::Handle<Rhi::Buffer> m_PreviewCameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_PreviewCameraSet;
		// U21:预览灯光 UBO(set0 binding 2 = LightUniforms)。
		Rhi::Handle<Rhi::Buffer> m_PreviewLightBuffer;
		// U21:预览专用网格与覆盖材质(不落盘、不进资产库缓存;只被本面板引用)。
		Ref<Mesh> m_PreviewMeshes[3];
		Ref<Mesh> m_CheckLightMesh;         // UV 棋盘格:亮格
		Ref<Mesh> m_CheckDarkMesh;          // UV 棋盘格:暗格
		Ref<Mesh> m_WireMesh;               // 线框:沿每个三角形边的细带
		Ref<Mesh> m_NormalMesh;             // 法线:每个顶点一根三棱柱
		int m_DerivedMeshFor = -1;          // 派生网格属于哪个预览网格(-1 = 未构建)
		Ref<Material> m_OverrideLight;      // 纯色覆盖材质(棋盘格亮格)
		Ref<Material> m_OverrideDark;       // 纯色覆盖材质(棋盘格暗格)
		Ref<Material> m_OverrideWire;       // 纯色覆盖材质(线框)
		Ref<Material> m_OverrideNormal;     // 纯色覆盖材质(法线)

		void EnsureGpuResources();
		// defer = true:旧句柄交给引擎的延迟释放队列(下一轮该帧槽位开始前回收)。
		// 目标尺寸变化时必须走这一条 —— 在飞命令还引用着旧 framebuffer/renderpass/命令缓冲,
		// 立刻销毁会触发 VUID-vkDestroyFramebuffer-00892 / VUID-vkFreeCommandBuffers-00047
		// 并把设备打丢(实测 detach 后一次尺寸变化 → 0xC0000005)。
		void ReleaseGpuResources(bool defer = false);
		// 无障碍诊断:按 WLD_PREVIEW_TEX_CAPTURE + WLD_SCREEN_CAPTURE_START/_EVERY/_COUNT
		// 把预览纹理连续写成 PPM(Vulkan 下唯一能"看到"预览内容的路径)。
		void CapturePreviewTextureSequence();
		// 预览目标尺寸 = 预览区物理像素(每帧核对,只有真的变了才重建资源)。
		void UpdatePreviewTargetSize(const Wui::WuiRect& view);
		// 渲染预览到离屏目标;返回可交给 WuiImage 的纹理 id(0 = 不可用)。
		uint64_t RenderPreview();
		// 预览用网格 / 派生网格(线框 / 法线 / UV 棋盘格)与纯色覆盖材质。
		const Ref<Mesh>& PreviewMeshFor(PreviewMesh kind);
		void BuildDerivedMeshes();
		void EnsureOverrideMaterials();
		// 头部(材质名 + 路径 + 脏标记 + 动作);返回占用高度。
		float DrawHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// ---- M4-S2/Slang-B1:着色器代码形态 ----
		// 布局(用户口径):左 预览 | 中 代码(复用 Wui::CodeEditor 内核) | 右 参数(注解默认值)。
		// 代码形态**没有**材质字段区:参数只来自注解,右边改的是 shader 的默认值(改写注解文本)。
		float DrawShaderDocument(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		float DrawShaderHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void DrawShaderCode(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// MAT-INTEL3:代码列帧首 / 补全 / 悬停前都调它 —— 按缓冲区版本刷新补全索引(含语义索引)。
		void EnsureShaderIndex();
		// MAT-INTEL2 探针钩子:把代码列**真正用到的**逐行 token 落盘(白色比例/关键字覆盖率的
		// 量化证据;触发 = WLD_SLANG_TOKEN_DUMP,不设环境变量时不会被调用,不影响正常使用)。
		void DumpShaderTokens(const char* path);
		// MAT-UI45:按缓冲区版本扫描 `//! param Color` 行(供代码列行尾色块 + a11y 节点)。
		void RefreshShaderColorSwatches();
		// MAT-UI45:画一行 Color 注解的色块(棋盘底 + 颜色覆盖 + 1px 描边)+ 登记只读 a11y 节点;
		// 放不下(会压住文本)时直接不画。绘制全部走库件(PanelBackground / HighlightOutline)。
		void DrawShaderColorSwatch(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const ShaderColorSwatch& swatch, const Wui::WuiRect& lineRect, float textEndX);
		float DrawShaderParams(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 从磁盘读源码(打开 / Revert);解析失败也**不丢文件**(面板显示错误行,仍可编辑保存)。
		void LoadShaderFromDisk();
		// 保存:临时文件 + 同目录原子替换(与脚本编辑器/材质保存同一口径)。
		void SaveShaderDocument();
		// MAT-INTEL:轻量格式化(只动空白/缩进,语义不变;写回 WuiTextBuffer → Ctrl+Z 可撤销)。
		void ApplyShaderFormat();
		// ---- M4-S3:代码态实时预览 ----
		// 帧内开头泵一次:消抖计时 → 消费后台结果(回主线程 Install)→ 投递下一次编译(单飞)。
		void PumpShaderCompile(double now);
		// 投递一份编译请求给工作线程(请求/结果都按"后来者覆盖前者"换代)。
		void DispatchShaderCompile(const std::string& source);
		// 工作线程主体:只在这里跑编译器 slangc(不碰任何 UI / 渲染状态)。
		void ShaderCompileWorkerLoop();
		// 主线程:消费一份编译结果 —— 成功才 Install(键按"缓冲是否已保存"选),失败不 Install。
		void ApplyShaderCompileOutcome(const ShaderCompileOutcome& outcome);
		// MAT-UI3b:预览替身材质的注解参数表是否与本帧内存里的注解表一致(名字 + 类型)。
		// 不一致 = 预览管线布局(按**内存注解**编译)与材质表(按**磁盘注解**加载)对不上:
		// 渲染侧打包参数块会失败并退到零值(整块清零 → 预览全黑),所以这份产物先不 Install,
		// 预览沿用上一份可用管线;保存(Ctrl+S)后表与布局重新对齐,新参数才进预览。
		bool PreviewParamTableMatchesMaterial() const;
		// 键(D2):缓冲有未保存改动 → `<路径>#preview`;否则 → `<路径>`(场景与预览共用)。
		std::string ShaderPathKey() const;
		std::string ShaderPreviewKey() const;
		// 磁盘热重载轮询(节流):缓冲未修改 → 重载 + 重编译 + Install(路径键);有改动 → 只提示。
		void PollShaderDiskChange(double now);
		void RecordShaderDiskStamp(const std::string& text);
		// 代码列底部的诊断列表(每条可点击跳行);返回占用高度。
		float DrawShaderDiagnostics(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 诊断 → 单行文本("line L:C  message")。
		static std::string FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic);
		// ---- Slang-B1:诊断码 → 人话(前 5 类;纯 Slang 语义,不挂迁移入口)----
		// 从 `error[E30019]: …` 形态里取稳定码;取不到返回空。
		static std::string ShaderDiagnosticCode(const std::string& message);
		// 分类 = 码 + 消息判据(采样器误用与截断同为 E30019,按消息里的 SamplerState 区分)。
		static ShaderDiagnosticClass ClassifyShaderDiagnostic(const ShaderDiagnostic& diagnostic);
		// "这是什么 + 怎么修"的单行解释(未分类/未知码返回空串,不硬凑文案)。
		static std::string ShaderDiagnosticHelp(const ShaderDiagnostic& diagnostic);
		// 状态行文案:编译结果(成功 = 字节数 + 耗时 + 键;失败 = 第一条错误含行列号)。
		std::string ShaderCompileStatusLine() const;
		// MAT-UI2:诊断是否已"定稿" —— 编辑后 kShaderDiagnosticsSettleSeconds 内,或源码括号未闭合时,
		// 不把编译错误当最终结果展示(预览仍照常按 350ms 消抖编译)。
		bool ShaderDiagnosticsUnsettled() const;
		// 重新解析注解参数表 + 定位错误行;顺带把认识的参数映射进预览替身材质。
		void RefreshShaderParams();
		// 把某个参数的默认值写回**注解文本**(参数默认值的唯一事实源是文件本身)。
		// 失败(找不到注解行 / 改完解析不回来)时回滚 buffer 并写状态行。
		// 参数按**值**传入:函数内部会重建参数表(RefreshShaderParams),引用会失效。
		bool WriteShaderParamDefault(MaterialParamDecl decl, const std::string& valueText);
		// 注解参数 → 预览替身材质(baseColor / metallic / roughness / emissive / albedo / normal)。
		void ApplyShaderDefaultsToPreview();
		// MAT-UI45:拖动中的参数值**只**推给预览替身材质(参数覆盖 + Revision → 渲染侧重打包参数块),
		// 不碰源码文本;松手那一帧仍由 WriteShaderParamDefault 落注解(一次撤销步)。
		void ApplyShaderParamLivePreview(const MaterialParamDecl& decl, const std::string& valueText);
		// 右栏一行的控件(按参数类型;返回是否有编辑,编辑值写进 outText)。
		bool DrawShaderParamControl(Wui::WuiContext& ctx, const Wui::WuiTheme& theme,
			const MaterialParamDecl& decl, const Wui::WuiRect& controlRect, const std::string& current,
			std::string* outText);
		// M4-TEX-P6a:Texture2D 参数的一行怎么排 —— **高度公式与绘制共用的唯一一份**排版结果
		// (内容高度/滚动范围与实际画出来的行必须一致;窄列下先拆标签,再拆控件簇)。
		// inlineControlWidth = "标签与控件并排"时控件能拿到的宽;stackedControlWidth = 标签换行后
		// 控件能拿到的宽(两个都按各站点的排版口径算好再传进来,函数只做判据与高度)。
		struct TextureRowLayout
		{
			bool StackLabel = false;   // 标签独占一行(控件簇与标签并排放不下)
			int ControlLines = 1;      // 控件占几行:1 = 路径框 + 选择 + 清空同行;2/3 = 路径框下面排按钮
			float Height = 26.0f;      // 本行总高(含行内说明)
		};
		TextureRowLayout TextureRowLayoutFor(const Wui::WuiTheme& theme, float inlineControlWidth,
			float stackedControlWidth, bool withError) const;
		// `.wmat` 形态的 shader 参数行:两种排版下控件能拿到的宽(高度计算与绘制共用同一份口径)。
		float ShaderTextureInlineControlWidth(float columnWidth, float labelWidthIn) const;
		float ShaderTextureStackedControlWidth(float columnWidth) const;
		// 参数区(搜索 + 分组 + 每字段控件 + 校验区);返回占用高度。
		float DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// ---- M4-S2:`Shader:` 引用的参数(材质实例形态)----
		// 参数表 = shader 的 `//! param` 注解;本文件只存覆盖(三态:覆盖 / 父级 / shader 默认)。
		// 没有 `Shader:` 的 `.wmat` 这两条都不占位(既有材质面板的布局逐像素不变)。
		float ShaderParamSectionHeight(const Wui::WuiTheme& theme, float columnWidth) const;
		float DrawShaderParamSection(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme,
			const Wui::WuiRect& contentRect, float y);
		// 一行参数:标签 + 控件 + 恢复默认 + 悬停说明 + 无障碍节点。
		//
		// U27(用户 2026-09-22「右侧编辑区域占了一整块,所有简短的选项都占了一行」):
		//  - `labelWidthOverride > 0` = 网格统一的标签列宽(同一行/整个网格里每一格一致);
		//    `< 0` = 按 width 现算(单列口径,与 U21/U22/U24 的几何完全一致);
		//  - `gridCell = true` = 这一格属于多列网格:值区右侧统一留出"恢复默认"槽位,
		//    让各格的标签列与值区严格对齐(只读格也让出同宽,不对齐会看着像两套网格)。
		void DrawParameterRow(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme, const RowPlan& row,
			float x, float y, float width, bool stacked, float labelWidthOverride = -1.0f,
			bool gridCell = false);
		// ---- U27:参数列的响应式网格(网格口径的唯一落点)----
		// 长内容(贴图路径 / 材质名 / 三分量 / 动作行)独占整行;其余短字段按组内**顺序**成对/成行。
		static bool RowSpansFullWidth(const RowPlan& row);
		// 可用宽度 → 列数(1/2/3):每格不小于 kGridCellMinWidth(300 设计单位),不够就回落。
		static int GridColumnCount(float width, const Wui::WuiTheme& theme);
		// 把一组的行切成"网格行":连续短字段按顺序填进同一行(最多 columns 格),
		// 遇到长内容先收尾再独占一行 —— 顺序不变,不把不相关的字段硬凑一行。
		static std::vector<std::vector<const RowPlan*>> BuildGridLines(
			const std::vector<const RowPlan*>& rows, int columns);
		// 预览区(卡片:图像 + 相机操作 + 预览设置标签/控件 + "仅影响预览显示"标注 + 读数);
		// 传入矩形 = 分配到的区域,返回实际占用的高度。
		float DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 双击 / F 取景:回到按网格包围半径算出的默认距离。
		void FramePreview();
		// Reveal:在资源管理器里选中当前 .wmat(未落盘时给出可读状态)。
		void RevealMaterialOnDisk();
		// ---- U25-M2 ----
		// 引用者:重扫(force=true 忽略 TTL)/ 画"被 N 处引用"条(返回占用高度)。
		void RefreshReferences(double now, bool force);
		float DrawReferences(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// 头部动作:赋值给选中实体 / 撤销本次赋值(都走 PanelHost 的字段写入口)。
		void AssignToSelection(PanelHost& host);
		void UndoAssign(PanelHost& host);
		// Save As…(变体):模态的打开/关闭/提交 + 实时落点与校验。
		void OpenSaveAsModal(Wui::WuiContext& ctx, PanelHost& host);
		void CloseSaveAsModal(Wui::WuiContext& ctx, PanelHost& host);
		void DrawSaveAsModal(Wui::WuiContext& ctx, PanelHost& host);
		std::string SaveAsTarget() const;
		std::string SaveAsNameError() const;
		// 拖放:贴图槽(登记屏幕矩形 + 取走投递)与头部(.wmat 切文档)。
		void RegisterSlotDrop(const Wui::WuiContext& ctx, PanelHost& host, const std::string& key,
			const Wui::WuiRect& rect);
		bool TakeSlotDrop(PanelHost& host, const std::string& key);
		void RegisterHeaderDrop(const Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiRect& rect);
		bool TakeHeaderDrop(Wui::WuiContext& ctx, PanelHost& host);
		// 切换本文档的材质:有未保存改动时先弹确认(由调用方在同一帧画)。
		void RequestOpenMaterial(Wui::WuiContext& ctx, const std::string& path, PanelHost& host);
		void DrawOpenConfirmModal(Wui::WuiContext& ctx, PanelHost& host);
		bool HasPanelModal() const { return m_SaveAsOpen || m_OpenConfirmOpen || m_TexturePickerOpen; }
		// payload("file:<逻辑路径>")→ 逻辑路径;前缀不符返回 false。
		static bool PayloadToLogical(const std::string& payload, std::string* logical);
		bool FieldModified(const std::string& key) const;
		// 预览选项是否偏离内置默认(球 / 纯色 / 三点光 / 强度 1 / 方位 35 / 仰角 45 / 关闭)。
		bool PreviewOptionModified(const std::string& key) const;
		// ---- M3:继承 / 覆盖 ----
		// 字段的继承状态:覆盖 / 从父级文件继承 / 引擎内置默认。
		enum class FieldState : uint8_t { Override = 0, Inherited = 1, EngineDefault = 2 };
		FieldState StateOfField(MaterialField field) const;
		// a11y 读数用的状态名(override / inherited / engine-default)。
		static const char* FieldStateName(FieldState state);
		// "继承自 <父>: <值>" / "引擎默认: <值>"(悬停与 a11y tooltip 共用一句)。
		std::string FieldStateDoc(const RowPlan& row, FieldState state) const;
		// 参数键(material.<key>)→ MaterialField;不是可继承字段返回 false。
		static bool FieldForKey(const std::string& key, MaterialField* field);
		// 该组里有多少个覆盖字段(组头 "n 项覆盖" 的唯一口径)。
		int GroupOverrideCount(const std::string& groupKey) const;
		// 头部父级行:`打开父材质`(父级是文件时可用;同窗口切文档,有未保存先确认)。
		void OpenParentMaterial(Wui::WuiContext& ctx, PanelHost& host);
		void SetFieldToDefault(const std::string& key);
		void ResetAllMaterialFields();
		void RefreshValidation(double now);
		void SaveCurrent();
		void RefreshCatalog();
		void RefreshPickerIndices();
		// ---- M4-TEX-P6a:纹理引用(源图 / .wtex 资产)----
		// 扫描内容根 → 资产条目在前、无同名资产的源图在后(唯一口径,刷新走 1.5s TTL)。
		std::vector<TextureCatalogEntry> BuildTextureCatalog() const;
		// 一条引用的现场判定(资产是否存在 / 资产 → 源图是否解析得到且存在 / 是否在内容根内)。
		TextureRefInfo InspectTextureRef(const std::string& logical) const;
		// 当前表里的条目(找不到返回 nullptr;`InCatalog=false` 的兜底条目也算)。
		const TextureCatalogEntry* FindTextureCatalogEntry(const std::string& logical) const;
		// 引用的工具提示句:"资产 → 源图"关系 + 缺失原因(空引用返回空串)。
		std::string TextureRefDoc(const std::string& logical) const;
		// 行内校验的一句话(缺失 / 资产缺源图;正常返回空串)—— 纹理参数行与槽位共用同一份口径。
		std::string TextureInlineWarning(const std::string& logical) const;
		// 该参数的校验里有没有"贴图缺失/资产缺源图"这类问题(行内提示用)。
		bool TextureFieldHasIssue(const std::string& key) const;
		void DrawTextureLocateButton(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme,
			const std::string& logical, const Wui::WuiRect& rect, const std::string& id);
		// 纹理选取模态(.slang Texture2D 参数:能填 / 能选 / 能清空)。
		void OpenTexturePicker(Wui::WuiContext& ctx, const std::string& target);
		void CloseTexturePicker(Wui::WuiContext& ctx);
		void DrawTexturePickerModal(Wui::WuiContext& ctx, PanelHost& host);
		// 采纳一次纹理选取(槽位写材质字段;注解参数写回默认值)。
		void ApplyTextureChoice(const std::string& target, const std::string& logical);
		// MAT-UI3b(用户 2026-09-25「点其他地方这个状态应该就结束了」):面板内部的点击若没有
		// 任何控件接手焦点(点在空白/非焦点控件上),就结束当前聚焦态 —— 焦点环下一帧消失。
		void ResetFocusAfterPanelBlankClick(Wui::WuiContext& ctx, const Wui::WuiRect& panelRect,
			Wui::WuiId focusAtFrameStart);
		// 上面那条规则的"哪些控件会自己接手焦点"名单:绘制时登记矩形,帧首清空。
		// 点在这张表里 = 这一下归那个控件(重复点同一个滑条/输入框不该把状态清掉);
		// 点在这张表外 = 空白 / 按钮 / 只读文字 → 结束聚焦态。
		void NoteFocusOwningRect(const Wui::WuiRect& rect) { m_FocusOwningRects.push_back(rect); }
		std::vector<Wui::WuiRect> m_FocusOwningRects;
	};
}
