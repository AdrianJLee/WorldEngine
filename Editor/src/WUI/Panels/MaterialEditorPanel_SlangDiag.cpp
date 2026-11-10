#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- Slang-B1:诊断码 → 人话(前 5 类)----
	// 口径来自 docs/dev/shader-contract.md §7(码表)+ 2026-09-23 实测的 slangc 2026.18.2 输出:
	//   E30019/E39999 = 严格类型(隐式截断/无重载),E20002 = 语法错,
	//   E41009/E41010 = 非 void 函数必须返回值,E38000 = 找不到入口函数,
	//   E31000/E39029 = 旧写法残留(combinedImageSampler / register 无 binding)。
	// 采样器误用与截断同为 E30019,只能按消息里是否点到 SamplerState 区分(实测确认)。
std::string MaterialEditorPanel::ShaderDiagnosticCode(const std::string& message){
		// 只认 `[E…]` 形态(Slang 的 `error[E30019]: …`);先找 "[E" 避免把消息里
		// 无关的方括号(数组下标之类)当成诊断码。
		const size_t open = message.find("[E");
		if (open == std::string::npos)
			return {};
		const size_t close = message.find(']', open + 1);
		if (close == std::string::npos || close < open + 3)
			return {};
		const std::string code = message.substr(open + 1, close - open - 1);
		for (const char character : code)
			if (!std::isalnum(static_cast<unsigned char>(character)))
				return {};
		return code;
	}


MaterialEditorPanel::ShaderDiagnosticClass MaterialEditorPanel::ClassifyShaderDiagnostic( const ShaderDiagnostic& diagnostic){
		const std::string code = ShaderDiagnosticCode(diagnostic.Message);
		const bool mentionsSampler = diagnostic.Message.find("SamplerState") != std::string::npos
			|| diagnostic.Message.find("SamplerComparisonState") != std::string::npos;
		if (code == "E30019" || code == "E39999")
			return mentionsSampler ? ShaderDiagnosticClass::Sampler : ShaderDiagnosticClass::Truncation;
		if (code == "E20002")
			return ShaderDiagnosticClass::Syntax;
		if (code == "E41009" || code == "E41010")
			return ShaderDiagnosticClass::MissingReturn;
		if (code == "E38000")
			return ShaderDiagnosticClass::Entry;
		if (code == "E31000" || code == "E39029")
			return ShaderDiagnosticClass::Sampler;
		return ShaderDiagnosticClass::Other;
	}


std::string MaterialEditorPanel::ShaderDiagnosticHelp(const ShaderDiagnostic& diagnostic){
		switch (ClassifyShaderDiagnostic(diagnostic))
		{
			case ShaderDiagnosticClass::Truncation:
				return Wui::Tr("panel.material.shader.diag.help.truncation",
					"Strict types: Slang has no implicit width conversion, so a float4 assigned/returned "
					"as float3 is an error. Fix: truncate explicitly -- `v.xyz` / `v.rgb` -- or "
					"construct the target type.");
			case ShaderDiagnosticClass::Syntax:
				return Wui::Tr("panel.material.shader.diag.help.syntax",
					"Syntax error: the compiler could not parse this line (missing/extra `;` `{` `}` "
					"`(` `)`, or a stray token at the caret). Fix: check the caret position and the "
					"brace/paren balance around it.");
			case ShaderDiagnosticClass::MissingReturn:
				return Wui::Tr("panel.material.shader.diag.help.missing_return",
					"Non-void function must return a value on every path. Fix: end it with "
					"`return <value>;` (every field of `Surface` is optional, the function itself is not).");
			case ShaderDiagnosticClass::Sampler:
				return Wui::Tr("panel.material.shader.diag.help.sampler",
					"Combined-sampler misuse: a texture parameter becomes `Sampler2D` and is sampled as "
					"`Tex.Sample(uv)` -- there is no separate sampler argument, and the old "
					"`register(...)` / `[[vk::combinedImageSampler]]` are not supported. Fix: drop the "
					"sampler argument and use explicit `[[vk::binding(N, S)]]`.");
			case ShaderDiagnosticClass::Entry:
				return Wui::Tr("panel.material.shader.diag.help.entry",
					"Missing entry point: the wrapper could not find the expected entry function (e.g. "
					"`PSMain`). Fix: keep the file's `Evaluate()` signature fixed and do not rename or "
					"remove engine-provided entry points / functions.");
			case ShaderDiagnosticClass::Other:
			default:
				return {};
		}
	}


void MaterialEditorPanel::RecordShaderDiskStamp(const std::string& text){
		(void)text;
		m_ShaderDiskStampValid = false;
		m_ShaderDiskSize = 0;
		if (m_ShaderPath.empty())
			return;
		const std::filesystem::path diskPath = ContentRootPath() / m_ShaderPath;
		std::error_code stampError;
		const std::filesystem::file_time_type writeTime =
			std::filesystem::last_write_time(diskPath, stampError);
		if (stampError)
			return;
		const uintmax_t size = std::filesystem::file_size(diskPath, stampError);
		if (stampError)
			return;
		m_ShaderDiskWriteTime = writeTime;
		m_ShaderDiskSize = size;
		m_ShaderDiskStampValid = true;
	}


void MaterialEditorPanel::DispatchShaderCompile(const std::string& source){
		ShaderCompileRequest request;
		request.Serial = ++m_ShaderCompileSerial;
		request.Source = source;
		// Slang-T4a:目标按**当前设备后端**选(GL 会话编译 GL 目标 SPIR-V,SPIR-V 1.0 +
		// 组合 Sampler2D),Install 侧再校验 artifact.Backend 与设备一致。
		request.Target = ShaderBackendIsVulkan()
			? SurfaceShaderBackend::VulkanSpirV : SurfaceShaderBackend::OpenGLSpirV;
		// 排列键仍是逻辑路径(M4-S2 口径):预览键/路径键只决定 Install 的落点,
		// 不参与编译缓存 —— 同一份内容两边共用产物,不会重复跑编译器。
		request.PermutationKey = m_ShaderPath;
		// MAT-FN3:材质 `#include` 的解析根(绝对路径;在主线程算,工作线程只读)。
		// 先在**当前**主线程取,避免工作线程读面板状态 / 内容根。
		request.IncludeRoots = SurfaceIncludeRoots(ContentRootPath(), m_ShaderPath);
		{
			std::lock_guard<std::mutex> lock(m_ShaderCompileMutex);
			m_ShaderCompileRequest = std::move(request);
			m_ShaderCompileRequestPending = true;
			m_ShaderCompileResultReady = false;   // 旧结果被新请求覆盖(后来者胜)
		}
		m_ShaderCompileInFlight = true;
		m_ShaderCompileDispatchedTime = ShaderWallClockSeconds();
		m_ShaderCompileCv.notify_one();
		WLD_CORE_INFO("[material-ui] shader compile dispatch #{0} ({1} bytes) key='{2}'",
			m_ShaderCompileSerial, source.size(), m_ShaderPath);
	}


