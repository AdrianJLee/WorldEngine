#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- M4-S2:代码形态的三列布局(预览 | 代码 | 参数)----
float MaterialEditorPanel::DrawShaderDocument(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		// 绘制之外只置位的请求在**帧内开头**消费(与脚本编辑器同一条纪律:不在事件派发期改文档)。
		if (m_PendingShaderRevert)
		{
			m_PendingShaderRevert = false;
			LoadShaderFromDisk();
		}
		if (m_PendingShaderSave)
		{
			m_PendingShaderSave = false;
			SaveShaderDocument();
		}
		// MAT-INTEL:注入路径(ui.key)不进 EditorLayer 的快捷键路由(那条路由只吃 GLFW 事件),
		// 所以代码列持焦时在这里补看一次 Ctrl+Shift+F —— 与 WuiCodeEditor 自己消费 Ctrl+S 是同一条
		// "帧内看输入"的口径;真实按键两条路径都置位同一个 flag,幂等。
		if (!m_PendingShaderFormat && ctx.Focus() == Wui::HashId("material.shader.code")
			&& ctx.Input().Ctrl && ctx.Input().Shift && ctx.WasKeyTriggered(KeyCodes::F))
			m_PendingShaderFormat = true;
		if (m_PendingShaderFormat)
		{
			m_PendingShaderFormat = false;
			ApplyShaderFormat();
		}
		if (m_ShaderCompileScheduled)
		{
			m_ShaderCompileScheduled = false;
			// 手动 Compile = 跳过消抖的立即编译(M4-S3:实际编译仍在工作线程,不阻塞 UI 帧)。
			m_ShaderForceCompile = true;
		}
		// M4-S3:防抖 → 后台编译 → 结果回主线程 Install;顺带轮询磁盘(着色器热重载)。
		const double now = ShaderWallClockSeconds();
		PumpShaderCompile(now);
		PollShaderDiskChange(now);
		const float headerHeight = DrawShaderHeader(ctx, { rect.X, rect.Y, rect.W, kHeaderBaseHeight }, host);
		const float pad = theme.Pad;
		const float gap = theme.PadSmall;
		const Wui::WuiRect body { rect.X, rect.Y + headerHeight, rect.W,
			std::max(80.0f, rect.H - headerHeight) };
		if (body.W >= kShaderThreeColumnMinWidth)
		{
			// 三列 + 两条可拖拽分隔条(与 U27 的预览/参数分隔条同一控件、同一口径)。
			const float usable = body.W - 2.0f * pad - 2.0f * gap;
			float previewW = SessionPreviewColumnWidth() > 0.0f
				? SessionPreviewColumnWidth() : body.W * 0.28f;
			previewW = std::clamp(previewW, kShaderPreviewMinWidth,
				std::max(kShaderPreviewMinWidth, usable - kShaderCodeMinWidth - kShaderParamsMinWidth));
			float codeW = m_ShaderCodeColumnWidth > 0.0f
				? m_ShaderCodeColumnWidth : usable * kShaderDefaultCodeRatio;
			codeW = std::clamp(codeW, kShaderCodeMinWidth,
				std::max(kShaderCodeMinWidth, usable - previewW - kShaderParamsMinWidth));
			const float paramsW = std::max(kShaderParamsMinWidth, usable - previewW - codeW);

			const float rowY = body.Y + gap;
			const float rowH = std::max(80.0f, body.H - gap - pad);
			const float previewX = body.X + pad;
			const float previewAxis = previewX + previewW + gap * 0.5f;
			const float codeX = previewX + previewW + gap;
			const float codeAxis = codeX + codeW + gap * 0.5f;
			const float paramsX = codeX + codeW + gap;
			// 双击复位:与 .wmat 形态同口径(复位必须在控件调用之前,拖拽锚点才是默认值)。
			const Wui::WuiId previewSplitId = Wui::HashId("material.shader.splitter.preview");
			const Wui::WuiId codeSplitId = Wui::HashId("material.shader.splitter.code");
			const Wui::WuiRect previewBand { previewAxis - 3.0f, rowY, 6.0f, rowH };
			const Wui::WuiRect codeBand { codeAxis - 3.0f, rowY, 6.0f, rowH };
			NoteFocusOwningRect(previewBand);   // MAT-UI3b:分隔条按下会拿焦点,重复点不清状态
			NoteFocusOwningRect(codeBand);
			const float defaultPreviewW = std::clamp(body.W * 0.28f, kShaderPreviewMinWidth,
				std::max(kShaderPreviewMinWidth, usable - kShaderCodeMinWidth - kShaderParamsMinWidth));
			const float defaultCodeW = std::clamp(usable * kShaderDefaultCodeRatio, kShaderCodeMinWidth,
				std::max(kShaderCodeMinWidth, usable - defaultPreviewW - kShaderParamsMinWidth));
			if (ctx.IsDoubleClicked(previewBand))
			{
				previewW = defaultPreviewW;
				SessionPreviewColumnWidth() = previewW;
			}
			if (ctx.IsDoubleClicked(codeBand))
			{
				codeW = defaultCodeW;
				m_ShaderCodeColumnWidth = codeW;
			}
			float movedValue = previewW;
			if (Wui::Splitter(ctx, previewSplitId, { previewAxis, rowY, 1.0f, rowH }, true, movedValue,
				kShaderPreviewMinWidth, std::max(kShaderPreviewMinWidth,
					usable - kShaderCodeMinWidth - kShaderParamsMinWidth), theme))
			{
				previewW = movedValue;
				SessionPreviewColumnWidth() = previewW;
			}
			movedValue = codeW;
			if (Wui::Splitter(ctx, codeSplitId, { codeAxis, rowY, 1.0f, rowH }, true, movedValue,
				kShaderCodeMinWidth, std::max(kShaderCodeMinWidth,
					usable - previewW - kShaderParamsMinWidth), theme))
			{
				codeW = movedValue;
				m_ShaderCodeColumnWidth = codeW;
			}
			Wui::Tooltip(ctx, previewBand, Wui::Tr("panel.material.shader.splitter.preview.tooltip",
				"Drag to resize the preview; double-click to restore the default split."));
			Wui::Tooltip(ctx, codeBand, Wui::Tr("panel.material.shader.splitter.code.tooltip",
				"Drag to resize the code column; double-click to restore the default split."));

			DrawPreview(ctx, { previewX, rowY, previewW, rowH }, host);
			DrawShaderCode(ctx, { codeX, rowY, codeW, rowH }, host);
			DrawShaderParams(ctx, { paramsX, rowY, std::max(kShaderParamsMinWidth, paramsW), rowH }, host);
		}
		else
		{
			// 窄窗单列:预览(上) → 代码(中) → 参数(下),三段不重叠(与其他面板的窄窗规则一致)。
			const float width = std::max(80.0f, body.W - 2.0f * pad);
			const float cardMax = std::max(kPreviewImageMinSide + 2.0f * gap, body.H * 0.34f);
			const Wui::WuiRect previewZone { body.X + pad, body.Y + gap, width, cardMax };
			const float previewUsed = DrawPreview(ctx, previewZone, host);
			// 代码与参数各占剩余空间的一半;参数列保底 kParamsMinHeight(它比代码更能被压缩)。
			const float remaining = std::max(200.0f, body.H - gap - previewUsed - 2.0f * pad);
			const float codeHeight = std::max(100.0f, remaining - kParamsMinHeight - pad);
			DrawShaderCode(ctx, { body.X + pad, previewZone.Y + previewUsed + pad, width, codeHeight }, host);
			DrawShaderParams(ctx, { body.X + pad, previewZone.Y + previewUsed + pad + codeHeight + pad,
				width, std::max(kParamsMinHeight, remaining - codeHeight - pad) }, host);
		}
		return body.H;
	}


	// ---- M4-S2:代码形态的头部(名称 + 路径 + 脏标记 + 动作)----