void MaterialEditorPanel::ShaderCompileWorkerLoop(){
		for (;;)
		{
			ShaderCompileRequest request;
			{
				std::unique_lock<std::mutex> lock(m_ShaderCompileMutex);
				m_ShaderCompileCv.wait(lock, [this]
				{
					return m_ShaderCompileThreadStop || m_ShaderCompileRequestPending;
				});
				if (m_ShaderCompileThreadStop)
					return;
				request = std::move(m_ShaderCompileRequest);
				m_ShaderCompileRequestPending = false;
			}
			// 工作线程只跑 slangc(内核自带缓存与互斥);不碰 UI / 渲染 / 面板状态。
			const SurfaceCompileResult result =
				MaterialSurfaceCompiler::CompileSurface(request.Source, request.PermutationKey,
					request.Target, request.IncludeRoots);
			ShaderCompileOutcome outcome;
			outcome.Serial = request.Serial;
			outcome.Success = result.Success;
			outcome.CacheHit = result.CacheHit;
			outcome.Source = request.Source;
			outcome.Artifact = result.Artifact;
			outcome.RawToolOutput = result.RawToolOutput;
			outcome.ElapsedMs = result.ElapsedMilliseconds;
			outcome.Diagnostics.reserve(result.Diagnostics.size());
			for (const SurfaceDiagnostic& diagnostic : result.Diagnostics)
			{
				ShaderDiagnostic entry;
				entry.Severity = diagnostic.Severity;
				entry.Message = diagnostic.Message;
				entry.InUserSource = diagnostic.InUserSource;
				entry.Line = static_cast<int>(diagnostic.InUserSource ? diagnostic.UserLine : diagnostic.Line);
				entry.Column = static_cast<int>(diagnostic.InUserSource
					? diagnostic.UserColumn : diagnostic.Column);
				outcome.Diagnostics.push_back(std::move(entry));
			}
			{
				std::lock_guard<std::mutex> lock(m_ShaderCompileMutex);
				if (m_ShaderCompileThreadStop)
					return;
				m_ShaderCompileOutcome = std::move(outcome);
				m_ShaderCompileResultReady = true;
			}
		}
	}


void MaterialEditorPanel::PumpShaderCompile(double now){
		if (!m_ShaderMode)
			return;
		// 1) 编辑 → 起/续消抖计时(Revision 是单调的内容变更计数)。
		const uint64_t revision = m_ShaderBuffer.Revision();
		if (revision != m_ShaderSeenRevision)
		{
			m_ShaderSeenRevision = revision;
			// MAT-FN3:材质函数库不编译 —— 不记编辑时刻,状态行也就不会停在"编译中…"。
			if (!m_ShaderIsLibrary)
			{
				m_ShaderEditTime = now;
				m_ShaderLastEditTime = now;   // MAT-UI2:诊断定稿计时(投递时不清零)
			}
		}
		// 2) 消费后台结果 —— 只有主线程会 Install / 改预览材质。
		ShaderCompileOutcome outcome;
		bool haveOutcome = false;
		{
			std::lock_guard<std::mutex> lock(m_ShaderCompileMutex);
			if (m_ShaderCompileResultReady)
			{
				outcome = std::move(m_ShaderCompileOutcome);
				m_ShaderCompileResultReady = false;
				haveOutcome = true;
			}
		}
		if (haveOutcome)
		{
			m_ShaderCompileInFlight = false;
			if (outcome.Source != m_ShaderBuffer.Text())
			{
				// 这份结果对应的是**当时**的源:源已经变了就当过期丢掉(下一步会编译最新源)。
				WLD_CORE_INFO("[material-ui] shader compile #{0} dropped (source changed)", outcome.Serial);
			}
			else
			{
				ApplyShaderCompileOutcome(outcome);
			}
		}
		// 3) 单飞投递:有请求在飞就不投;消抖到点(或强制:打开 / 重载 / Compile 按钮)才投。
		// MAT-FN3:材质函数库(`shaders/lib/**`)没有 `Evaluate` 入口,不当作材质编译 ——
		// 打开 / 编辑 / 保存都不投递(依赖它的材质各自编译)。上面第 2 步照常消费旧结果,
		// 单飞标记不会卡住(换文档后仍能编译)。
		if (m_ShaderIsLibrary)
			return;
		if (m_ShaderCompileInFlight)
			return;
		const bool sourceStale = revision != m_ShaderRequestedRevision;
		const bool debounceElapsed = m_ShaderEditTime > 0.0
			&& (now - m_ShaderEditTime) >= kShaderCompileDebounceSeconds;
		if (m_ShaderForceCompile || (sourceStale && debounceElapsed))
		{
			m_ShaderForceCompile = false;
			m_ShaderEditTime = 0.0;
			m_ShaderRequestedRevision = revision;
			DispatchShaderCompile(m_ShaderBuffer.Text());
		}
	}


bool MaterialEditorPanel::PreviewParamTableMatchesMaterial() const{
		if (!m_Material)
			return true;
		// 判据只看"内存注解里有的参数,材质的表里有没有、类型一不一样":
		//  - 内存多出来的参数 → 编译出的布局里有这个成员,渲染侧却拿不到它的声明
		//    (decl 缺失 = PackParamValues 直接失败 → 整块写零 → 预览全黑);
		//  - 同名但类型不同 → 偏移/宽度按错的口径写,颜色/向量会读错;
		//  - 内存少掉的参数不影响打包(布局里没有它,表里多一条而已)。
		const std::vector<MaterialParamDecl>& table = m_Material->Params();
		for (const MaterialParamDecl& decl : m_ShaderParams)
		{
			const MaterialParamDecl* known = FindParamDecl(table, decl.Name);
			if (known == nullptr || known->Type != decl.Type)
				return false;
		}
		return true;
	}