float MaterialEditorPanel::DrawShaderHeader(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		const float gap = theme.PadSmall;
		const float x = rect.X;
		const float y = rect.Y + gap;
		const float buttonH = theme.ControlHeight;
		const bool dirty = m_ShaderBuffer.Dirty();
		const bool readOnly = host.IsReadOnlyMode();

		struct ShaderAction
		{
			const char* Id = "";
			std::string Label;
			std::string Doc;
			bool Enabled = true;
			bool Primary = false;
		};
		std::vector<ShaderAction> actions {
			{ "material.shader.save", Wui::Tr("panel.material.shader.save", "Save"),
				Wui::Tr("panel.material.shader.save.tooltip",
					"Save (Ctrl+S): write the source back to the .slang on disk (temporary file + atomic "
					"replace). The parameter annotations in the file are the single source of truth. "
					"Saving also publishes the compiled pipeline for the scene: unsaved edits only "
					"change this panel's preview."),
				!readOnly, true },
			{ "material.shader.revert", Wui::Tr("panel.material.shader.revert", "Revert"),
				Wui::Tr("panel.material.shader.revert.tooltip",
					"Revert (Ctrl+R): drop unsaved edits and read the .slang from disk again."),
				true, false },
			// MAT-UI2:按钮语义写清楚 —— 平时是"跳过消抖立即编译 / 失败后重试",自动编译照常。
			// MAT-UI3b:标签/说明进目录(新增键落在 panels/material_editor.json)。
			// MAT-FN3:材质函数库没有 `Evaluate`,不单独编译 —— 按钮禁用,悬停说明为什么。
			{ "material.shader.compile", Wui::Tr("panel.material.shader.compile_now", "Compile now"),
				m_ShaderIsLibrary
					? Wui::Tr("panel.material.shader.compile_now.tooltip.library",
						"Material function libraries are not compiled on their own: there is no Evaluate "
						"entry, so only the materials that include the library are compiled and baked.")
					: Wui::Tr("panel.material.shader.compile_now.tooltip",
						"Compile now (skip the 350 ms debounce). Live edits are compiled automatically on a "
						"worker thread; use this button to compile immediately, or to retry after a failure. "
						"The preview follows the newest successful compile; the scene only switches after Save."),
				!m_ShaderIsLibrary, false },
			// MAT-INTEL:格式化 —— MAT-UI3b 起标签/说明也走目录。
			{ "material.shader.format", Wui::Tr("panel.material.shader.format", "Format"),
				Wui::Tr("panel.material.shader.format.tooltip",
					"Format (Ctrl+Shift+F): whitespace only — trailing whitespace removed, tabs → 4 spaces, "
					"braces drive a 4-space indent, runs of 3+ blank lines collapse to one, one trailing "
					"newline, `//!` gets a space. Identifiers and expressions are untouched. The formatted "
					"text goes into the edit buffer (Ctrl+Z undoes it); Save writes it back to disk."),
				!readOnly, false },
			{ "material.shader.reveal", Wui::Tr("panel.material.shader.reveal", "Reveal"),
				Wui::Tr("panel.material.shader.reveal.tooltip",
					"Reveal: select the .slang in Windows Explorer."),
				true, false },
		};
		float actionX = x;
		float actionY = y;
		float actionsHeight = buttonH;
		for (const ShaderAction& action : actions)
		{
			const float measured = ctx.MeasureTextWidth(action.Label, 13.0f) + 18.0f;
			const float width = std::min(std::max(kActionMinWidth, measured), std::max(24.0f, rect.W - 2.0f));
			if (actionX > x && actionX + width > x + rect.W - 2.0f)
			{
				actionX = x;
				actionY += buttonH + 6.0f;
			}
			const Wui::WuiRect actionRect { actionX, actionY, width, buttonH };
			const std::string actionId = action.Id;
			if (ActionButton(ctx, Wui::HashId(action.Id), actionRect, action.Label, action.Doc,
				action.Enabled, action.Primary, theme))
			{
				if (actionId == "material.shader.save")
					SaveShaderDocument();
				else if (actionId == "material.shader.revert")
					LoadShaderFromDisk();
				else if (actionId == "material.shader.compile")
					m_ShaderCompileScheduled = true;   // 在绘制之外执行(帧内首段消费)
				else if (actionId == "material.shader.format")
					ApplyShaderFormat();
				else if (actionId == "material.shader.reveal")
				{
					const std::filesystem::path disk = ContentRootPath() / m_ShaderPath;
					std::error_code existsError;
					if (!std::filesystem::exists(disk, existsError))
					{
						m_ShaderStatus = Wui::Tr("panel.material.shader.status.reveal_missing",
							"Reveal failed: the .slang is not on disk yet (save it first)");
						m_ShaderStatusIsError = true;
					}
#ifdef _WIN32
					else
					{
						// 与 .wmat 的 Reveal / 内容浏览器"Show in Explorer"同一条系统调用。
						const std::wstring parameters = L"/select,\""
							+ std::filesystem::absolute(disk).wstring() + L"\"";
						const HINSTANCE revealResult = ShellExecuteW(nullptr, L"open", L"explorer.exe",
							parameters.c_str(), nullptr, SW_SHOWNORMAL);
						if (reinterpret_cast<intptr_t>(revealResult) <= 32)
						{
							m_ShaderStatus = Wui::Tr("panel.material.shader.status.reveal_failed",
								"Reveal failed");
							m_ShaderStatusIsError = true;
						}
						else
						{
							m_ShaderStatus = Wui::Tr("panel.material.shader.status.revealed",
								"Revealed in Explorer: ") + m_ShaderPath;
							m_ShaderStatusIsError = false;
						}
					}
#else
					else
					{
						m_ShaderStatus = Wui::Tr("panel.material.shader.status.reveal_unsupported",
							"Reveal is only implemented on Windows");
						m_ShaderStatusIsError = true;
					}
#endif
				}
			}
			actionsHeight = (actionY - y) + buttonH;
			actionX += actionRect.W + 6.0f;
		}

		// 脏标记(与 .wmat 同一语言:* = 内存与磁盘不一致)。
		const std::string marker = dirty ? "*" : "";
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.dirty");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.dirty.label", "Unsaved changes");
			node.Value = dirty ? "dirty" : "clean";
			node.Tooltip = Wui::Tr("panel.material.shader.dirty.tooltip",
				"* = the editor buffer differs from the .slang on disk. Unsaved edits are compiled and "
				"shown in this panel's preview only; the scene switches after Save.");
			node.Rect = { x, y + actionsHeight + 4.0f, 24.0f, 14.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// 标题行:名称(带脏标记)+ 逻辑路径。
		const float titleY = y + actionsHeight + 6.0f;
		const std::string title = std::filesystem::path(m_ShaderPath).filename().string() + marker;
		Wui::Label(ctx, { x + 14.0f, titleY }, title, theme.Text, kHeaderTextHeight);
		Wui::Label(ctx, { x, titleY + kHeaderTextHeight + 2.0f },
			EllipsizeToWidth(ctx, m_ShaderPath, std::max(40.0f, rect.W - 8.0f), kHeaderStatusHeight),
			theme.TextMuted, kHeaderStatusHeight);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.title");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.title", "Material Shader");
			node.Value = m_ShaderPath + (dirty
				? Wui::Tr("panel.material.shader.unsaved", " (unsaved)")
				: Wui::Tr("panel.material.shader.saved", " (saved)"));
			node.Tooltip = Wui::Tr("panel.material.shader.title.tooltip",
				"Logical path of this .slang (relative to the content root). The material editor opens "
				"shaders in code form: preview | code | declared parameters.");
			node.Rect = { x, titleY - 2.0f, std::max(40.0f, rect.W - 8.0f),
				kHeaderTextHeight + kHeaderStatusHeight + 6.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const float headerHeight = (titleY - rect.Y) + kHeaderTextHeight + kHeaderStatusHeight + 2.0f;
		return headerHeight;
	}


	// MAT-INTEL2(探针钩子):把代码列**真正用到**的逐行 token 落盘 —— 白色比例与关键字覆盖率的
	// 量化证据来源(解析器见 tools/agents/scratch/mat-intel2-white-ratio.py)。格式:
	//   FILE <逻辑路径>
	//   LINE <行号> <字节数> <start>-<end>:<kind> …
	// 触发 = `WLD_SLANG_TOKEN_DUMP=<绝对路径>`;`WLD_SLANG_TOKEN_DUMP_SHADER=<逻辑路径后缀>` 可把
	// dump 限定在某个着色器上(同一个进程里每个着色器只写一次)。不设环境变量时本函数不会被调用。
void MaterialEditorPanel::DumpShaderTokens(const char* path){
		const char* only = std::getenv("WLD_SLANG_TOKEN_DUMP_SHADER");
		if (only != nullptr && *only != '\0')
		{
			const std::size_t suffix = std::strlen(only);
			if (m_ShaderPath.size() < suffix
				|| m_ShaderPath.compare(m_ShaderPath.size() - suffix, suffix, only) != 0)
				return;
		}
		static std::vector<std::string> written;
		for (const std::string& done : written)
			if (done == m_ShaderPath)
				return;
		std::ofstream out(path, std::ios::binary);
		if (!out)
		{
			WLD_CORE_WARN("[matintel2] token dump 写不进去: {0}", path);
			return;
		}
		written.push_back(m_ShaderPath);
		out << "FILE " << m_ShaderPath << "\n";
		SlangHighlightSymbols symbols;
		symbols.FileNames = &m_ShaderCompletion.DeclaredNames();
		SlangHighlightState state;
		std::vector<Wui::WuiCodeToken> tokens;
		for (int line = 0; line < m_ShaderBuffer.LineCount(); ++line)
		{
			const std::pair<size_t, size_t> range = m_ShaderBuffer.LineRange(line);
			const std::string_view text(m_ShaderBuffer.Text().data() + range.first,
				range.second - range.first);
			if (SlangAnnotations::IsAnnotationLine(text))
				SlangAnnotations::HighlightLineWithAnnotations(text, state, tokens, &symbols);
			else
				SlangHighlighter::HighlightLine(text, state, tokens, &symbols);
			out << "LINE " << line << ' ' << text.size();
			for (const Wui::WuiCodeToken& token : tokens)
			{
				out << ' ' << token.StartByte << '-' << token.EndByte << ':'
					<< SlangTokens::KindName(token.Kind);
			}
			out << "\n";
		}
		WLD_CORE_INFO("[matintel2] token dump written: {0} ({1} lines, shader '{2}')",
			path, m_ShaderBuffer.LineCount(), m_ShaderPath);
	}

namespace MaterialEditorPanelDetail
{
std::string FormatShaderSwatchHex(const glm::vec4& rgba){
			const auto channel = [](float value)
			{
				return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
			};
			char buffer[16] = {};
			if (rgba.a >= 0.999f)
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
					channel(rgba.r), channel(rgba.g), channel(rgba.b));
			else
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
					channel(rgba.r), channel(rgba.g), channel(rgba.b), channel(rgba.a));
			return buffer;
		}

}

	// MAT-UI45:按缓冲区版本扫一遍 `//! param Color <name> = r, g, b[, a]`。
	// 只收解析成功的行(类型 = Color、4 个分量都能解析;缺 alpha 按 1)—— 非法值不进表,不画色块。
void MaterialEditorPanel::RefreshShaderColorSwatches(){
		const uint64_t revision = m_ShaderBuffer.Revision();
		if (m_ShaderColorSwatchRevision == revision)
			return;
		m_ShaderColorSwatchRevision = revision;
		m_ShaderColorSwatches.clear();
		const std::string& text = m_ShaderBuffer.Text();
		for (int line = 0; line < m_ShaderBuffer.LineCount(); ++line)
		{
			const std::pair<size_t, size_t> range = m_ShaderBuffer.LineRange(line);
			const std::string_view lineView(text.data() + range.first, range.second - range.first);
			SlangAnnotations::ParamDecl decl;
			if (!SlangAnnotations::ParseParamDecl(lineView, decl))
				continue;
			ParamType type = ParamType::Float;
			if (!ParseParamTypeName(decl.Type, &type) || type != ParamType::Color)
				continue;
			float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			if (!ParseParamFloatComponents(decl.Default, ParamType::Color, rgba, 4))
				continue;
			ShaderColorSwatch swatch;
			swatch.Line = line;
			swatch.Name = decl.Name;
			swatch.Rgba = glm::vec4 { rgba[0], rgba[1], rgba[2], rgba[3] };
			swatch.Hex = FormatShaderSwatchHex(swatch.Rgba);
			m_ShaderColorSwatches.push_back(std::move(swatch));
		}
	}


	// MAT-UI45:行尾 12×12 圆角色块(棋盘底 + 颜色覆盖 + 1px 描边)+ 只读 a11y 节点。
	// 位置:首选紧跟本行文本右端(textEndX + 6);注解行通常带 group()/label() 而代码列不宽,
	// 所以放不下时**贴文本带右缘**(避免色块被裁掉 = 功能在窄列里彻底看不见;代价是极长行会
	// 盖住右端 ~12px 的正文,拖宽代码列即可看到全文)。绘制全走库件(不引入裸绘制)。
void MaterialEditorPanel::DrawShaderColorSwatch(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, const ShaderColorSwatch& swatch, const Wui::WuiRect& lineRect, float textEndX){
		constexpr float kSwatchSize = 12.0f;      // 派工口径:12×12
		constexpr float kSwatchGap = 6.0f;        // 文本右端 → 色块左缘
		constexpr float kSwatchRadius = 3.0f;
		constexpr float kCheckerCell = 4.0f;      // 与取色器折叠态色块同一档(4px)
		const float maxX = lineRect.X + lineRect.W - 2.0f - kSwatchSize;
		const float x = std::min(textEndX + kSwatchGap, maxX);
		if (x < lineRect.X)
			return;   // 文本带比色块还窄(极端布局):不画,不越界
		const Wui::WuiRect rect { x, lineRect.Y + std::max(0.0f, (lineRect.H - kSwatchSize) * 0.5f),
			kSwatchSize, kSwatchSize };
		// 棋盘底:底色 + 交替格(与 ColorField 的折叠态色块同一视觉口径)。
		Wui::PanelBackground(ctx, rect, theme.ButtonBg, kSwatchRadius);
		const int cells = static_cast<int>(std::ceil(kSwatchSize / kCheckerCell));
		for (int cellX = 0; cellX < cells; ++cellX)
			for (int cellY = 0; cellY < cells; ++cellY)
			{
				if (((cellX + cellY) & 1) == 0)
					continue;
				const Wui::WuiRect cell { rect.X + kCheckerCell * static_cast<float>(cellX),
					rect.Y + kCheckerCell * static_cast<float>(cellY),
					std::min(kCheckerCell, rect.X + rect.W - (rect.X + kCheckerCell * cellX)),
					std::min(kCheckerCell, rect.Y + rect.H - (rect.Y + kCheckerCell * cellY)) };
				if (cell.W <= 0.0f || cell.H <= 0.0f)
					continue;
				Wui::PanelBackground(ctx, cell, theme.ButtonHover, 0.0f);
			}
		Wui::PanelBackground(ctx, rect, Wui::WuiColor { std::clamp(swatch.Rgba.r, 0.0f, 1.0f),
			std::clamp(swatch.Rgba.g, 0.0f, 1.0f), std::clamp(swatch.Rgba.b, 0.0f, 1.0f),
			std::clamp(swatch.Rgba.a, 0.0f, 1.0f) }, kSwatchRadius);
		Wui::HighlightOutline(ctx, rect, theme.Border, kSwatchRadius, 1.0f);
		// 只读 a11y 节点:脚本/读屏按 kind="color-swatch" 直接读色块矩形与规范色值。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId(("material.shader.swatch." + swatch.Name).c_str());
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "color-swatch";
		node.Label = swatch.Name;
		node.Value = swatch.Hex;
		node.Tooltip = Wui::Tr("panel.material.shader.swatch.tooltip",
			"Color of the //! param Color annotation on this line: ") + swatch.Hex;
		node.Rect = rect;
		node.Enabled = true;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
	}


	// MAT-INTEL3:补全索引 = "按缓冲区版本刷新"。除了代码列帧首,**补全 / 悬停回调进入前也刷一次**:
	// `WuiCodeEditor` 在同一帧里先插入文本再查候选,只用帧首版本会让"这一帧刚敲出来的那一行"还没进索引
	// —— 光标行落在函数体区间之外,局部变量/形参被误判成不可见(实测:文末新起一行时必现)。
void MaterialEditorPanel::EnsureShaderIndex(){
		if (m_ShaderCompletionRevision == m_ShaderBuffer.Revision())
			return;
		m_ShaderCompletionRevision = m_ShaderBuffer.Revision();
		m_ShaderCompletion.SetFileSource(m_ShaderBuffer.Text());
	}