void MaterialEditorPanel::ResetFocusAfterPanelBlankClick(Wui::WuiContext& ctx, const Wui::WuiRect& panelRect, Wui::WuiId focusAtFrameStart){
		if (focusAtFrameStart == 0 || ctx.Focus() != focusAtFrameStart)
			return;   // 本帧没有焦点 / 已经有控件接手了焦点(点开下拉、拖滑条、点代码列…)
		if (!ctx.Input().MouseClicked[0] || ctx.IsPointerClickConsumed())
			return;   // 没有点击,或这一下已被弹层(模态 / 下拉)消费
		// 只认"落在这个面板里"的点击:窗口内坐标系由宿主给出,点到别的面板/视口不归本面板管。
		if (!ctx.HitTestRaw(panelRect, ctx.Input().MousePos))
			return;
		// 落在"会自己接手焦点"的控件上(滑条 / 输入框 / 下拉 / 代码列 / 分隔条):这一下归它,
		// 重复点同一个控件不该把状态清掉(拖动条体时会重复按下同一个 id)。
		for (const Wui::WuiRect& rect : m_FocusOwningRects)
			if (ctx.HitTestRaw(rect, ctx.Input().MousePos))
				return;
		// 点在空白(或不吃焦点的按钮/复选框)上 = 结束聚焦/选中态:焦点环下一帧不再画;
		// 读屏与脚本读到的 focused 也回到 false(两个通道同源:都看 ctx.Focus())。
		ctx.SetFocus(0);
	}


void MaterialEditorPanel::ApplyShaderCompileOutcome(const ShaderCompileOutcome& outcome){
		m_ShaderDiagnostics = outcome.Diagnostics;
		m_ShaderCompileFailed = !outcome.Success;
		if (!outcome.Success)
		{
			// 编译失败:**不 Install** —— 上一份可用管线继续用,预览不得变黑。
			std::string firstError;
			for (const ShaderDiagnostic& diagnostic : outcome.Diagnostics)
			{
				if (diagnostic.Severity != "error")
					continue;
				firstError = FormatShaderDiagnostic(diagnostic);
				if (m_ShaderParseError.empty() && diagnostic.InUserSource && diagnostic.Line > 0)
					m_ShaderErrorLine = diagnostic.Line;
				break;
			}
			if (firstError.empty())
			{
				firstError = outcome.Diagnostics.empty()
					? (outcome.RawToolOutput.empty()
						? Wui::Tr("panel.material.shader.compile.no_diagnostics",
							"the compiler returned no diagnostics")
						: outcome.RawToolOutput)
					: FormatShaderDiagnostic(outcome.Diagnostics.front());
			}
			m_ShaderCompileStatus = Wui::Tr("panel.material.shader.compile.failed", "Compile failed")
				+ Wui::Tr("panel.material.shader.compile.failed_hint", " — first error: ") + firstError
				+ Wui::Tr("panel.material.shader.compile.keeps_last",
					"  (the preview and the scene keep the last working pipeline)");
			WLD_CORE_WARN("[material-ui] shader compile failed key='{0}': {1}", m_ShaderPath, firstError);
			return;
		}
		if (m_ShaderParseError.empty())
			m_ShaderErrorLine = 0;
		// 记住这份成功产物:保存成功时用它把路径键提升到同一份内容(不再多跑一次编译器)。
		m_ShaderLastArtifact = outcome.Artifact;
		m_ShaderLastArtifactSource = outcome.Source;
		// 键按"这份源是否等于磁盘内容"选(而不是按请求时刻的脏标记):
		//   相等 = 已保存内容 → 路径键(场景也换);不等 = 未保存的实时改动 → 预览键(只有预览变)。
		const bool savedContents = !m_ShaderDiskText.empty() && outcome.Source == m_ShaderDiskText;
		const std::string key = savedContents ? ShaderPathKey() : ShaderPreviewKey();
		std::string summary = Wui::Tr("panel.material.shader.compile.ok", "Compiled: ")
			+ std::to_string(outcome.Artifact.ByteSize())
			+ Wui::Tr("panel.material.shader.compile.bytes", " bytes of SPIR-V")
			+ Wui::Tr("panel.material.shader.compile.ok_elapsed", " in ")
			+ FormatShaderMilliseconds(outcome.ElapsedMs)
			+ Wui::Tr("panel.material.shader.compile.ok_key", " ms -> key ") + key;
		if (outcome.CacheHit)
			summary += Wui::Tr("panel.material.shader.compile.cached", " (cache hit)");
		if (key.empty())
		{
			m_ShaderCompileStatus = summary + Wui::Tr("panel.material.shader.compile.no_key",
				" — no Install: the shader has no logical path yet");
			return;
		}
		// MAT-UI3b(用户 2026-09-25「把 tint 那行复制一次、改名,左侧预览就变黑了」):
		// 未保存的注解改动里**新增/改名/改类型**的参数,材质的注解表(磁盘那份)还没有 ——
		// 布局(按内存注解编译)与表对不上时渲染侧打包参数块会失败并退到零值(整块清零),
		// 预览就是全黑。这里先不 Install,预览继续用上一份可用管线(参数默认值照旧可见),
		// 并在状态行说清"保存后才进预览"。
		// 取证开关(WLD_MATERIAL_PREVIEW_LAYOUT_GUARD=0)= 关掉这道守卫、回到旧行为(照装):
		// 探针用它做"改前 vs 改后"的**同一二进制**对照(旧行为:渲染侧打包失败 → 写零 → 预览全黑)。
		static const bool previewLayoutGuardDisabled = []
		{
			const char* value = std::getenv("WLD_MATERIAL_PREVIEW_LAYOUT_GUARD");
			return value != nullptr && *value != '\0' && std::string(value) == "0";
		}();
		if (!savedContents && !previewLayoutGuardDisabled && !PreviewParamTableMatchesMaterial())
		{
			m_ShaderCompileStatus = summary + Wui::Tr("panel.material.shader.params.layout_pending",
				" — the parameter set changed (added / renamed / retyped): the preview keeps the last "
				"working pipeline and the new parameters appear after Save (Ctrl+S).");
			WLD_CORE_INFO("[material-ui] preview install skipped (in-memory parameter table differs "
				"from the material's) key='{0}'", key);
			return;
		}
		const MaterialSurfaceRuntime::InstallResult install =
			MaterialSurfaceRuntime::Install(key, outcome.Artifact);
		if (install.Success)
		{
			// 预览材质指向本次的键:未保存时用 `<路径>#preview`(只有预览变),
			// 保存后才指回路径键(与场景共用同一份已发布管线)。
			// 键走 Material::SetSurfaceKeyOverride(不是 ShaderPath):预览替身材质没有
			// 自己的着色器引用,覆盖键只影响渲染侧的管线选择,不写盘、不读注解。
			if (m_Material)
				m_Material->SetSurfaceKeyOverride(key);
			m_ShaderInstalledKey = key;
			m_ShaderInstalledVersion = MaterialSurfaceRuntime::PublishedVersion(key);
			m_ShaderCompileStatus = summary
				+ Wui::Tr("panel.material.shader.compile.ok_version", " (published v")
				+ std::to_string(m_ShaderInstalledVersion) + ")";
			WLD_CORE_INFO("[material-ui] shader pipeline installed key='{0}' variants={1} v{2} ({3} ms)",
				key, install.Pipelines, m_ShaderInstalledVersion, outcome.ElapsedMs);
		}
		else
		{
			// 内核给了结构化原因(后端不支持 / 变体建不出来 / 设备缺失):照实报,预览保持上一份。
			m_ShaderCompileStatus = summary + Wui::Tr("panel.material.shader.compile.not_published",
				" — the pipeline was not published: ")
				+ (install.Error.empty()
					? Wui::Tr("panel.material.shader.compile.unknown_reason", "unknown reason") : install.Error)
				+ Wui::Tr("panel.material.shader.compile.keeps_last",
					"  (the preview and the scene keep the last working pipeline)");
			WLD_CORE_WARN("[material-ui] shader pipeline not published key='{0}': {1}", key, install.Error);
		}
	}