void MaterialEditorPanel::DrawShaderCode(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		Wui::PanelBackground(ctx, rect, theme.ContentBg, theme.Radius);
		// M4-S3:诊断列表住在代码列底部(每条可点击跳行);没有诊断时一点位置都不占。
		const float wantedStrip = m_ShaderDiagnostics.empty() ? 0.0f
			: (static_cast<float>(std::min<int>(static_cast<int>(m_ShaderDiagnostics.size()), 4)) * 16.0f
				+ (m_ShaderDiagnostics.size() > 4 ? 14.0f : 0.0f));
		const float stripHeight = std::min(wantedStrip, std::max(0.0f, rect.H - 12.0f - 40.0f));
		const Wui::WuiRect editorRect { rect.X + 6.0f, rect.Y + 6.0f,
			std::max(40.0f, rect.W - 12.0f), std::max(40.0f, rect.H - 12.0f - stripHeight) };
		const bool readOnly = host.IsReadOnlyMode();
		const float fontSize = std::max(10.0f, std::min(32.0f,
			Editor::EditorPreferences::Get().Data().ScriptFontSize));
		Wui::WuiAccessNode editorNode;
		editorNode.Id = Wui::HashId("material.shader.code");
		editorNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		editorNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		editorNode.Kind = "editor";
		editorNode.Label = Wui::Tr("panel.material.shader.code", "shader code");
		editorNode.Value = m_ShaderPath;
		editorNode.Rect = editorRect;
		editorNode.Enabled = !readOnly;
		editorNode.Interactive = true;
		Wui::WuiAccessibility::Get().Register(editorNode);
		NoteFocusOwningRect(editorRect);   // MAT-UI3b:代码列自己接手焦点

		// MAT-INTEL:补全/Hover 的文档表 —— 本文件声明的 `//! param` 名字进候选(Revision 变化才重扫);
		// MAT-INTEL3 起同一份索引也带语义索引(形参 / 局部变量 / 文件级声明)。
		EnsureShaderIndex();
		// MAT-INTEL2:高亮与补全读同一份词表(SlangKeywords.h)+ 同一份参数名扫描 —— 缓存 Update
		// 放在索引刷新之后,改注解声明的那一帧颜色就是对的。
		m_ShaderHighlight.Update(m_ShaderBuffer, m_ShaderCompletion.DeclaredNames());
		// MAT-INTEL2 探针钩子(只读;不设 WLD_SLANG_TOKEN_DUMP 时这条分支不执行)。
		if (const char* tokenDump = std::getenv("WLD_SLANG_TOKEN_DUMP"))
			DumpShaderTokens(tokenDump);
		Wui::WuiCodeEditorOptions options;
		// MAT-UI6b:会话缩放初值(内核只在实例第一次出现时读它;之后以内核状态为准)。
		// 材质代码列与脚本编辑器**各一份**,互不影响;任何缩放路径都不写偏好文件。
		options.UiZoom = m_ShaderZoom;
		options.FontSize = fontSize;
		options.LineHeight = std::round(fontSize * (20.0f / 14.0f));
		// MAT-UI2:诊断没"定稿"时不给代码列标红(错误仍在状态行/诊断条里,等停手再出现)。
		const bool diagnosticsUnsettled = ShaderDiagnosticsUnsettled();
		options.ErrorLine = (!diagnosticsUnsettled && m_ShaderErrorLine > 0) ? m_ShaderErrorLine - 1 : -1;
		options.ReadOnly = readOnly;
		options.Highlight = [this](std::string_view text, std::vector<Wui::WuiCodeToken>& out)
		{
			// 注解行与代码行同一条路径:`//!` 行的注解体按 Slang 语法着色(整行注释色会让
			// WuiCodeEditor "注释里不弹补全"的闸门挡住注解体),缓存里已经这么算。
			if (const std::vector<Wui::WuiCodeToken>* cached = m_ShaderHighlight.Find(text))
			{
				out = *cached;
				return;
			}
			// 本帧改过文本(缓冲区重分配)→ 重建缓存后重试;仍未命中就现场兜底。
			m_ShaderHighlight.Update(m_ShaderBuffer, m_ShaderCompletion.DeclaredNames());
			if (const std::vector<Wui::WuiCodeToken>* refreshed = m_ShaderHighlight.Find(text))
			{
				out = *refreshed;
				return;
			}
			SlangHighlightSymbols symbols;
			symbols.FileNames = &m_ShaderCompletion.DeclaredNames();
			SlangHighlightState state;
			if (SlangAnnotations::IsAnnotationLine(text))
				SlangAnnotations::HighlightLineWithAnnotations(text, state, out, &symbols);
			else
				SlangHighlighter::HighlightLine(text, state, out, &symbols);
		};
		// MAT-INTEL:补全(成员 / 文件参数+引擎函数+类型+内建+关键字 / `//!` 注解)+ 悬停同一份文档。
		// MAT-INTEL3:补全查询带上**光标所在行** —— 局部变量 / 形参按"同一函数体内 + 声明在光标之前"
		// 过滤(口径见 SlangCompletion.h 的 SlangSemantics);高亮仍只按名字集合着色。
		options.CompletionIdPrefix = "material.suggest";
		options.Completion = [this](std::string_view linePrefix,
			std::vector<World::LuauCompletionItem>& out)
		{
			EnsureShaderIndex();   // 同帧新敲的文本必须在索引里(见本函数上方注释)
			const int caretLine = m_ShaderBuffer.LineOfOffset(m_ShaderBuffer.Caret());
			m_ShaderCompletion.QueryAt(linePrefix, caretLine, 50, out);
		};
		options.Hover = [this](std::string_view linePrefix, std::string_view word,
			World::LuauCompletionItem& out)
		{
			EnsureShaderIndex();
			return m_ShaderCompletion.Describe(linePrefix, word, out);
		};
		options.GetClipboard = [](std::string& out)
		{
			if (!Application::HasInstance())
				return false;
			out = Application::Get().GetWindow().GetClipboardText();
			return !out.empty();
		};
		options.SetClipboard = [](std::string_view text)
		{
			if (!Application::HasInstance())
				return false;
			Application::Get().GetWindow().SetClipboardText(std::string(text));
			return true;
		};
		// MAT-UI45:代码列行尾 Color 色块 —— `//! param Color …` 解析成功才画(非法值不画),
		// 位置由引擎的逐行几何给出(含滚动/裁剪),浮层(补全/Hover)天然盖在色块之上。
		RefreshShaderColorSwatches();
		options.LineDecorator = [this, &theme](Wui::WuiContext& decoratorCtx, int line,
			const Wui::WuiRect& lineRect, float textEndX)
		{
			for (const ShaderColorSwatch& swatch : m_ShaderColorSwatches)
				if (swatch.Line == line)
				{
					DrawShaderColorSwatch(decoratorCtx, theme, swatch, lineRect, textEndX);
					break;
				}
		};
		const Wui::WuiCodeEditorResult result =
			Wui::CodeEditor(ctx, Wui::HashId("material.shader.code"), editorRect, m_ShaderBuffer, options);
		// P1c-a(72c9ca2)起 WuiCodeEditor 自己也登记一个同 id、kind=code-editor 的本体节点,把上面
		// 那个面板节点顶掉(Register 同 id 以最后一次为准);而 AI 通道 ui.type 的 kind 闸门只认
		// editor / text-field —— 结果:脚本编辑器与材质代码列都再也没法按 id 注入文本(m4s3 探针
		// 的 ui.type 现在会被拒)。在引擎侧改闸门/节点合并策略超出本单边界,这里按面板侧重新登记
		// 自己的节点:kind=editor(可注入)、label/value 与 P1c-a 之前一致,focused 反映真实焦点。
		editorNode.Focused = ctx.Focus() == Wui::HashId("material.shader.code");
		Wui::WuiAccessibility::Get().Register(editorNode);
		if (result.SaveRequested)
			m_PendingShaderSave = true;
		// MAT-UI6b:回读内核会话缩放(内核是唯一事实源;Ctrl+滚轮 / Ctrl+0 都在 WuiCodeEditor 内,
		// 宿主不再有第二条缩放路径)。值一变就刷新宿主会话值;指示画在参数列的状态行上。
		if (std::fabs(result.UiZoom - m_ShaderZoom) > 1e-6f)
			m_ShaderZoom = result.UiZoom;
		if (result.ZoomChanged)
			m_ShaderZoomIndicatorUntil = ShaderWallClockSeconds() + 2.0;
		if (result.Changed)
		{
			// 编辑中的注解文本可能已经不合法:每帧重解析只在"源码里出现过 //! 或错误尚未清除"时做,
			// 避免大文件每帧全量解析。
			// MAT-UI2 修复(用户报"属性报错改正后不消失,保存才消失"):旧代码还有 `m_ShaderParseError.empty()`
			// 前置 —— 一旦注解解析报错就**再也不刷新**,必须保存/重载才清。现在只要有 `//!` 就重解析,
			// 改对了当帧就清掉参数错误、参数表同步更新。
			static const std::string kMarker = "//!";
			if (m_ShaderBuffer.Text().find(kMarker) != std::string::npos)
				RefreshShaderParams();
		}
		if (stripHeight > 0.0f)
		{
			// MAT-UI2:未定稿时只画一行中性提示,不列错误行(打一半不吓人)。
			const Wui::WuiRect strip { editorRect.X, editorRect.Y + editorRect.H + 2.0f,
				editorRect.W, stripHeight };
			if (diagnosticsUnsettled)
				Wui::Label(ctx, { strip.X + 4.0f, strip.Y + 2.0f },
					Wui::Tr("panel.material.shader.diagnostic.settling",
						"Checking… (errors appear when you pause typing)"),
					theme.TextMuted, 11.0f);
			else
				DrawShaderDiagnostics(ctx, strip, host);
		}
	}


	// ---- M4-S2:参数列(注解 = 事实源;改默认值 = 改写注解) ----
	// M4-TEX-P6a:Texture2D 参数的排版 —— 用户口径「控件最小宽度之和 > 列宽时纵向堆叠
	// (label 一行、控件下一行)」的**唯一落点**:标签 + 控件簇(路径框 + 选择 + 清空)并排放不下
	// 就先拆标签;控件簇自己也放不下,再拆成"路径框一行 → 按钮一行/两行"。
float MaterialEditorPanel::ShaderTextureInlineControlWidth(float columnWidth, float labelWidthIn) const{
		const float rowWidth = std::max(80.0f, columnWidth - kGroupIndent - 6.0f);
		return std::max(60.0f, rowWidth - labelWidthIn - 8.0f - (kResetWidth + 6.0f));
	}


float MaterialEditorPanel::ShaderTextureStackedControlWidth(float columnWidth) const{
		const float rowWidth = std::max(80.0f, columnWidth - kGroupIndent - 6.0f);
		return std::max(60.0f, rowWidth - (kResetWidth + 6.0f));
	}


	// 高度在这里算,**绘制端只按这里的判据与结果摆位** —— 两处不会再各算一套(那会让滚动范围错)。
	// M4-TEX-P11:控件换成 `Wui::WuiTexturePicker`(下拉 + 徽标 + 清空 + 定位)后,控件本体**恒定一行**;
	// 放不下就只拆标签(组件内部自己切列表/徽标/定位,不再有"路径框下面排按钮"的 2/3 行形态)。
MaterialEditorPanel::TextureRowLayout MaterialEditorPanel::TextureRowLayoutFor( const Wui::WuiTheme& theme, float inlineControlWidth, float stackedControlWidth, bool withError) const{
		constexpr float baseRowHeight = 26.0f;
		(void)stackedControlWidth;   // 组件自己内部布局:行高不再依赖"控件簇拆成几行"
		TextureRowLayout layout;
		if (inlineControlWidth < kTexturePickerMinWidth)
			layout.StackLabel = true;   // 标签独占一行,控件拿整行宽
		float height = layout.StackLabel
			? kTextureLabelLineHeight + kTextureControlLineHeight + kTextureWarningGap
			: std::max(baseRowHeight, kTextureControlLineHeight);
		// 行内校验说明:控件下一行一句话(绘制端用同一组常量摆位,见 DrawShaderParamControl)。
		if (withError)
			height += kTextureWarningGap + theme.FontSizeCaption + kTextureWarningGap;
		layout.Height = height;
		return layout;
	}