std::string MaterialEditorPanel::ShaderCompileStatusLine() const{
		std::string status;
		// MAT-FN3:库文件永不投递编译,状态行也就没有"编译中…"可言。
		const bool debouncePending = !m_ShaderIsLibrary && m_ShaderEditTime > 0.0
			&& m_ShaderBuffer.Revision() != m_ShaderRequestedRevision;
		// MAT-UI2:编辑后 1s 内的编译结果只是"过程",不当最终错误展示(用户报"代码还没写完就报")。
		const bool unsettled = ShaderDiagnosticsUnsettled();
		if (m_ShaderCompileInFlight)
		{
			const double elapsed = (ShaderWallClockSeconds() - m_ShaderCompileDispatchedTime) * 1000.0;
			status = Wui::Tr("panel.material.shader.compile.running", "Compiling… (background thread, ")
				+ FormatShaderMilliseconds(elapsed) + " ms)";
		}
		else if (debouncePending)
		{
			status = Wui::Tr("panel.material.shader.compile.debounce",
				"Compiling… (waiting for the 350 ms debounce)");
		}
		else if (unsettled && m_ShaderCompileFailed)
		{
			status = Wui::Tr("panel.material.shader.compile.checking",
				"Checking… (errors appear when you pause typing)");
		}
		else if (!m_ShaderCompileStatus.empty())
		{
			status = m_ShaderCompileStatus;
		}
		if (m_ShaderDiskChangedNotice)
		{
			if (!status.empty())
				status += "   |   ";
			status += Wui::Tr("panel.material.shader.disk.changed",
				"The .slang changed on disk while this buffer had unsaved edits — nothing was "
				"overwritten (Revert to load the file).");
		}
		return status;
	}


bool MaterialEditorPanel::ShaderDiagnosticsUnsettled() const{
		const double now = ShaderWallClockSeconds();
		if (m_ShaderLastEditTime > 0.0 && (now - m_ShaderLastEditTime) < kShaderDiagnosticsSettleSeconds)
			return true;
		// 括号没闭合 = 明显写一半:即使停手也不急着把错误当定稿(避免"刚打完 { 就报错")。
		int braces = 0;
		int parens = 0;
		int brackets = 0;
		bool inString = false;
		bool inLineComment = false;
		bool inBlockComment = false;
		const std::string& text = m_ShaderBuffer.Text();
		for (size_t index = 0; index < text.size(); ++index)
		{
			const char c = text[index];
			const char next = index + 1 < text.size() ? text[index + 1] : '\0';
			if (inLineComment)
			{
				if (c == '\n')
					inLineComment = false;
				continue;
			}
			if (inBlockComment)
			{
				if (c == '*' && next == '/')
				{
					inBlockComment = false;
					++index;
				}
				continue;
			}
			if (inString)
			{
				if (c == '\\')
				{
					++index;
					continue;
				}
				if (c == '"')
					inString = false;
				continue;
			}
			if (c == '/' && next == '/')
			{
				inLineComment = true;
				++index;
				continue;
			}
			if (c == '/' && next == '*')
			{
				inBlockComment = true;
				++index;
				continue;
			}
			if (c == '"')
			{
				inString = true;
				continue;
			}
			if (c == '{')
				++braces;
			else if (c == '}')
				--braces;
			else if (c == '(')
				++parens;
			else if (c == ')')
				--parens;
			else if (c == '[')
				++brackets;
			else if (c == ']')
				--brackets;
		}
		return braces != 0 || parens != 0 || brackets != 0 || inString || inBlockComment;
	}


void MaterialEditorPanel::PollShaderDiskChange(double now){
		if (!m_ShaderMode || m_ShaderPath.empty())
			return;
		if (now - m_ShaderDiskPollTime < kShaderDiskPollSeconds)
			return;
		m_ShaderDiskPollTime = now;
		const std::filesystem::path diskPath = ContentRootPath() / m_ShaderPath;
		std::error_code statError;
		const std::filesystem::file_time_type writeTime =
			std::filesystem::last_write_time(diskPath, statError);
		if (statError)
			return;
		const uintmax_t size = std::filesystem::file_size(diskPath, statError);
		if (statError)
			return;
		if (m_ShaderDiskStampValid && writeTime == m_ShaderDiskWriteTime && size == m_ShaderDiskSize)
			return;
		// 指纹变了(可能是别的窗口保存 / 外部编辑器改写):读内容再比对,只有真的变了才算。
		std::ifstream input(diskPath, std::ios::binary);
		if (!input.is_open())
			return;
		std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (text == m_ShaderDiskText)
		{
			// 只是时间戳/大小变了(内容相同):更新指纹,不当成外部改动。
			m_ShaderDiskWriteTime = writeTime;
			m_ShaderDiskSize = size;
			m_ShaderDiskStampValid = true;
			return;
		}
		if (m_ShaderBuffer.Dirty())
		{
			// 缓冲有未保存改动 → 只提示,**绝不**覆盖用户的输入。
			if (!m_ShaderDiskChangedNotice)
				WLD_CORE_INFO("[material-ui] shader '{0}' changed on disk; unsaved edits kept", m_ShaderPath);
			m_ShaderDiskChangedNotice = true;
			m_ShaderDiskWriteTime = writeTime;
			m_ShaderDiskSize = size;
			m_ShaderDiskStampValid = true;
			return;
		}
		// 缓冲未修改 → 重新载入 + 重编译 + Install(路径键)(用户口径:D2 的"已保存版本"路径)。
		m_ShaderBuffer.SetText(text);
		m_ShaderHighlight.Clear();
		RefreshShaderParams();
		m_ShaderDiskText = text;
		RecordShaderDiskStamp(text);
		m_ShaderDiskChangedNotice = false;
		m_ShaderSeenRevision = m_ShaderBuffer.Revision();
		m_ShaderRequestedRevision = ~0ull;
		m_ShaderForceCompile = true;
		m_ShaderStatus = Wui::Tr("panel.material.shader.status.reloaded",
			"Reloaded the changed .slang from disk: ") + m_ShaderPath;
		m_ShaderStatusIsError = false;
		WLD_CORE_INFO("[material-ui] shader '{0}' reloaded from disk (hot reload)", m_ShaderPath);
	}


float MaterialEditorPanel::DrawShaderDiagnostics(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		if (m_ShaderDiagnostics.empty() || rect.H <= 0.0f)
			return 0.0f;
		const Wui::WuiTheme& theme = host.Theme();
		const float rowHeight = 16.0f;
		const int rows = std::min<int>(static_cast<int>(m_ShaderDiagnostics.size()),
			std::min(4, static_cast<int>(rect.H / rowHeight)));
		if (rows <= 0)
			return 0.0f;
		float used = 0.0f;
		for (int index = 0; index < rows; ++index)
		{
			const ShaderDiagnostic& diagnostic = m_ShaderDiagnostics[static_cast<size_t>(index)];
			const Wui::WuiRect rowRect { rect.X, rect.Y + used, std::max(40.0f, rect.W), rowHeight };
			const bool hovered = ctx.IsHovered(rowRect);
			// T2c:只有**用户源**行列号的条目真的会跳行(T2b),所以也只有它们才带"可点击"
			// 的视觉/无障碍 affordance —— 非用户源行(包装模板上下文)不再给手型光标,
			// 也不登记 interactive,避免"看起来能点、点了不动"。
			const bool canJump = diagnostic.InUserSource && diagnostic.Line > 0;
			Wui::HoverRow(ctx, rowRect, hovered, false, theme, 2.0f);
			const bool isError = diagnostic.Severity != "warning";
			const std::string text = FormatShaderDiagnostic(diagnostic);
			// Slang-B1:前 5 类诊断给"这是什么 + 怎么修"(悬停可见 + 同一条进无障碍 tooltip)。
			const std::string help = ShaderDiagnosticHelp(diagnostic);
			Wui::Label(ctx, { rowRect.X + 4.0f, rowRect.Y + 1.0f },
				EllipsizeToWidth(ctx, text, std::max(40.0f, rowRect.W - 8.0f), 11.0f),
				isError ? theme.Danger : theme.TextMuted, 11.0f);
			if (hovered && canJump)
				ctx.SetCursor(Wui::WuiCursor::Hand);
			if (hovered && !help.empty())
				Wui::Tooltip(ctx, rowRect, text + "\n" + help);
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(("material.shader.diagnostic." + std::to_string(index)).c_str());
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "button";
				node.Label = Wui::Tr("panel.material.shader.diagnostic.label", "Diagnostic ")
					+ std::to_string(index + 1);
				node.Value = text;
				// 不可跳行的行不要在 tooltip 里承诺"点击跳行"(affordance 与真实行为一致)。
				std::string tooltip = canJump
					? text + "\n" + Wui::Tr("panel.material.shader.diagnostic.hint",
						"Click to move the caret to this line in the code column.")
					: text;
				if (!help.empty())
					tooltip += "\n" + help;
				node.Tooltip = tooltip;
				node.Rect = rowRect;
				node.Enabled = true;
				node.Interactive = canJump;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			// T2b:只有**用户源**行列号的条目可点击跳行 —— 包装模板上下文行(非用户源的
			// `In file included from …surface_wrapper.slang:386`)的 Line 是模板行号,
			// 点它会跳到用户缓冲区的无关位置。
			if (hovered && ctx.IsClicked(rowRect) && canJump)
			{
				// 跳行:光标落到该行行首(错误行由 options.ErrorLine 标红;控件自己跟随光标滚动)。
				const std::pair<size_t, size_t> range = m_ShaderBuffer.LineRange(diagnostic.Line - 1);
				m_ShaderBuffer.SetCaret(range.first);
				ctx.SetFocus(Wui::HashId("material.shader.code"));
				m_ShaderStatus = Wui::Tr("panel.material.shader.diagnostic.jumped", "Caret moved to line ")
					+ std::to_string(diagnostic.Line)
					+ Wui::Tr("panel.material.shader.diagnostic.jumped.unit", "");
				m_ShaderStatusIsError = false;
			}
			used += rowHeight;
		}
		if (static_cast<int>(m_ShaderDiagnostics.size()) > rows && rect.H - used >= 14.0f)
		{
			const std::string more = Wui::Tr("panel.material.shader.diagnostic.more", "+")
				+ std::to_string(m_ShaderDiagnostics.size() - static_cast<size_t>(rows))
				+ Wui::Tr("panel.material.shader.diagnostic.more_suffix", " more diagnostics");
			Wui::Label(ctx, { rect.X + 4.0f, rect.Y + used + 1.0f }, more, theme.TextMuted, 11.0f);
			used += 14.0f;
		}
		return used;
	}