bool MaterialEditorPanel::DrawShaderParamControl(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme, const MaterialParamDecl& decl, const Wui::WuiRect& controlRect, const std::string& current, std::string* outText){
		// 控件 id 是**稳定契约**(派工单点名):material.shader.params.<name>。
		const Wui::WuiId id = Wui::HashId(("material.shader.params." + decl.Name).c_str());
		switch (decl.Type)
		{
			case ParamType::Float:
			{
				float value = decl.Min;
				if (!ParseParamFloat(current, &value))
					value = decl.Min;
				// 值域 = 注解里的 [min,max](缺省 0..1,与内核同口径);值区常显 + 可键入 + ↑↓ 步进。
				Wui::DragBarFloat(ctx, id, controlRect, value, decl.Min, decl.Max, theme);
				const float rounded = std::round(value * 1000.0f) / 1000.0f;
				const std::string text = FormatParamFloatText(rounded);
				if (text != current)
				{
					*outText = text;
					return true;
				}
				return false;
			}
			case ParamType::Int:
			{
				int value = static_cast<int>(decl.Min);
				if (!ParseParamInt(current, &value))
					value = static_cast<int>(decl.Min);
				const int minValue = static_cast<int>(decl.Min);
				const int maxValue = std::max(minValue, static_cast<int>(decl.Max));
				if (maxValue - minValue <= 16)
					Wui::StepperInt(ctx, id, controlRect, value, minValue, maxValue, theme);
				else
				{
					int64_t wide = value;
					Wui::NumberFieldInt(ctx, id, controlRect, wide, minValue, maxValue, theme);
					value = static_cast<int>(wide);
				}
				const std::string text = std::to_string(value);
				if (text != current)
				{
					*outText = text;
					return true;
				}
				return false;
			}
			case ParamType::Bool:
			{
				bool value = false;
				ParseParamBool(current, &value);
				if (Wui::Checkbox(ctx, id, controlRect, decl.Label.empty() ? decl.Name : decl.Label, value, theme))
				{
					*outText = value ? "true" : "false";
					return true;
				}
				return false;
			}
			case ParamType::Color:
			{
				glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f };
				float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				if (ParseParamFloatComponents(current, ParamType::Color, rgba, 4))
					color = glm::vec4 { rgba[0], rgba[1], rgba[2], rgba[3] };
				if (Wui::ColorField(ctx, id, controlRect, color, theme))
				{
					const glm::vec4 quantized = QuantizeMaterialValue(color);
					*outText = FormatParamFloatText(quantized.x) + ", " + FormatParamFloatText(quantized.y)
						+ ", " + FormatParamFloatText(quantized.z) + ", " + FormatParamFloatText(quantized.w);
					return true;
				}
				return false;
			}
			case ParamType::Vec2:
			case ParamType::Vec3:
			case ParamType::Vec4:
			{
				// MAT-UI3b:Vec2/Vec4 改用 we_engine 的同族部件(Vec2Field / Vec4Field,与
				// Vec3Field 同一份 VecFieldCore 实现:小标签 + 数值区 + 拖动/键入/↑↓)。
				// [min,max] 只对 Float/Int 有效(内核口径):向量统一用固定宽容区间。
				const float vectorMin = -8.0f;
				const float vectorMax = 8.0f;
				if (decl.Type == ParamType::Vec2)
				{
					glm::vec2 value { 0.0f };
					float xy[2] = { 0.0f, 0.0f };
					if (ParseParamFloatComponents(current, ParamType::Vec2, xy, 2))
						value = glm::vec2 { xy[0], xy[1] };
					if (Wui::Vec2Field(ctx, id, controlRect, value, 0.01f, vectorMin, vectorMax, theme, 0))
					{
						*outText = FormatParamFloatText(value.x) + ", " + FormatParamFloatText(value.y);
						return true;
					}
					return false;
				}
				if (decl.Type == ParamType::Vec4)
				{
					glm::vec4 value { 0.0f };
					float xyzw[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					if (ParseParamFloatComponents(current, ParamType::Vec4, xyzw, 4))
						value = glm::vec4 { xyzw[0], xyzw[1], xyzw[2], xyzw[3] };
					if (Wui::Vec4Field(ctx, id, controlRect, value, 0.01f, vectorMin, vectorMax, theme, 0))
					{
						*outText = FormatParamFloatText(value.x) + ", " + FormatParamFloatText(value.y)
							+ ", " + FormatParamFloatText(value.z) + ", " + FormatParamFloatText(value.w);
						return true;
					}
					return false;
				}
				if (decl.Type == ParamType::Vec3)
				{
					glm::vec3 value { 0.0f };
					float rgb[3] = { 0.0f, 0.0f, 0.0f };
					if (ParseParamFloatComponents(current, ParamType::Vec3, rgb, 3))
						value = glm::vec3 { rgb[0], rgb[1], rgb[2] };
					if (Wui::Vec3Field(ctx, id, controlRect, value, 0.01f, vectorMin, vectorMax, theme, 0))
					{
						*outText = FormatParamFloatText(value.x) + ", " + FormatParamFloatText(value.y)
							+ ", " + FormatParamFloatText(value.z);
						return true;
					}
					return false;
				}
				// Vec2 / Vec3 / Vec4 三条都在上面返回:这里不可达(留一个显式断言,防将来加类型漏接线)。
				return false;
			}
			case ParamType::Texture2D:
			default:
			{
				// M4-TEX-P11:纹理参数 = `Wui::WuiTexturePicker`(可搜索下拉:资产 / 源图 + 徽标 +
				// 清空 + 定位 + 拖放)。整块矩形给组件(它内部切列表 | 徽标条 | 定位),面板不再画
				// 路径框 / `Pick…` / 清空那三件套(D1 已确认不保留手输路径)。
				// 口径与 `.wmat` 槽位完全一致:同一份候选清单、同一条"选中源图 = 当场导入成容器"
				// 归一、同一条行内校验一句话(缺失 / 资产缺源图不静默通过)。
				const std::string label = decl.Label.empty() ? decl.Name : decl.Label;
				const std::string inlineWarning = Editor::TextureInlineWarning(current);
				const float width = std::max(40.0f, controlRect.W);
				// 跨窗口拖放:与槽位**同一 sink**,key = 参数名(组件用 Options::DroppedValue 应用)。
				RegisterSlotDrop(ctx, host, decl.Name, controlRect);
				std::string dropped;
				const bool consumedDrop = TakeSlotDrop(host, decl.Name, &dropped);
				auto options = Editor::TexturePickerOptions(current, label,
					"material.shader.params." + decl.Name, consumedDrop ? dropped : std::string());
				bool revealRequested = false;
				options.RevealRequested = &revealRequested;
				// 值 = 组件自己的进出参:返回值 = **值有没有变**(变才写材质/注解 —— 每帧写会让
				// Revision 逐帧 +1,渲染侧每帧重建材质描述符集 → 预览逐帧闪)。
				std::string value = current;
				const bool changed = Wui::WuiTexturePicker(ctx, id, controlRect, value, options, theme);
				if (changed)
				{
					// M4-TEX P9:选中的是源图 = 先导入成单文件容器资产(注解/覆盖写资产路径);
					// 导入说明由上层写入路径消费(状态行)。
					std::string importNote;
					const std::string assigned = Editor::NormalizeTextureChoice(value, &importNote);
					m_TextureChoiceNote = importNote;
					*outText = assigned;
					return assigned != current;
				}
				if (revealRequested)
					RevealTextureRef(host, value.empty() ? current : value);
				// 行内校验(资产缺源图 / 文件缺失):画在控件**下面**一行(与高度公式同一组常量)。
				if (!inlineWarning.empty())
				{
					const float warningY = controlRect.Y + kTextureControlLineHeight + kTextureWarningGap;
					const Wui::WuiRect warningRect { controlRect.X, warningY, width,
						theme.FontSizeCaption + 2.0f };
					Wui::Label(ctx, { warningRect.X + 2.0f, warningRect.Y },
						EllipsizeToWidth(ctx, inlineWarning, warningRect.W - 4.0f, theme.FontSizeCaption),
						theme.Warning, theme.FontSizeCaption);
					Wui::WuiAccessNode node;
					node.Id = Wui::HashId(("material.shader.params." + decl.Name + ".warning").c_str());
					node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					node.Kind = "text";
					node.Label = label + Wui::Tr("panel.material.texture.warn.label", " — texture warning");
					node.Value = inlineWarning;
					node.Tooltip = Editor::TextureRefDoc(current);
					node.Rect = warningRect;
					node.Enabled = true;
					node.Interactive = false;
					node.Visible = true;
					Wui::WuiAccessibility::Get().Register(node);
				}
				return false;
			}
		}
	}


float MaterialEditorPanel::DrawShaderParams(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		const Wui::WuiTheme& theme = host.Theme();
		float y = rect.Y;
		// 顶部:标题 + 参数条数(读屏/脚本由此确认"右栏就是注解参数表")。
		// MAT-FN3:`shaders/lib/**` 的库文件不是材质:标题换成"材质函数库",不列
		// `//! param` 参数表(库文件不应有参数,参数写在使用它的材质里),只给一条统一说明。
		const std::string title = m_ShaderIsLibrary
			? Wui::Tr("panel.material.shader.library.title", "Material Function Library")
			: Wui::Tr("panel.material.shader.params", "Shader Parameters");
		Wui::Label(ctx, { rect.X, y + 2.0f }, title, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.params.header");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "group";
			node.Label = title;
			if (m_ShaderIsLibrary)
			{
				node.Value = Wui::Tr("panel.material.shader.library.params_hidden",
					"library file — //! param lines are not shown here");
				node.Tooltip = Wui::Tr("panel.material.shader.library.tooltip",
					"A material function library has no Evaluate entry, is never compiled or baked on its "
					"own, and is used by materials through #include. Its functions are pure: resources "
					"(Sampler2D) and values come from the material as parameters.");
			}
			else
			{
				node.Value = std::to_string(m_ShaderParams.size())
					+ Wui::Tr("panel.material.shader.params.declared", " declared parameters");
				node.Tooltip = Wui::Tr("panel.material.shader.params.tooltip",
					"Parameters declared by the //! param annotations in this file. Editing a value here "
					"rewrites the annotation default (the file stays the single source of truth).");
			}
			node.Rect = { rect.X, y, std::max(40.0f, rect.W), 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		y += 22.0f;
		if (!m_ShaderParseError.empty() && !m_ShaderIsLibrary)
		{
			Wui::Label(ctx, { rect.X, y }, Wui::Tr("panel.material.shader.parse_error", "Annotation error: ")
				+ EllipsizeToWidth(ctx, m_ShaderParseError, std::max(40.0f, rect.W), 12.0f), theme.Danger, 12.0f);
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.parse_error");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.parse_error.label", "Parameter annotation error");
			node.Value = m_ShaderParseError;
			node.Tooltip = Wui::Tr("panel.material.shader.parse_error.tooltip",
				"The //! param annotations could not be parsed; the readable reason carries line:column. "
				"Fix the text in the code column — nothing is written to disk until you save.");
			node.Rect = { rect.X, y - 2.0f, std::max(40.0f, rect.W), 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			y += 20.0f;
		}
		// 提示:行数 = 声明数(探针按它断言"右栏条数与 ParseMaterialParams 一致")。
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.params.count");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.params.count.label", "Declared parameters");
			node.Value = m_ShaderIsLibrary
				? Wui::Tr("panel.material.shader.library.params_hidden",
					"library file — //! param lines are not shown here")
				: std::to_string(m_ShaderParams.size());
			node.Tooltip = title;
			node.Rect = { rect.X, y, std::max(40.0f, rect.W), 14.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		y += 18.0f;

		// MAT-FN3:库文件的统一说明(库文件没有参数表可列)。逐词折行 —— 整句要看得见,
		// 窄列下不能被截断;同一句话也进无障碍节点(读屏/脚本的断言锚点)。
		if (m_ShaderIsLibrary)
		{
			const std::string notice = Wui::Tr("panel.material.shader.library.notice",
				"Material function library: no Evaluate entry — it is not baked on its own; materials "
				"use it through #include. You can still edit and save it here.");
			const float noticeWidth = std::max(40.0f, rect.W - 4.0f);
			const float noticeTop = y;
			std::vector<std::string> lines;
			std::string line;
			size_t index = 0;
			while (index < notice.size())
			{
				size_t end = notice.find(' ', index);
				if (end == std::string::npos)
					end = notice.size();
				const std::string word = notice.substr(index, end - index);
				index = end + 1;
				if (word.empty())
					continue;
				const std::string candidate = line.empty() ? word : line + " " + word;
				if (!line.empty() && ctx.MeasureTextWidth(candidate, 12.0f) > noticeWidth)
				{
					lines.push_back(line);
					line = word;
				}
				else
				{
					line = candidate;
				}
			}
			if (!line.empty())
				lines.push_back(line);
			for (const std::string& noticeLine : lines)
			{
				Wui::Label(ctx, { rect.X, y }, noticeLine, theme.TextMuted, 12.0f);
				y += 15.0f;
			}
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.library.notice");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = title;
			node.Value = notice;
			node.Tooltip = Wui::Tr("panel.material.shader.library.tooltip",
				"A material function library has no Evaluate entry, is never compiled or baked on its "
				"own, and is used by materials through #include. Its functions are pure: resources "
				"(Sampler2D) and values come from the material as parameters.");
			node.Rect = { rect.X, noticeTop - 2.0f, noticeWidth, std::max(18.0f, y - noticeTop) };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			y += 6.0f;
		}

		// M4-S3:底部现在是两行 —— 编译状态(字节数 + 耗时 + 键 / 第一条错误的行列号)在上一行。
		const Wui::WuiRect content { rect.X, y, rect.W, std::max(20.0f, rect.Y + rect.H - y - 38.0f) };
		// MAT-FN3:库文件不显示 `//! param` 面板区(库文件不应有参数 —— 参数写在使用它的材质里)。
		// 参数表为空表,下面的分组/行/空态提示因此都不出现。
		static const std::vector<MaterialParamDecl> kNoShaderParams;
		const std::vector<MaterialParamDecl>& paramRows =
			m_ShaderIsLibrary ? kNoShaderParams : m_ShaderParams;
		// 内容高度 ≈ 组头 20 + 每行 26(与下面绘制一致;折叠组只多留一点余量,不影响可读性)。
		const std::string defaultGroupLabel =
			Wui::Tr("panel.material.shader.params.group.default", "Parameters");
		std::vector<std::string> groups;
		for (const MaterialParamDecl& decl : paramRows)
		{
			const std::string group = decl.Group.empty() ? defaultGroupLabel : decl.Group;
			if (std::find(groups.begin(), groups.end(), group) == groups.end())
				groups.push_back(group);
		}
		const float contentHeight = static_cast<float>(groups.size()) * 20.0f
			+ [&]
			{
				float rows = 0.0f;
				for (const MaterialParamDecl& decl : paramRows)
				{
					if (decl.Type == ParamType::Texture2D)
					{
						const float labelWidthIn = std::min(140.0f, std::max(70.0f, content.W * 0.34f));
						const bool withError = !Editor::TextureInlineWarning(decl.Default).empty();
						rows += TextureRowLayoutFor(theme,
							std::max(60.0f, content.W - labelWidthIn - 16.0f),
							std::max(40.0f, content.W), withError).Height;
						continue;
					}
					rows += ShaderParamRowHeight(decl.Type, 26.0f);
				}
				return rows;
			}() + 4.0f;
		// 滚动位置复用 m_ScrollY(材质字段列与代码形态不会同屏出现)。
		Wui::BeginScrollArea(ctx, content, contentHeight, m_ScrollY, theme);
		// 简化:参数不多(注解表通常几条到十几条),不做虚拟化;直接按分组顺序画。
		float cursor = content.Y + 2.0f - m_ScrollY;
		std::string currentGroup;
		bool groupOpen = true;
		for (const MaterialParamDecl& decl : paramRows)
		{
			const std::string group = decl.Group.empty()
				? defaultGroupLabel : decl.Group;
			if (group != currentGroup)
			{
				currentGroup = group;
				auto found = m_ShaderGroupOpen.find(group);
				if (found == m_ShaderGroupOpen.end())
					found = m_ShaderGroupOpen.emplace(group, true).first;
				groupOpen = found->second;
				const Wui::WuiRect headerRect { content.X, cursor, std::max(40.0f, content.W), 20.0f };
				const bool hovered = ctx.IsHovered(headerRect);
				Wui::HoverRow(ctx, headerRect, hovered, false, theme, 4.0f);
				Wui::Label(ctx, { headerRect.X + 6.0f, cursor + 3.0f }, (groupOpen ? "v " : "> ") + group,
					theme.TextMuted, 12.0f);
				if (hovered)
					ctx.SetCursor(Wui::WuiCursor::Hand);
				if (ctx.IsClicked(headerRect))
					found->second = !found->second;
				Wui::Tooltip(ctx, headerRect, Wui::Tr("panel.material.shader.params.group.tooltip",
					"Group name written in the annotation: group(\"…\"). Click to expand or collapse."));
				cursor += 20.0f;
				if (!groupOpen)
					continue;
			}
			if (!groupOpen)
				continue;
			// 一行 = 标签 + 控件(+ 单位/范围/类型说明)。
			// M4-TEX-P6a:纹理参数走"标签/控件最小宽之和 > 列宽 → 纵向堆叠"的排版(唯一落点
			// = TextureRowLayoutFor);其它类型仍是既有的单行口径。
			const std::string label = decl.Label.empty() ? decl.Name : decl.Label;
			const float labelWidthIn = std::min(140.0f, std::max(70.0f, content.W * 0.34f));
			const std::string rowRawValue = decl.Default;
			const std::string rowWarning = decl.Type == ParamType::Texture2D
				? Editor::TextureInlineWarning(rowRawValue) : std::string();
			const TextureRowLayout layout = decl.Type == ParamType::Texture2D
				? TextureRowLayoutFor(theme, std::max(60.0f, content.W - labelWidthIn - 16.0f),
					std::max(40.0f, content.W), !rowWarning.empty())
				: TextureRowLayout { false, ShaderParamRowHeight(decl.Type, 26.0f) };
			const float rowHeight = layout.Height;
			const Wui::WuiRect rowRect { content.X, cursor, std::max(40.0f, content.W), rowHeight };
			const float labelWidth = layout.StackLabel ? std::max(40.0f, content.W - 8.0f) : labelWidthIn;
			const float controlX = layout.StackLabel ? content.X : content.X + labelWidth + 8.0f;
			// 控件高度:纹理参数簇固定 22(路径框/按钮);其它类型沿用"行高 − 4"(Vec4 的 2×2 需要 48)。
			const float controlHeight = decl.Type == ParamType::Texture2D
				? 22.0f : std::max(22.0f, rowHeight - 4.0f);
			const Wui::WuiRect controlRect { controlX, cursor + (layout.StackLabel ? 18.0f : 0.0f),
				layout.StackLabel ? std::max(40.0f, content.W)
					: std::max(60.0f, content.W - labelWidth - 16.0f), controlHeight };
			NoteFocusOwningRect(controlRect);   // MAT-UI3b:注解参数行自己接手焦点
			Wui::Label(ctx, { content.X, layout.StackLabel ? cursor : cursor + 4.0f },
				EllipsizeToWidth(ctx, label, labelWidth, 12.0f), theme.Text, 12.0f);
			// MAT-UI45(用户复报「颜色板按住拖拽,颜色不会变」):拖动期间**以控件自己的值为准** ——
			// 把待提交值回灌给控件,而不是每帧把注解里的旧文本再喂回去。旧行为下控件内部的 HSV
			// 编辑态每帧被旧文本顶回去:取色器标记/折叠态色块/预览都停在旧色,只有松手才跳变。
			// 鼠标按住 = 拖动中(键盘输入与点击都是单帧提交,仍走常规路径,语义不变)。
			std::string rowValue = decl.Default;
			if (ctx.Input().MouseDown[0] && m_ShaderPendingParamName == decl.Name
				&& !m_ShaderPendingParamValue.empty())
				rowValue = m_ShaderPendingParamValue;
			std::string valueText = rowValue;
			if (DrawShaderParamControl(ctx, host, theme, decl, controlRect, rowValue, &valueText))
			{
				if (m_ShaderPendingParamName != decl.Name || m_ShaderPendingParamValue != valueText)
				{
					m_ShaderPendingParamName = decl.Name;
					m_ShaderPendingParamValue = valueText;
				}
				// 拖动期间把值实时推给预览替身材质(只写内存覆盖,不动源码文本 → 不塞撤销历史、
				// 不触发每帧重解析/重编译);松手那一帧仍由下面的 WriteShaderParamDefault 落注解。
				if (ctx.Input().MouseDown[0])
					ApplyShaderParamLivePreview(decl, valueText);
			}
			// 悬停说明:注解 doc("…") 说明(有则第一行,MAT-UI7b)+ 类型 / 范围 / 单位 / 当前默认值。
			// (原来的 "在这里编辑会改写 //! param 注解;按保存才写盘" 一句按用户 2026-09-25 的要求整条删除。)
			std::string doc = Wui::Tr("panel.material.shader.param.type", "Type: ")
				+ ParamTypeName(decl.Type);
			if (decl.Type == ParamType::Float || decl.Type == ParamType::Int)
				doc += "\n" + Wui::Tr("panel.material.shader.param.range", "Range: ")
					+ FormatParamFloatText(decl.Min) + " .. " + FormatParamFloatText(decl.Max);
			if (!decl.Unit.empty())
				doc += "\n" + Wui::Tr("panel.material.shader.param.unit", "Unit: ") + decl.Unit;
			doc += "\n" + Wui::Tr("panel.material.shader.param.default", "Default (from the annotation): ")
				+ decl.Default;
			// M4-TEX-P6a:纹理参数补"资产 → 源图"关系(全文路径与缺失原因都在这句里)。
			if (decl.Type == ParamType::Texture2D)
			{
				const std::string textureDoc = Editor::TextureRefDoc(decl.Default);
				if (!textureDoc.empty())
					doc += "\n" + textureDoc;
			}
			const std::string rowDoc = ShaderParamRowTooltip(decl, doc);
			Wui::Tooltip(ctx, rowRect, rowDoc);
			RegisterShaderParamDocNode(decl, label, rowRect);
			if (decl.Type == ParamType::Texture2D && !decl.Default.empty())
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(("material.param." + decl.Name + ".path").c_str());
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "text";
				node.Label = label + Wui::Tr("panel.material.texture.ref.path_label", " — path");
				node.Value = decl.Default;
				node.Tooltip = Editor::TextureRefDoc(decl.Default);
				node.Rect = { content.X, cursor + rowHeight - 2.0f, std::max(20.0f, content.W), 2.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(("material.param." + decl.Name + ".source").c_str());
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "text";
				node.Label = label + Wui::Tr("panel.material.shader.param.source.label", " — source");
				node.Value = "shader-default";
				// 行说明与 `.wmat` 形态同一份(类型 / 范围 / 单位 / 来源),读屏两态一致。
				node.Tooltip = rowDoc;
				node.Rect = { content.X, cursor, 2.0f, rowHeight };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			cursor += rowHeight;
		}
		Wui::EndScrollArea(ctx);
		// M4-TEX-P6a:参数比视口长时给一条**纵向**滚动指示(与 .wmat 参数列同一条画法)——
		// "下面还有参数"必须看得见;行变高(纹理参数堆叠/行内说明)后这条更容易被用到。
		if (contentHeight > content.H + 1.0f)
		{
			const float maxScroll = std::max(0.0f, contentHeight - content.H);
			const float trackHeight = std::max(24.0f, content.H - 8.0f);
			const float thumbHeight = std::max(24.0f, trackHeight * (content.H / contentHeight));
			const float offset = maxScroll > 0.0f ? (m_ScrollY / maxScroll) * (trackHeight - thumbHeight) : 0.0f;
			const Wui::WuiRect track { content.X + content.W - 3.0f, content.Y + 4.0f, 2.0f, trackHeight };
			Wui::PanelBackground(ctx, track, Wui::WuiColor { 0.169f, 0.192f, 0.220f, 1.0f }, 1.0f);
			Wui::PanelBackground(ctx, { track.X, track.Y + offset, 2.0f, thumbHeight },
				Wui::WuiColor { 0.298f, 0.553f, 1.0f, 0.55f }, 1.0f);
		}
		// 拖拽/输入结束那一帧才改写注解:一次拖动 = 一个撤销步(拖动期间每帧都改会把
		// 撤销历史塞满,也会让解析器每帧重跑)。
		if (!m_ShaderPendingParamName.empty()
			&& (!ctx.Input().MouseDown[0] || ctx.Input().MouseReleased[0]))
		{
			const std::string pendingName = m_ShaderPendingParamName;
			std::string pendingValue = m_ShaderPendingParamValue;
			m_ShaderPendingParamName.clear();
			m_ShaderPendingParamValue.clear();
			const MaterialParamDecl* pendingDecl = FindParamDecl(m_ShaderParams, pendingName);
			// M4-TEX P9:Texture2D 参数赋的是**源图**时,先导入成单文件容器资产,注解写资产路径。
			// M4-TEX-P11:下拉选源图时适配层已经归一过,这里再走一遍是兜底(资产路径原样返回),
			// 状态行反馈优先用控件那一帧留下的导入说明。
			if (pendingDecl != nullptr && pendingDecl->Type == ParamType::Texture2D)
			{
				std::string importNote;
				pendingValue = Editor::NormalizeTextureChoice(pendingValue, &importNote);
				if (!importNote.empty())
				{
					m_ShaderStatus = importNote;
					m_ValidationRevision = 0;
				}
				else if (!m_TextureChoiceNote.empty())
				{
					m_ShaderStatus = m_TextureChoiceNote;
					m_ValidationRevision = 0;
				}
			}
			m_TextureChoiceNote.clear();
			const bool written = pendingDecl != nullptr
				&& WriteShaderParamDefault(*pendingDecl, pendingValue);
			// MAT-UI45:注解没落上(找不到行 / 写回回读校验失败)= 拖动期间推给预览的覆盖值必须撤回,
			// 让"文件是唯一事实源"重新成立(预览不许停在文本里没有的颜色上)。
			if (!written)
				ApplyShaderDefaultsToPreview();
		}
		if (paramRows.empty() && m_ShaderParseError.empty() && !m_ShaderIsLibrary)
			Wui::Label(ctx, { content.X, content.Y + 2.0f },
				Wui::Tr("panel.material.shader.params.none",
					"This shader declares no parameters yet — add //! param lines in the code column."),
				theme.TextMuted, 12.0f);
		// 底部状态行:M4-S3 的编译状态(成功 = 字节数 + 耗时 + 键;失败 = 第一条错误含行列号)
		// 单独占一行并给稳定 a11y id(material.shader.compile.status),探针按它读结果。
		const float statusY = rect.Y + rect.H - 18.0f;
		const float compileStatusY = statusY - 14.0f;
		const std::string compileStatus = ShaderCompileStatusLine();
		if (!compileStatus.empty())
		{
			Wui::Label(ctx, { rect.X + 2.0f, compileStatusY },
				EllipsizeToWidth(ctx, compileStatus, std::max(40.0f, rect.W - 4.0f), 11.0f),
				m_ShaderCompileFailed ? theme.Danger : theme.TextMuted, 11.0f);
		}
		{
			// Slang-B1:编译失败时把第一条诊断的"这是什么 + 怎么修"接在 tooltip 后面
			// (状态行本身只放得下一条错误;解释走悬停/读屏,与诊断行同一份文案)。
			const std::string firstHelp = m_ShaderDiagnostics.empty()
				? std::string() : ShaderDiagnosticHelp(m_ShaderDiagnostics.front());
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.compile.status");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.compile.status.label", "Shader compile status");
			node.Value = compileStatus;
			node.Tooltip = firstHelp.empty() ? compileStatus : (compileStatus + "\n" + firstHelp);
			node.Rect = { rect.X, compileStatusY - 2.0f, std::max(40.0f, rect.W - 4.0f), 16.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// MAT-UI6b:状态行尾巴上的会话缩放指示(`Font N%`)—— 缩放 ≠ 100% 时常显,
		// 刚缩放过的 2 秒内即使回到 100% 也留一行(Ctrl+0 复位据此可见/可断言)。
		std::string status = m_ShaderStatus;
		{
			const int zoomPercent = static_cast<int>(std::lround(m_ShaderZoom * 100.0f));
			if (zoomPercent != 100 || ShaderWallClockSeconds() < m_ShaderZoomIndicatorUntil)
				status += "   Font " + std::to_string(zoomPercent) + "%";
		}
		Wui::Label(ctx, { rect.X + 2.0f, statusY },
			EllipsizeToWidth(ctx, status, std::max(40.0f, rect.W - 4.0f), 11.0f),
			m_ShaderStatusIsError ? theme.Danger : theme.TextMuted, 11.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.status");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.status.label", "Status");
			node.Value = status;
			node.Tooltip = status;
			node.Rect = { rect.X, statusY - 2.0f, std::max(40.0f, rect.W - 4.0f), 16.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		if (!m_ShaderDiagnostics.empty())
		{
			// Slang-B1:第一条诊断的"这是什么 + 怎么修"一并进 tooltip —— 状态行是一条 11px 的
			// 单行文本,放不下整段解释;悬停/读屏能拿到完整口径(与诊断行 tooltip 同一份文案)。
			const std::string firstDiagnostic = FormatShaderDiagnostic(m_ShaderDiagnostics.front());
			const std::string firstDiagnosticHelp = ShaderDiagnosticHelp(m_ShaderDiagnostics.front());
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.diagnostics");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.diagnostics", "Compiler diagnostics");
			node.Value = firstDiagnostic;
			node.Tooltip = firstDiagnosticHelp.empty()
				? firstDiagnostic : (firstDiagnostic + "\n" + firstDiagnosticHelp);
			node.Rect = { rect.X, compileStatusY - 16.0f, std::max(40.0f, rect.W - 4.0f), 14.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		return y - rect.Y;
	}


void MaterialEditorPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
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
		// MAT-UI3b:这一帧开始时谁持焦(面板内点空白 = 结束这个状态;见函数注释)。
		const Wui::WuiId focusAtFrameStart = ctx.Focus();
		m_FocusOwningRects.clear();   // 每帧重建:本轮画过的"会接手焦点"的控件矩形
		// M4-S2:代码形态(`.slang`)与材质形态(`.wmat`)是本面板的两种形态,布局/动作各走一条;
		// `.wmat` 路径的既有行为一行不动。
		if (m_ShaderMode)
		{
			// M4-TEX-P11:纹理选取改成**行内下拉**后,代码形态不再有面板级模态
			// (旧的纹理选取模态已删除;Save As / 打开确认只存在于 `.wmat` 形态)。
			DrawShaderDocument(ctx, rect, host);
			ResetFocusAfterPanelBlankClick(ctx, rect, focusAtFrameStart);
			return;
		}
		// U25-M2:面板级模态(Save As… / 丢弃未保存改动再打开)按"自己的输入封锁"处理 ——
		// 材质面板可能是**独立窗口**(外壳的 m_PanelModalOwner 只作用于主窗口的停靠树),
		// 所以在本面板内容之前登记整窗遮挡,画模态本体之前解开(与 WuiModal 的三段式一致)。
		const bool panelModal = HasPanelModal();
		if (panelModal)
			Wui::BeginModalInputBlock(ctx);
		if (!m_Material)
		{
			// M3:加载失败的面板要给出**可读原因**(循环引用 / 父级链坏 / 文件读不到),
			// 不能只剩一句"没有材质"。整句同时进无障碍节点(material.load_error)与悬停。
			const std::string message = m_LoadError.empty()
				? Wui::Tr("panel.material.none_open",
					"No material open: double-click a .wmat file in the Content Browser")
				: Wui::Tr("panel.material.status.load_failed", "Load failed: ") + m_LoadError;
			Wui::Label(ctx, { rect.X + 10.0f, rect.Y + 10.0f },
				EllipsizeToWidth(ctx, message, std::max(40.0f, rect.W - 20.0f), 13.0f),
				m_LoadError.empty() ? theme.TextMuted : theme.Danger, 13.0f);
			if (!m_LoadError.empty())
			{
				const Wui::WuiRect errorRect { rect.X + 8.0f, rect.Y + 6.0f,
					std::max(40.0f, rect.W - 16.0f), 24.0f };
				RegisterReadOnlyNode(Wui::HashId("material.load_error"),
					Wui::Tr("panel.material.load_error", "Load error"), m_LoadError, errorRect, message);
				Wui::Tooltip(ctx, errorRect, message);
			}
			if (panelModal)
				Wui::EndModalInputBlock(ctx);
			ResetFocusAfterPanelBlankClick(ctx, rect, focusAtFrameStart);
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
			// 宽窗:左 = 预览卡片(图像 + 预览设置标签),右 = 可搜索的材质参数区,
			// 中间一条**可拖拽分隔条**(U27,用户「预览窗口能不能弄成可伸缩的」)。
			// 宽度顺序:预览列宽 = 会话记住的值(默认 40%,夹在 [220, 340]),本帧再夹到
			// [220, body.W - 参数列最小宽 - 3*Pad] —— 拖到极限也不越界、两列不重叠。
			const float defaultPreviewColumn = std::clamp(body.W * kSplitterDefaultRatio,
				kSplitterMinPreviewWidth, kSplitterDefaultMaxWidth);
			const float maxPreviewColumn = std::max(kSplitterMinPreviewWidth,
				body.W - kSplitterMinParamsWidth - 3.0f * pad);
			float& rememberedPreviewColumn = SessionPreviewColumnWidth();
			float previewColumn = rememberedPreviewColumn > 0.0f
				? rememberedPreviewColumn : defaultPreviewColumn;
			previewColumn = std::clamp(previewColumn, kSplitterMinPreviewWidth, maxPreviewColumn);
			// 分隔条轴线落在两列之间那段 Pad 的正中;命中带(6px)/悬停高亮/左右箭头光标
			// 都由共享控件负责(不新造原语)。
			const float splitAxis = body.X + pad + previewColumn + pad * 0.5f;
			const Wui::WuiRect splitterRect { splitAxis, body.Y + gap, 1.0f,
				std::max(40.0f, body.H - gap - pad) };
			const Wui::WuiRect splitterBand { splitAxis - 3.0f, splitterRect.Y, 6.0f, splitterRect.H };
			NoteFocusOwningRect(splitterBand);   // MAT-UI3b:分隔条自己接手焦点
			// 双击复位必须在控件调用**之前**:控件在按下那一帧把"当前值"记成拖拽锚点,
			// 先复位再按下 = 锚点就是默认值,不会出现"拖一下又跳回旧位置"。
			const Wui::WuiId splitterId = Wui::HashId("material.splitter");
			const bool splitterReset = ctx.IsDoubleClicked(splitterBand);
			if (splitterReset)
				previewColumn = defaultPreviewColumn;
			const bool splitterMoved = Wui::Splitter(ctx, splitterId, splitterRect, true, previewColumn,
				kSplitterMinPreviewWidth, maxPreviewColumn, theme);
			// 只有用户真的拖了/双击复位才写回会话记忆:窗口变窄时只在本帧夹取,不覆写。
			if (splitterMoved || splitterReset)
				rememberedPreviewColumn = previewColumn;
			const std::string splitterDoc = Wui::Tr("panel.material.splitter.tooltip",
				"Drag to change how much room the preview takes; double-click to restore the "
				"default 40% split. The preview keeps at least 220 and the parameters at least 260.");
			Wui::Tooltip(ctx, splitterBand, splitterDoc);
			AnnotateNode(ctx, splitterId, Wui::Tr("panel.material.splitter", "Preview / Parameters"),
				splitterDoc);
			previewZone = { body.X + pad, body.Y + gap, previewColumn,
				std::max(80.0f, body.H - gap - pad) };
			parameterRect = { body.X + pad + previewColumn + pad, body.Y + gap,
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
		ResetFocusAfterPanelBlankClick(ctx, rect, focusAtFrameStart);
	}

}