void MaterialEditorPanel::RefreshShaderParams(){
		m_ShaderParams.clear();
		m_ShaderParseError.clear();
		m_ShaderErrorLine = 0;
		std::vector<MaterialParamDecl> parsed;
		std::string error;
		if (!ParseMaterialParams(m_ShaderBuffer.Text(), &parsed, &error))
		{
			m_ShaderParseError = error.empty()
				? Wui::Tr("panel.material.shader.parse_error.fallback",
					"the parameter annotations could not be parsed") : error;
			// 内核的错误格式固定为 `<行>:<列>: <原因>`(1 基)—— 行号直接进代码编辑器的红标。
			const size_t colon = m_ShaderParseError.find(':');
			if (colon != std::string::npos && colon > 0)
			{
				const std::string lineText = m_ShaderParseError.substr(0, colon);
				char* end = nullptr;
				const long value = std::strtol(lineText.c_str(), &end, 10);
				if (end != nullptr && *end == '\0' && value > 0)
					m_ShaderErrorLine = static_cast<int>(value);
			}
			return;
		}
		m_ShaderParams = std::move(parsed);
		// 文本编辑缓冲跟随新表重建(参数名集合可能变了)。
		m_ShaderParamTextBuffers.clear();
		m_TextureChoiceNote.clear();     // M4-TEX-P11:换文档时上一份文档的导入反馈一起清
		// Slang-S7 修复(关键):预览替身材质也要像 `.wmat` 一样**引用这份 shader**,
		// 否则它没有注解表 → 参数(尤其贴图)解析不出默认值 → 引擎绑白色 1×1 →
		// 法线贴图退化成 (1,1,1) → 光照≈0 → 代码形态预览**全黑**。
		// 渲染侧的管线选择仍只认 `SetSurfaceKeyOverride` 的键(未保存编辑只进预览)。
		// MAT-FN3:材质函数库不是材质,预览替身材质**不引用**它(否则渲染侧会把它当成
		// 材质着色器去装配管线);库文件下预览只是引擎默认表面。
		if (m_Material && !m_ShaderPath.empty() && !m_ShaderIsLibrary)
		{
			m_Material->SetShaderPath(m_ShaderPath);
			MaterialLibrary::Get().RefreshParams(*m_Material);
		}
		ApplyShaderDefaultsToPreview();
	}


void MaterialEditorPanel::ApplyShaderDefaultsToPreview(){
		if (!m_Material || m_ShaderParams.empty())
			return;
		// 名字映射(约定,写在报告与工具提示里):S3 才会用**编译后的管线**渲染预览,
		// 本批让最常见的几个名字反映进替身材质,拖参数时能立刻看到预览变化。
		MaterialDesc desc = m_Material->GetDesc();
		for (const MaterialParamDecl& decl : m_ShaderParams)
		{
			const std::string key = ToLowerAscii(decl.Name);
			if (decl.Type == ParamType::Color
				&& (key == "basecolor" || key == "base_color" || key == "albedocolor"))
			{
				float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				if (ParseParamFloatComponents(decl.Default, ParamType::Color, rgba, 4))
					desc.BaseColor = glm::vec4 { rgba[0], rgba[1], rgba[2], rgba[3] };
			}
			else if (decl.Type == ParamType::Float && key == "metallic")
			{
				float value = 0.0f;
				if (ParseParamFloat(decl.Default, &value))
					desc.Metallic = value;
			}
			else if (decl.Type == ParamType::Float && key == "roughness")
			{
				float value = 0.0f;
				if (ParseParamFloat(decl.Default, &value))
					desc.Roughness = value;
			}
			else if (decl.Type == ParamType::Vec3 && (key == "emissive" || key == "emission"))
			{
				float rgb[3] = { 0.0f, 0.0f, 0.0f };
				if (ParseParamFloatComponents(decl.Default, ParamType::Vec3, rgb, 3))
					desc.Emissive = glm::vec3 { rgb[0], rgb[1], rgb[2] };
			}
			else if (decl.Type == ParamType::Texture2D && key == "albedo")
				desc.AlbedoTexture = decl.Default;
			else if (decl.Type == ParamType::Texture2D && key == "normal")
				desc.NormalTexture = decl.Default;
			else if (decl.Type == ParamType::Bool && key == "doublesided")
			{
				bool value = false;
				if (ParseParamBool(decl.Default, &value))
					desc.DoubleSided = value;
			}
		}
		m_Material->SetDesc(desc);
		// Slang-S7 修复:上面的"名字白名单"只覆盖历史约定名(basecolor/albedo/normal/…),
		// 一旦用户用别的名字(如 albedoMap/normalMap/tint)就全不命中 → 替身材质贴图槽为空 →
		// 引擎绑白色 1×1 → 法线贴图退化 → 预览全黑。
		// 正确语义与 `.wmat` 无覆盖时一致:**注解默认值就是这份材质的生效值**。
		// 因此这里把全部注解默认值显式写进预览材质的参数表(surface 管线的参数块/贴图槽按它绑定)。
		for (const MaterialParamDecl& decl : m_ShaderParams)
			m_Material->SetParamOverride(decl.Name, decl.Default);
	}


	// MAT-UI45:拖动中的值只进**预览替身材质**的参数覆盖(SetParamOverride 在值真的变了时才
	// BumpRevision → 渲染侧按新 Revision 重打包参数块 → 预览逐帧跟随),不改源码文本。
	// 正式值仍在松手那一帧写进注解(一次撤销步);写回失败时调用方会把覆盖撤回。
void MaterialEditorPanel::ApplyShaderParamLivePreview(const MaterialParamDecl& decl, const std::string& valueText){
		if (!m_Material || valueText.empty())
			return;
		// 值方言与注解默认值同一份(控件返回的就是这个方言);这里只做"必须可归一"的前置校验,
		// 坏值(半截输入 / 非法分量)不进预览。类型判定交给 Material::SetParamOverride。
		std::string normalized = valueText;
		if (!NormalizeParamValue(decl.Type, normalized, &normalized, nullptr))
			return;
		m_Material->SetParamOverride(decl.Name, normalized);
	}


bool MaterialEditorPanel::WriteShaderParamDefault(MaterialParamDecl decl, const std::string& valueText){
		// 1) 先按内核方言规范化(颜色补齐 4 个分量、浮点最短往返),失败就不改文件。
		// MAT-UI3b(用户 2026-09-25「图片选取后无法选回无」):贴图路径必须**带引号**写回注解。
		// NormalizeParamValue(Texture2D) 的输出是**去引号**的路径(空 = 空串),旧代码把它的结果
		// 直接写回去,于是"选回 (无)"会写成 `= group(...)`(默认值整体消失)→ 回读校验不通过 →
		// 回滚 + "无法把默认值写回注解",表现就是选了图之后再也清不掉。这里补引号后写。
		std::string normalized = valueText;
		std::string error;
		if (decl.Type == ParamType::Texture2D)
		{
			std::string path;
			if (!NormalizeParamValue(ParamType::Texture2D, normalized, &path, &error))
			{
				m_ShaderStatus = Wui::Tr("panel.material.shader.status.value_invalid",
					"Value rejected: ") + (error.empty() ? valueText : error);
				m_ShaderStatusIsError = true;
				return false;
			}
			// 空路径 = 引擎绑白色 1×1(与 `.wmat` 的空贴图字段同一语义),注解里写成 `""`。
			normalized = "\"" + path + "\"";
		}
		else if (!NormalizeParamValue(decl.Type, normalized, &normalized, &error))
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.value_invalid",
				"Value rejected: ") + (error.empty() ? valueText : error);
			m_ShaderStatusIsError = true;
			return false;
		}
		// 2) 在源码里定位这条注解(`//! param <type> <name> = …`),只替换默认值那一段 ——
		//    范围、单位、分组、显示名原样保留(它们是用户在文件里写的排版)。
		const std::string text = m_ShaderBuffer.Text();
		std::vector<std::pair<size_t, size_t>> lines;   // [start,end) 不含换行
		{
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				lines.push_back({ start, end });
				if (end == text.size())
					break;
				start = end + 1;
			}
		}
		for (const std::pair<size_t, size_t>& line : lines)
		{
			const std::string lineText = text.substr(line.first, line.second - line.first);
			const size_t marker = lineText.find("//!");
			if (marker == std::string::npos)
				continue;
			std::string_view view(lineText);
			view.remove_prefix(marker + 3);
			// 逐 token:param <type> <name> =
			auto skipSpaces = [](std::string_view& v)
			{
				while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r'))
					v.remove_prefix(1);
			};
			auto takeToken = [](std::string_view& v)
			{
				size_t length = 0;
				while (length < v.size() && v[length] != ' ' && v[length] != '\t' && v[length] != '\r')
					++length;
				std::string token(v.substr(0, length));
				v.remove_prefix(length);
				return token;
			};
			skipSpaces(view);
			if (takeToken(view) != "param")
				continue;
			skipSpaces(view);
			(void)takeToken(view);   // <type>
			skipSpaces(view);
			if (takeToken(view) != decl.Name)
				continue;
			skipSpaces(view);
			if (view.empty() || view.front() != '=')
				continue;
			view.remove_prefix(1);
			skipSpaces(view);
			// 默认值一直取到 `[` / `unit(` / `group(` / `label(` / `doc(` 或行尾(行尾可能带 '\r')。
			// MAT-FN2(收尾 MAT-UI7a §3 点名的遗留):终点表必须与写出侧
			// (`FormatMaterialParamAnnotation`)认得**同一组字段** —— 旧表漏了 `doc(`,于是
			// `= 0.4 doc("…")`(doc 排在第一位、无范围/无 unit/group/label)在面板改默认值时
			// 会被整段当成默认值替换掉:说明静默消失,而且回读校验还是通过。
			// 行尾注释 ` //`(空白后紧跟)同理:它不是默认值的一部分。
			const size_t valueOffsetInView = lineText.size() - (marker + 3) - view.size();
			size_t valueLength = 0;
			bool inQuotes = false;   // 引号内的 `//` 是贴图路径(如 "textures//Icon.png"),不是注释
			while (valueLength < view.size())
			{
				const char c = view[valueLength];
				if (c == '"')
					inQuotes = !inQuotes;
				if (c == '[' || c == '\r' || c == '\n')
					break;
				// 只看行内切片:紧跟 value 的 ` unit(` / ` group(` / ` label(` / ` doc(` 是下一段。
				if (c == ' ' && !inQuotes)
				{
					const std::string_view rest = view.substr(valueLength + 1);
					if (rest.rfind("unit(", 0) == 0 || rest.rfind("group(", 0) == 0
						|| rest.rfind("label(", 0) == 0 || rest.rfind("doc(", 0) == 0
						|| rest.rfind("//", 0) == 0)
						break;
				}
				++valueLength;
			}
			// 去掉尾部空白(不是默认值的一部分)。
			while (valueLength > 0 && (view[valueLength - 1] == ' ' || view[valueLength - 1] == '\t'))
				--valueLength;
			// valueOffsetInView 是"从 `//!` 之后到默认值起点"的消费长度,所以绝对位置要加上
			// 行内 `//!` 的位置与它自己的 3 个字符。
			const size_t replaceStart = line.first + marker + 3 + valueOffsetInView;
			const size_t replaceLength = valueLength;
			const std::string updated = text.substr(0, replaceStart) + normalized
				+ text.substr(replaceStart + replaceLength);
			const std::string before = text;
			m_ShaderBuffer.ReplaceAll(updated);   // 整篇替换 = 一次撤销步
			// 3) 解析回读:新默认值必须被注解解析器读成我们写的那份(否则回滚,不留下坏文件)。
			std::vector<MaterialParamDecl> check;
			std::string checkError;
			const MaterialParamDecl* found = nullptr;
			if (ParseMaterialParams(m_ShaderBuffer.Text(), &check, &checkError))
				found = FindParamDecl(check, decl.Name);
			bool valueMatches = false;
			if (found != nullptr)
			{
				valueMatches = found->Default == normalized;
				if (!valueMatches)
				{
					// 解析器可能对值做了等价规范化(贴图去引号、颜色补 alpha):按规范化结果比一次。
					std::string written;
					std::string readBack;
					if (NormalizeParamValue(decl.Type, normalized, &written, nullptr)
						&& NormalizeParamValue(decl.Type, found->Default, &readBack, nullptr))
						valueMatches = written == readBack;
				}
			}
			if (!valueMatches)
			{
				m_ShaderBuffer.ReplaceAll(before);
				m_ShaderStatus = Wui::Tr("panel.material.shader.status.write_back_failed",
					"Could not write the default back into the annotation")
					+ (checkError.empty() ? std::string() : (": " + checkError));
				m_ShaderStatusIsError = true;
				return false;
			}
			RefreshShaderParams();
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.default_changed",
				"Default updated in the annotation: ") + decl.Name + " = " + normalized
				+ Wui::Tr("panel.material.shader.status.default_changed.hint",
					" (press Save / Ctrl+S to write the file)");
			m_ShaderStatusIsError = false;
			return true;
		}
		m_ShaderStatus = Wui::Tr("panel.material.shader.status.annotation_not_found",
			"Cannot find the annotation line for parameter '") + decl.Name
			+ Wui::Tr("panel.material.shader.status.annotation_not_found.hint",
				"' — edit the file text directly, then save.");
		m_ShaderStatusIsError = true;
		return false;
	}


void MaterialEditorPanel::SaveShaderDocument(){
		if (m_ShaderPath.empty())
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.no_path", "No shader path");
			m_ShaderStatusIsError = true;
			return;
		}
		const std::filesystem::path target = ContentRootPath() / m_ShaderPath;
		std::error_code dirError;
		if (!target.parent_path().empty())
			std::filesystem::create_directories(target.parent_path(), dirError);
		// 临时文件 + 同目录原子替换(与脚本编辑器/材质保存同一口径:失败保留内存内容)。
		const std::filesystem::path temporary = target.parent_path()
			/ (target.filename().string() + ".tmp-" + std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output.is_open())
			{
				m_ShaderStatus = Wui::Tr("panel.material.shader.error.temp_create",
					"Cannot create temporary file: ") + temporary.string();
				m_ShaderStatusIsError = true;
				return;
			}
			const std::string& source = m_ShaderBuffer.Text();
			output.write(source.data(), static_cast<std::streamsize>(source.size()));
			output.flush();
			if (!output.good())
			{
				output.close();
				std::error_code cleanupError;
				std::filesystem::remove(temporary, cleanupError);
				m_ShaderStatus = Wui::Tr("panel.material.shader.error.temp_write",
					"Failed to write the temporary file (editor content preserved)");
				m_ShaderStatusIsError = true;
				return;
			}
		}
		std::string replaceError;
		bool replaced = false;
#ifdef _WIN32
		replaced = MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
		if (!replaced)
			replaceError = Wui::Tr("panel.material.shader.error.replace_failed",
				"Save failed (the file may be in use): ") + m_ShaderPath
				+ " (Win32 " + std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
#else
		std::error_code renameError;
		std::filesystem::rename(temporary, target, renameError);
		replaced = !renameError;
		if (!replaced)
			replaceError = renameError.message();
#endif
		if (!replaced)
		{
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			m_ShaderStatus = replaceError;
			m_ShaderStatusIsError = true;
			return;
		}
		m_ShaderBuffer.MarkSaved();
		RefreshShaderParams();
		// M4-S3(D2):保存成功 = 写盘 + Install("<路径>", artifact)—— 这时场景才换管线。
		m_ShaderDiskText = m_ShaderBuffer.Text();
		RecordShaderDiskStamp(m_ShaderDiskText);
		m_ShaderDiskChangedNotice = false;
		bool promoted = false;
		bool publishFailed = false;
		std::string publishNote;
		if (!m_ShaderLastArtifactSource.empty() && m_ShaderLastArtifactSource == m_ShaderBuffer.Text())
		{
			const std::string key = ShaderPathKey();
			const MaterialSurfaceRuntime::InstallResult install =
				MaterialSurfaceRuntime::Install(key, m_ShaderLastArtifact);
			if (install.Success)
			{
				// 预览材质从 `<路径>#preview` 指回普通键:场景与预览从此共用同一份已发布管线。
				if (m_Material)
					m_Material->SetSurfaceKeyOverride(key);
				m_ShaderInstalledKey = key;
				m_ShaderInstalledVersion = MaterialSurfaceRuntime::PublishedVersion(key);
				promoted = true;
				WLD_CORE_INFO("[material-ui] shader saved + promoted to key '{0}' v{1}",
					key, m_ShaderInstalledVersion);
			}
			else
			{
				publishFailed = true;
				publishNote = Wui::Tr("panel.material.shader.status.saved_not_published",
					"Saved, but the pipeline was not published: ")
					+ (install.Error.empty()
						? Wui::Tr("panel.material.shader.compile.unknown_reason", "unknown reason")
						: install.Error);
			}
		}
		if (promoted)
		{
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.saved_live", "Saved and live: ")
				+ m_ShaderPath;
			m_ShaderStatusIsError = false;
		}
		else
		{
			// 带错保存 / 编译还没回来:照常写盘(用户意图),但不 Install ——
			// 场景继续用上一份可用管线(D6:不做静默降级,状态行说明)。
			// MAT-FN3:库文件不编译,状态行不能写"编译中…"(它永远不会编译成管线)。
			const std::string reason = m_ShaderIsLibrary
				? Wui::Tr("panel.material.shader.library.status_saved",
					"material function library — not compiled or baked on its own; materials pick it up "
					"through #include")
				: (publishFailed
					? publishNote
					: (m_ShaderCompileFailed
						? Wui::Tr("panel.material.shader.status.saved_errors",
							"the shader has errors — the scene keeps the last good pipeline")
						: Wui::Tr("panel.material.shader.status.saved_compiling",
							"compiling — the scene keeps the last good pipeline until it succeeds")));
			m_ShaderStatus = Wui::Tr("panel.material.shader.status.saved", "Saved ") + m_ShaderPath
				+ " (" + reason + ")";
			// 带错保存本身不算操作失败(文件确实写下去了);只有发布失败才是错误色。
			m_ShaderStatusIsError = publishFailed;
		}
		WLD_CORE_INFO("[material-ui] saved shader '{0}' ({1} bytes)", m_ShaderPath, m_ShaderBuffer.Text().size());
	}


	// MAT-INTEL:轻量格式化 —— 只动空白/缩进(规则表见 SlangFormat.h),语义不变。
	// 一次 ReplaceAll = 一步撤销历史,所以 Ctrl+Z 回到格式化前的原文。
	// 状态行复用脚本编辑器已有的两条键(语言包不在本单边界内;新键会被 audit-localization 判缺失)。
void MaterialEditorPanel::ApplyShaderFormat(){
		const std::size_t beforeBytes = m_ShaderBuffer.Text().size();
		const std::string formatted = FormatSlangSource(m_ShaderBuffer.Text());
		if (m_ShaderBuffer.ReplaceAll(formatted))
		{
			m_ShaderStatus = Wui::Tr("panel.script.status.formatted",
				"Formatted (4-space indent + trailing whitespace removed)");
			m_ShaderStatusIsError = false;
			WLD_CORE_INFO("[material-ui] formatted shader '{0}' ({1} -> {2} bytes)",
				m_ShaderPath, beforeBytes, m_ShaderBuffer.Text().size());
		}
		else
		{
			m_ShaderStatus = Wui::Tr("panel.script.status.format_unchanged", "No formatting changes");
			m_ShaderStatusIsError = false;
		}
	}


bool MaterialEditorPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt){
		(void)alt;
		if (!m_ShaderMode)
			return false;   // `.wmat` 形态不抢键(既有接线不变)
		if (!ctrl)
			return false;
		if (keyCode == KeyCodes::S)
		{
			m_PendingShaderSave = true;
			return true;
		}
		if (keyCode == KeyCodes::R && !shift)
		{
			m_PendingShaderRevert = true;
			return true;
		}
		if (keyCode == KeyCodes::F && shift)
		{
			// MAT-INTEL:Ctrl+Shift+F = 轻量格式化(与脚本编辑器同一键位;Ctrl+S/Ctrl+R 已占用,
			// CodeEditor 自身只消费 Ctrl+S/A/C/X/V/Z/Y,不冲突)。
			m_PendingShaderFormat = true;
			return true;
		}
		return false;
	}

}
