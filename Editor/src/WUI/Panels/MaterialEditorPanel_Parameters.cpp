#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- 一行参数:标签 + 控件 + 恢复默认 + 悬停说明 + 无障碍 ----
	// ==== M4-S2:`.wmat` 引用 shader(`Shader:`)时的参数组 ====
	//
	// 语义(方案 §2.1 + M3 的继承口径):参数表来自 shader 的 `//! param` 注解;`.wmat`
	// **只存覆盖**,所以每行有三态 —— 覆盖(本文件)/ 父级覆盖 / shader 默认。行右侧的复位图标
	// 语义 = "丢掉本文件的覆盖,回退到 shader 默认(或父级)"。
float MaterialEditorPanel::ShaderParamSectionHeight(const Wui::WuiTheme& theme, float columnWidth) const{
		if (!m_Material)
			return 0.0f;
		const size_t rows = m_Material->Params().size();
		const size_t warnings = m_Material->ParamWarnings().size()
			+ (m_Material->ShaderWarning().empty() ? 0u : 1u);
		if (rows == 0 && warnings == 0)
			return 0.0f;   // 没有 Shader / 没有警告:整块不占位(既有 .wmat 布局逐像素不变)
		float height = kShaderGroupHeaderHeight;
		if (m_ShaderParamsOpen)
			for (const MaterialParamDecl& decl : m_Material->Params())
			{
				if (decl.Type == ParamType::Texture2D)
				{
					// 与绘制端同一份排版(含"资产 → 源图"缺失时的行内说明那一行)。
					const float labelWidthIn = std::min(kLabelColumnMax, std::max(84.0f, columnWidth * 0.30f));
					const bool withError = !Editor::TextureInlineWarning(
						m_Material->ResolvedParamValue(decl.Name)).empty();
					height += TextureRowLayoutFor(theme,
						ShaderTextureInlineControlWidth(columnWidth, labelWidthIn),
						ShaderTextureStackedControlWidth(columnWidth), withError).Height;
					continue;
				}
				height += ShaderParamRowHeight(decl.Type, kShaderRowHeight);
			}
		if (warnings > 0)
			height += 6.0f + static_cast<float>(warnings) * 16.0f;
		return height + kGroupGap;
	}


float MaterialEditorPanel::DrawShaderParamSection(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme, const Wui::WuiRect& contentRect, float y){
		if (!m_Material)
			return y;
		const std::vector<MaterialParamDecl>& decls = m_Material->Params();
		const std::vector<std::string>& paramWarnings = m_Material->ParamWarnings();
		const std::string shaderWarning = m_Material->ShaderWarning();
		const std::string shaderPath = m_Material->ShaderPath();
		if (decls.empty() && paramWarnings.empty() && shaderWarning.empty())
			return y;

		int overridden = 0;
		for (const MaterialParamDecl& decl : decls)
			if (m_Material->HasParamOverride(decl.Name))
				++overridden;
		const size_t warningCount = paramWarnings.size() + (shaderWarning.empty() ? 0u : 1u);

		// 容器(与其它分组同一条视觉语言:底 + 圆角描边 + 左侧归属竖条 + 子项缩进)。
		float blockHeight = kShaderGroupHeaderHeight;
		if (m_ShaderParamsOpen)
			for (const MaterialParamDecl& decl : decls)
			{
				if (decl.Type == ParamType::Texture2D)
				{
					const float labelWidthIn = std::min(kLabelColumnMax, std::max(84.0f, contentRect.W * 0.30f));
					const bool withError = !Editor::TextureInlineWarning(
						m_Material->ResolvedParamValue(decl.Name)).empty();
					blockHeight += TextureRowLayoutFor(theme,
						ShaderTextureInlineControlWidth(contentRect.W, labelWidthIn),
						ShaderTextureStackedControlWidth(contentRect.W), withError).Height;
					continue;
				}
				blockHeight += ShaderParamRowHeight(decl.Type, kShaderRowHeight);
			}
		if (warningCount > 0)
			blockHeight += 6.0f + static_cast<float>(warningCount) * 16.0f;
		const Wui::WuiRect blockRect { contentRect.X + 2.0f, y - 3.0f,
			std::max(40.0f, contentRect.W - 12.0f), blockHeight + 4.0f };
		// 滚出可视区:既不该画,也不该登记无障碍节点(与其它分组同一条规则),但高度照走。
		const bool blockVisible = (blockRect.Y + blockRect.H > contentRect.Y)
			&& (blockRect.Y < contentRect.Y + contentRect.H);
		if (!blockVisible)
			return y + blockHeight + kGroupGap;
		Wui::PanelBackground(ctx, blockRect, theme.ContentBg, 6.0f);
		Wui::HighlightOutline(ctx, blockRect, theme.Border, 6.0f, 1.0f);
		Wui::PanelBackground(ctx,
			{ blockRect.X + 1.0f, blockRect.Y + 5.0f, 2.0f, std::max(6.0f, blockRect.H - 10.0f) },
			overridden > 0 ? theme.Warning : theme.BorderStrong, 1.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.group.shader");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "group";
			node.Label = Wui::Tr("material.group.shader", "Shader Parameters");
			node.Value = std::to_string(decls.size())
				+ Wui::Tr("panel.material.a11y.items", " items, ")
				+ std::to_string(overridden) + Wui::Tr("panel.material.a11y.modified", " modified");
			node.Tooltip = Wui::Tr("material.group.shader.tooltip",
				"Parameters declared by the shader this material references (Shader: <path>). "
				"Each row shows whether the value is overridden here, inherited from the parent "
				"material, or the shader default.");
			node.Rect = blockRect;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// 组头:标题 + 计数 + `Edit Shader`(打开代码形态)。
		const Wui::WuiRect headerRect { blockRect.X + 4.0f, y, std::max(40.0f, blockRect.W - 8.0f),
			kShaderGroupHeaderHeight - 4.0f };
		const bool headerHovered = ctx.IsHovered(headerRect);
		Wui::HoverRow(ctx, headerRect, headerHovered, false, theme, 4.0f);
		Wui::Label(ctx, { headerRect.X + 8.0f, y + 3.0f }, m_ShaderParamsOpen ? "v" : ">",
			theme.TextMuted, 12.0f);
		Wui::Label(ctx, { headerRect.X + 22.0f, y + 3.0f },
			Wui::Tr("material.group.shader", "Shader Parameters"), theme.Text, 13.0f);
		const std::string countText = std::to_string(decls.size()) + " "
			+ Wui::Tr("panel.material.group.items", "items")
			+ (overridden > 0
				? std::string(" · ") + std::to_string(overridden) + " "
					+ Wui::Tr("panel.material.group.overridden", "overridden")
				: std::string(" · ") + Wui::Tr("material.group.shader.inherited", "none overridden"));
		const float countWidth = ctx.MeasureTextWidth(countText, 11.0f);
		Wui::Label(ctx, { headerRect.X + std::max(24.0f, headerRect.W - countWidth - 8.0f), y + 5.0f },
			countText, overridden > 0 ? theme.Warning : theme.TextDisabled, 11.0f);
		// 标题行的空白处点击 = 折叠/展开(与其它分组同一手感;右侧的按钮自己吃点击)。
		Wui::WuiRect toggleRect { headerRect.X, headerRect.Y, std::max(40.0f, headerRect.W), headerRect.H };
		if (!shaderPath.empty())
		{
			const float buttonWidth = ctx.MeasureTextWidth(
				Wui::Tr("material.group.shader.open", "Edit Shader"), 12.0f) + 16.0f;
			const Wui::WuiRect buttonRect { headerRect.X + headerRect.W - buttonWidth, y + 1.0f,
				buttonWidth, kShaderGroupHeaderHeight - 6.0f };
			toggleRect.W = std::max(40.0f, buttonRect.X - headerRect.X - 4.0f);
			if (Wui::Button(ctx, Wui::HashId("material.shader.open"), buttonRect,
				Wui::Tr("material.group.shader.open", "Edit Shader"), theme))
				host.OpenMaterialEditor(shaderPath);   // 同一个编辑器的代码形态
			Wui::Tooltip(ctx, buttonRect, Wui::Tr("material.group.shader.open.tooltip",
				"Open this material's shader (.slang) in the material editor's code form: "
				"preview | code | annotated parameters."));
		}
		if (headerHovered)
			ctx.SetCursor(Wui::WuiCursor::Hand);
		if (ctx.IsClicked(toggleRect))
			m_ShaderParamsOpen = !m_ShaderParamsOpen;
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.section.shader");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "section";
			node.Label = Wui::Tr("material.group.shader", "Shader Parameters");
			node.Value = (m_ShaderParamsOpen ? std::string("expanded") : std::string("collapsed"))
				+ ", " + std::to_string(decls.size())
				+ Wui::Tr("panel.material.a11y.items", " items, ")
				+ std::to_string(overridden) + Wui::Tr("panel.material.a11y.modified", " modified");
			node.Tooltip = Wui::Tr("panel.material.section.tooltip",
				"Click to expand or collapse this group.");
			node.Rect = toggleRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// 引用的 shader 路径本身也是一条可读信息(标题行右侧)。
		if (!shaderPath.empty())
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.path");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("material.shader.path.label", "Shader");
			node.Value = shaderPath;
			node.Tooltip = Wui::Tr("material.shader.path.tooltip", "Shader referenced by this material: ")
				+ shaderPath;
			node.Rect = { headerRect.X, headerRect.Y, std::max(40.0f, headerRect.W), headerRect.H };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		y += kShaderGroupHeaderHeight;
		if (m_ShaderParamsOpen)
		{
			for (const MaterialParamDecl& decl : decls)
			{
				const MaterialParamSource source = m_Material->ParamSource(decl.Name);
				const bool isOverride = m_Material->HasParamOverride(decl.Name);
				const std::string resolved = m_Material->ResolvedParamValue(decl.Name);
				const float labelWidthIn = std::min(kLabelColumnMax, std::max(84.0f, contentRect.W * 0.30f));
				const float rowX = contentRect.X + kGroupIndent;
				const float rowWidth = std::max(80.0f, contentRect.W - kGroupIndent - 6.0f);
				const float reserved = kResetWidth + 6.0f;
				// M4-TEX-P6a:纹理参数走"标签/控件最小宽之和 > 列宽 → 纵向堆叠"的同一份排版;
				// reserved(复位图标槽)不参与控件行,但要从可用宽里先扣掉(否则控件会顶到复位图标)。
				const bool textureDecl = decl.Type == ParamType::Texture2D;
				const float inlineControlWidth = std::max(60.0f, rowWidth - labelWidthIn - 8.0f - reserved);
				const float stackedControlWidth = std::max(60.0f, rowWidth - reserved);
				const TextureRowLayout layout = textureDecl
					? TextureRowLayoutFor(theme, inlineControlWidth, stackedControlWidth,
						!Editor::TextureInlineWarning(resolved).empty())
					: TextureRowLayout { false, ShaderParamRowHeight(decl.Type, kShaderRowHeight) };
				const float labelWidth = layout.StackLabel ? std::max(40.0f, rowWidth - reserved)
					: labelWidthIn;
				const float controlX = layout.StackLabel ? rowX : rowX + labelWidth + 8.0f;
				const float rowHeight = layout.Height;
				const Wui::WuiRect rowRect { rowX, y, rowWidth, rowHeight };
				// 控件高度:纹理参数簇固定 22;其它类型沿用"行高 − 4"(Vec4 的 2×2 需要 48)。
				const Wui::WuiRect controlRect { controlX,
					y + (layout.StackLabel ? 18.0f : 0.0f),
					layout.StackLabel ? stackedControlWidth : inlineControlWidth,
					textureDecl ? 22.0f : std::max(22.0f, rowHeight - 4.0f) };
				const Wui::WuiRect resetRect { rowX + rowWidth - kResetWidth - 2.0f, y + 1.0f,
					kResetWidth, kResetWidth };
				NoteFocusOwningRect(controlRect);   // MAT-UI3b:参数控件自己接手焦点
				const std::string label = decl.Label.empty() ? decl.Name : decl.Label;
				// 三态强调条(与 M3 的字段行同一条语言:覆盖 = Accent / 父级 = BorderStrong / shader 默认 = Border)。
				const Wui::WuiColor stateColor = isOverride ? theme.Accent
					: (source == MaterialParamSource::Parent ? theme.BorderStrong : theme.Border);
				Wui::PanelBackground(ctx, { rowX - 6.0f, y + 3.0f, 2.0f, rowHeight - 6.0f },
					stateColor, 1.0f);
				Wui::Label(ctx, { rowX, layout.StackLabel ? y : y + 4.0f },
					EllipsizeToWidth(ctx, label, labelWidth, 12.0f),
					isOverride ? theme.Text : theme.TextMuted, 12.0f);
				std::string edited = resolved;
				if (DrawShaderParamControl(ctx, host, theme, decl, controlRect, resolved, &edited)
					&& edited != resolved)
				{
					std::string normalized = edited;
					std::string valueError;
					if (NormalizeParamValue(decl.Type, normalized, &normalized, &valueError))
					{
						const uint32_t revision = m_Material->GetRevision();
						m_Material->SetParamOverride(decl.Name, normalized);
						if (m_Material->GetRevision() != revision)
							m_Material->MarkDirty(true);
						// M4-TEX-P11:选中的是源图时先导入成单文件容器(适配层),状态行先说这件事。
						m_Status = m_TextureChoiceNote.empty()
							? Wui::Tr("panel.material.status.param_override",
								"Parameter override written (press Save to keep it): ") + decl.Name
							: m_TextureChoiceNote;
						m_TextureChoiceNote.clear();
						m_StatusIsError = false;
					}
					else
					{
						m_Status = Wui::Tr("panel.material.status.param_override_invalid",
							"Parameter value rejected: ") + (valueError.empty() ? edited : valueError);
						m_StatusIsError = true;
					}
				}
				// 复位 = 丢掉本文件的覆盖(有父级时回到父级值,否则回到 shader 默认)。
				const std::string resetDoc = isOverride
					? (Wui::Tr("panel.material.shader.param.reset.tooltip",
						"Revert to the shader default (drops this file's override): ") + decl.Default)
					: Wui::Tr("panel.material.shader.param.reset.default.tooltip",
						"Already the shader default — nothing to revert.");
				if (Wui::ResetDefaultButton(ctx, Wui::HashId(
					("material.shader.params." + decl.Name + ".reset").c_str()), resetRect, isOverride,
					theme, label, resetDoc))
				{
					const uint32_t revision = m_Material->GetRevision();
					m_Material->RevertParam(decl.Name);
					if (m_Material->GetRevision() != revision)
						m_Material->MarkDirty(true);
				}
				// 悬停:注解 doc("…") 说明(有则第一行,MAT-UI7b)+ 类型 / 范围 / 单位 + 三态来源
				// (读屏看不到强调条,必须能读出来)。
				std::string doc = Wui::Tr("panel.material.shader.param.type", "Type: ")
					+ ParamTypeName(decl.Type);
				if (decl.Type == ParamType::Float || decl.Type == ParamType::Int)
					doc += "\n" + Wui::Tr("panel.material.shader.param.range", "Range: ")
						+ FormatParamFloatText(decl.Min) + " .. " + FormatParamFloatText(decl.Max);
				if (!decl.Unit.empty())
					doc += "\n" + Wui::Tr("panel.material.shader.param.unit", "Unit: ") + decl.Unit;
				doc += "\n" + (source == MaterialParamSource::Local
					? Wui::Tr("panel.material.shader.param.source.local", "Overridden in this file: ")
					: (source == MaterialParamSource::Parent
						? Wui::Tr("panel.material.shader.param.source.parent", "From parent override: ")
						: Wui::Tr("panel.material.shader.param.source.shader_default", "From shader default: ")))
					+ resolved;
				// M4-TEX-P6a:纹理参数把"资产 → 源图"关系与缺失原因接在同一句里(全文路径不丢)。
				if (decl.Type == ParamType::Texture2D)
				{
					const std::string textureDoc = Editor::TextureRefDoc(resolved);
					if (!textureDoc.empty())
						doc += "\n" + textureDoc;
				}
				const std::string rowDoc = ShaderParamRowTooltip(decl, doc);
				Wui::Tooltip(ctx, rowRect, rowDoc);
				RegisterShaderParamDocNode(decl, label, rowRect);
				if (decl.Type == ParamType::Texture2D && !resolved.empty())
				{
					Wui::WuiAccessNode node;
					node.Id = Wui::HashId(("material.param." + decl.Name + ".path").c_str());
					node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
					node.Kind = "text";
					node.Label = label + Wui::Tr("panel.material.texture.ref.path_label", " — path");
					node.Value = resolved;
					node.Tooltip = Editor::TextureRefDoc(resolved);
					node.Rect = { rowX, y + rowHeight - 2.0f, std::max(20.0f, rowWidth), 2.0f };
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
					node.Value = isOverride ? "override"
						: (source == MaterialParamSource::Parent ? "parent" : "shader-default");
					node.Tooltip = rowDoc;
					node.Rect = { rowX, y, 2.0f, rowHeight };
					node.Enabled = true;
					node.Interactive = false;
					node.Visible = true;
					Wui::WuiAccessibility::Get().Register(node);
				}
				y += rowHeight;
			}
		}
		// 警告区(未声明参数 / 值类型不符 / shader 读不到):**可读**且不阻断面板。
		if (warningCount > 0)
		{
			std::string joined;
			float warningY = y + 2.0f;
			if (!shaderWarning.empty())
			{
				Wui::Label(ctx, { contentRect.X + kGroupIndent, warningY },
					EllipsizeToWidth(ctx, Wui::Tr("panel.material.shader.warning", "Shader warning: ")
						+ shaderWarning, std::max(40.0f, contentRect.W - kGroupIndent - 8.0f), 11.0f),
					theme.Danger, 11.0f);
				joined = shaderWarning;
				warningY += 16.0f;
			}
			for (const std::string& warning : paramWarnings)
			{
				Wui::Label(ctx, { contentRect.X + kGroupIndent, warningY },
					EllipsizeToWidth(ctx, warning, std::max(40.0f, contentRect.W - kGroupIndent - 8.0f), 11.0f),
					theme.Warning, 11.0f);
				if (!joined.empty())
					joined += " | ";
				joined += warning;
				warningY += 16.0f;
			}
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.shader.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.shader.warning.label", "Shader parameter warnings");
			node.Value = joined;
			node.Tooltip = joined;
			node.Rect = { contentRect.X + kGroupIndent, y, std::max(40.0f, contentRect.W - kGroupIndent - 8.0f),
				warningCount * 16.0f + 4.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			y = warningY;
		}
		y += kGroupGap;
		return y;
	}


void MaterialEditorPanel::DrawParameterRow(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiTheme& theme, const RowPlan& row, float x, float y, float width, bool stacked, float labelWidthOverride, bool gridCell){
		const MaterialDesc& desc = m_Material->GetDesc();
		const Wui::WuiId controlId = Wui::HashId(row.ControlId.c_str());
		// U27:网格里标签列宽由整块网格统一给出(不是每行各算各的),这是"两列对齐"的前提。
		const float labelWidth = labelWidthOverride > 0.0f ? labelWidthOverride
			: (stacked ? width - 8.0f
				: std::min(kLabelColumnMax, std::max(84.0f, width * 0.36f)));
		const float labelY = stacked ? y + 1.0f : y + 4.0f;
		const float controlY = stacked ? y + 20.0f : y;
		const float controlHeight = stacked ? 20.0f : 22.0f;
		// U24:恢复默认 = **固定占位**(偏离默认与等于默认都占同一个槽位);控件区宽度与行高
		// 在两种状态下逐像素相同 —— 旧实现"只在偏离时出现"会把控件挤窄(用户反馈②)。
		const RowControl control = RowControlFor(row.Key);
		const bool hasResetSlot = row.HasReset && !row.ReadOnly && control != RowControl::Button;
		// U27:多列网格里**每一格**都留出同宽的占位(连只读格也一样),各列的值区才会严格对齐;
		// 单列沿用旧口径(只读行占满,不给不存在的按钮留白)。
		const bool reserveResetSlot = hasResetSlot || (gridCell && row.ReadOnly);
		// M4-TEX-P11:贴图行给组件**整个控件矩形**(定位按钮由组件在自己的槽位里切分:列表 |
		// 徽标条 | 定位 26+6)—— 面板不再额外预留定位宽、也不再自绘定位按钮(旧口径在"有引用"时
		// 会把定位宽从控件里再扣一次 ⇒ 下拉变窄、显示名被重新省略,实测踩过)。
		const bool textureRow = row.Key == std::string("albedo") || row.Key == std::string("normal");
		const float controlX = stacked ? x : x + labelWidth + 8.0f;
		const float reserved = reserveResetSlot ? kResetWidth + 6.0f : 0.0f;
		const float controlWidth = std::max(60.0f, width - (controlX - x) - reserved);
		const Wui::WuiRect controlRect { controlX, controlY, controlWidth, controlHeight };
		// MAT-UI3b:可编辑控件的矩形进"会接手焦点"名单(点空白清焦点时按它豁免)。
		if (!row.ReadOnly)
			NoteFocusOwningRect(controlRect);
		const Wui::WuiRect rowRect { x, y, width, row.Height };
		// 数值行的悬停说明补一句"条体拖动 / 值区输入 / ↑↓ 步进"(分类规则落在 DragBar 上的行)。
		// M3:继承态的字段把"继承自 <父>: <值>"拼进同一句悬停说明(读屏与脚本同源)。
		const FieldState fieldState = row.HasField ? StateOfField(row.Field) : FieldState::Override;
		const bool inheritedField = row.HasField && fieldState != FieldState::Override;
		const std::string stateDoc = row.HasField ? FieldStateDoc(row, fieldState) : std::string();
		std::string rowDoc = control == RowControl::DragBar
			? row.Doc + "\n" + Wui::Tr("panel.material.numeric.tooltip",
				"Drag the bar to change the value, or click the value box to type it "
				"(Enter commits, Esc cancels); Up/Down step by 1% of the range.")
			: row.Doc;
		if (!stateDoc.empty())
			rowDoc += "\n" + stateDoc;
		// M4-TEX-P6a:贴图行把"资产 → 源图"关系与缺失原因拼进同一句悬停说明
		// (完整逻辑路径也在这里,下拉显示名只是短名)。
		if (textureRow)
		{
			const std::string& referenced = row.Key == std::string("normal")
				? desc.NormalTexture : desc.AlbedoTexture;
			const std::string textureDoc = Editor::TextureRefDoc(referenced);
			if (!textureDoc.empty())
				rowDoc += "\n" + textureDoc;
		}
		Wui::Tooltip(ctx, rowRect, rowDoc);

		// M3:继承/覆盖的左侧强调条(与组容器的归属竖条同一条竖线上):
		//  覆盖 = Accent(这一行是本文件的显式值)、继承 = BorderStrong、引擎默认 = Border(更弱)。
		// 只画在行内,不占布局 —— U24 的"固定占位 / 零位移"口径不变。
		if (row.HasField)
		{
			const Wui::WuiColor barColor = fieldState == FieldState::Override ? theme.Accent
				: (fieldState == FieldState::Inherited ? theme.BorderStrong : theme.Border);
			Wui::PanelBackground(ctx,
				{ x - 6.0f, y + 3.0f, 2.0f, std::max(4.0f, row.Height - 6.0f) }, barColor, 1.0f);
		}

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
			// 贴图引用有问题(缺失 / 资产没有源图)时标签转警示色:行内就能看出这一行不对。
			(textureRow && TextureFieldHasIssue(row.Key)) ? theme.Warning
				: (inheritedField ? theme.TextMuted : theme.Text),
			12.0f);

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
		else if (textureRow)
		{
			// U25-M2 B + M4-TEX-P11:贴图槽 = `Wui::WuiTexturePicker` 的**整槽矩形**
			// (清单 | 徽标条 | 定位按钮都由组件切分;a11y id 口径保持 `material.<key>` /
			// `material.<key>.locate`,新增 `material.<key>.state`)。
			const bool isNormal = row.Key == std::string("normal");
			const std::string current = isNormal ? desc.NormalTexture : desc.AlbedoTexture;
			RegisterSlotDrop(ctx, host, row.Key, controlRect);
			// 跨窗口拖放:面板取走一次投放 → 组件应用(Options::DroppedValue);同窗口由组件自收。
			std::string dropped;
			const bool consumedDrop = TakeSlotDrop(host, row.Key, &dropped);
			auto options = Editor::TexturePickerOptions(current, row.Label, "material." + row.Key,
				consumedDrop ? dropped : std::string());
			bool revealRequested = false;
			options.RevealRequested = &revealRequested;
			// 值 = 组件自己的进出参:返回值 = **值有没有变**(变才写材质 —— 每帧写会让 Revision 逐帧
			// +1,渲染侧每帧重建材质描述符集 → 预览逐帧闪)。
			std::string value = current;
			const bool changed = Wui::WuiTexturePicker(ctx, controlId, controlRect, value, options, theme);
			if (changed)
			{
				// M4-TEX P9:选中源图 = 当场导入成单文件容器资产,材质引用资产。
				std::string importNote;
				const std::string chosen = Editor::NormalizeTextureChoice(value, &importNote);
				m_TextureChoiceNote.clear();   // 槽位导入反馈直接进状态行(不给后续写入路径留陈旧句子)
				if (chosen != current)
				{
					applyEdit([&]
					{
						if (isNormal)
							m_Material->SetNormalTexture(chosen);
						else
							m_Material->SetAlbedoTexture(chosen);
					});
					Editor::InvalidateTextureRefCatalog();   // 刚导入的资产下一帧就能在清单里看到
					m_ValidationRevision = 0;
					if (!importNote.empty())
					{
						m_Status = importNote;
						m_StatusIsError = false;
					}
					else if (consumedDrop)
					{
						m_Status = Wui::Tr("panel.material.status.texture_dropped",
							"Texture assigned by drop: ") + chosen;
						m_StatusIsError = false;
					}
					WLD_CORE_INFO("[material-ui] texture pick '{0}' -> slot '{1}' (assigned '{2}')", value,
						row.Key, chosen);
				}
			}
			if (revealRequested)
				RevealTextureRef(host, value.empty() ? current : value);
			// 完整逻辑路径 + "资产 → 源图"关系进无障碍节点(下拉里显示的是完整路径,脚本/读屏按它读值)。
			const std::string shown = changed ? value : current;
			if (!shown.empty())
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(("material.texture." + row.Key + ".path").c_str());
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "text";
				node.Label = row.Label + Wui::Tr("panel.material.texture.ref.path_label", " — path");
				node.Value = shown;
				node.Tooltip = Editor::TextureRefDoc(shown);
				node.Rect = { controlRect.X, controlRect.Y + controlRect.H, controlRect.W, 2.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
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

		// M3:继承态的**值区弱化** —— 只在非悬停/非焦点时压一层半透明面板底,
		// 悬停/焦点反馈与无障碍节点(仍可交互、仍登记)不受影响。
		if (inheritedField && !ctx.IsHovered(controlRect) && ctx.Focus() != controlId)
		{
			Wui::WuiColor dim = theme.PanelBg;
			dim.A = 0.22f;
			Wui::PanelBackground(ctx, controlRect, dim, 2.0f);
		}

		// ---- 回退到父级(固定占位:两个状态同一个 rect,行布局零位移)----
		if (hasResetSlot)
		{
			const Wui::WuiRect resetRect { x + width - kResetWidth, controlY, kResetWidth, controlHeight };
			const std::string resetId = ResetIdFor(row.Key);
			// M3:按钮语义 = 回退到父级(没有父级 = 回退引擎默认);按钮的 a11y value 仍是
			// modified/default(= 覆盖位),U24 的固定占位与 U21 的值口径都不变。
			// 只有父级真的解析到了才算"回退到父级";父级缺失 = 回退引擎默认(与字段态一致)。
			const bool hasParentFile = m_Material->ResolvedParent() != nullptr;
			const std::string resetLabel = hasParentFile
				? Wui::Tr("panel.material.revert_row", "Revert to parent")
				: Wui::Tr("panel.material.revert_row.engine", "Revert to engine default");
			const std::string resetDoc = (hasParentFile
				// 批准 5(2026-09-23):这一条与头部 Revert 按钮共用过 "panel.material.revert.tooltip",
				// 中文目录里后写的"回退到父级…"把头部按钮的"丢弃未保存改动"覆盖掉了 —— 拆成独立 key。
				? Wui::Tr("panel.material.revert.tooltip_row",
					"Revert to parent: drop this file's value so the field inherits again from") + " "
					+ m_Material->ParentPath() + "."
				: Wui::Tr("panel.material.revert.tooltip_engine",
					"Revert to engine default: drop this file's value (this material has no parent)."))
				+ " " + Wui::Tr("panel.material.reset.placeholder",
					"Always occupies this slot: it stays dimmed while the field is already inherited.");
			// 控件自己登记 a11y(kind="reset-default"、value="modified"/"default"、enabled 跟随),
			// 两个状态只改高亮不改几何 —— 探测口径:前后行矩形逐像素相同。
			const Wui::WuiId resetWuiId = Wui::HashId(resetId.c_str());
			if (Wui::ResetDefaultButton(ctx, resetWuiId, resetRect, row.Modified,
				theme, resetLabel, resetDoc))
				SetFieldToDefault(row.Key);
			// 控件的 a11y 节点自带 kind/value/enabled,但**不带**悬停说明:两态都补齐
			// (AnnotateNode 保留 live 的 value/kind,只补 label/tooltip)。
			AnnotateNode(ctx, resetWuiId, resetLabel, resetDoc);
		}
		// 控件自己登记过节点:补上参数名与悬停说明(值/矩形仍以控件为准)。
		AnnotateNode(ctx, controlId, row.Label, rowDoc);
		// M3:继承态在无障碍里可读(id + value + tooltip):override / inherited / engine-default。
		if (row.HasField)
		{
			RegisterReadOnlyNode(Wui::HashId(("material.prop." + row.Key + ".state").c_str()),
				row.Label, FieldStateName(fieldState), rowRect, stateDoc, "material-field-state");
		}
	}


	// ---- U27:参数列的响应式网格 ----
	//
	// 规则(方案 §A):可用宽度足够时每行放 2 个短字段(极宽 3 个),不够就回落成 1 个/行;
	// "长内容"独占整行 —— 贴图路径(资产下拉)、材质名(自由文本)、三分量向量、动作行。
	// 配对只用**组内顺序**:连续短字段按原顺序成行(例如基础外观的金属度/粗糙度、
	// 透明度与混合的混合模式/双面、贴图采样的两条色彩空间诊断、高级的格式/修订),
	// 长字段会打断当前行 —— 不跨组、不跳序、不硬凑。
bool MaterialEditorPanel::RowSpansFullWidth(const RowPlan& row){
		return row.Key == "albedo"        // 资产下拉:路径可能很长
			|| row.Key == "normal"        // 资产下拉
			|| row.Key == "emissive"      // 三分量向量:每分量一个输入框
			|| row.Key == "name"          // 自由文本
			|| row.Key == "reset_all";    // 动作行(按钮)
	}


int MaterialEditorPanel::GridColumnCount(float width, const Wui::WuiTheme& theme){
		int columns = 1;
		for (int candidate = 2; candidate <= kGridMaxColumns; ++candidate)
		{
			const float needed = kGridCellMinWidth * static_cast<float>(candidate)
				+ theme.PadSmall * static_cast<float>(candidate - 1);
			if (width + 0.5f >= needed)
				columns = candidate;
		}
		return columns;
	}


std::vector<std::vector<const MaterialEditorPanel::RowPlan*>> MaterialEditorPanel::BuildGridLines( const std::vector<const RowPlan*>& rows, int columns){
		std::vector<std::vector<const RowPlan*>> lines;
		std::vector<const RowPlan*> pending;
		const auto flush = [&lines, &pending]()
		{
			if (!pending.empty())
			{
				lines.push_back(pending);
				pending.clear();
			}
		};
		for (const RowPlan* plan : rows)
		{
			if (columns <= 1 || RowSpansFullWidth(*plan))
			{
				flush();
				lines.push_back({ plan });
				continue;
			}
			pending.push_back(plan);
			if (static_cast<int>(pending.size()) >= columns)
				flush();
		}
		flush();
		return lines;
	}


	// ---- 参数区:搜索 + 分组折叠 + 每字段控件 + 校验区 ----
float MaterialEditorPanel::DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
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
		NoteFocusOwningRect(searchRect);   // MAT-UI3b:搜索框自己接手焦点
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
			// M3:整份回退的按钮文案跟随父级是否存在(有父级 = 回退到父级,否则回退引擎默认)。
			if (plan.Key == std::string("reset_all"))
				plan.Label = m_Material->ParentPath().empty()
					? Wui::Tr("material.prop.reset_all.engine", "Revert All to Engine Default")
					: Wui::Tr("material.prop.reset_all.parent", "Revert All to Parent");
			plan.ReadOnly = spec.ReadOnly != 0;
			plan.HasReset = spec.HasReset != 0;
			plan.Modified = plan.HasReset && !plan.ReadOnly && FieldModified(plan.Key);
			// M3:可继承字段的继承/覆盖状态(参数列的强调条、弱化、状态节点都用它)。
			MaterialField field;
			if (FieldForKey(plan.Key, &field))
			{
				plan.HasField = true;
				plan.Field = field;
				plan.Override = m_Material->HasOverride(field);
			}
			// 只读行的显示值(采样/高级诊断)。
			if (plan.Key == std::string("normal.space"))
				plan.Value = Wui::Tr("material.value.linear", "Linear (UNORM, no sRGB decode)");
			else if (plan.Key == std::string("albedo.space"))
				plan.Value = Wui::Tr("material.value.srgb", "sRGB (hardware decode)");
			else if (plan.Key == std::string("sampler"))
			{
				char samplerText[128] = {};
				std::snprintf(samplerText, sizeof(samplerText),
					Wui::Tr("panel.material.value.sampler.aniso", "%s, anisotropy %.0f").c_str(),
					Wui::Tr("material.value.sampler", "Linear filter, repeat wrap").c_str(),
					static_cast<double>(RenderSettings::Get().Anisotropy));
				plan.Value = samplerText;
			}
			else if (plan.Key == std::string("format"))
				plan.Value = Wui::Tr("panel.material.value.format", "FormatVersion ")
					+ std::to_string(m_Material->GetFormatVersion());
			else if (plan.Key == std::string("revision"))
				plan.Value = Wui::Tr("panel.material.value.revision", "Revision ")
					+ std::to_string(m_Material->GetRevision());
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

		// ---- U27:响应式网格(列数 / 格宽 / 统一标签列宽)----
		// 列数只看**可用宽度**(不够就 1 列,既有窄窗口径不变);格宽与标签列宽在整块网格里
		// 是同一个值 —— 两列的标签、值区才会逐列对齐,而不是每行各算各的。
		const float gridAreaWidth = std::max(80.0f, contentRect.W - kGroupIndent - 10.0f);
		const int gridColumns = GridColumnCount(gridAreaWidth, theme);
		const float gridGap = theme.PadSmall;
		const float gridCellWidth = std::max(80.0f, (gridAreaWidth
			- gridGap * static_cast<float>(gridColumns - 1)) / static_cast<float>(gridColumns));
		const float gridLabelWidth = gridColumns >= 2
			? std::clamp(gridCellWidth * 0.42f, 64.0f, 110.0f) : -1.0f;
		const auto rowsOfGroup = [&plans](const std::string& key)
		{
			std::vector<const RowPlan*> rows;
			for (const RowPlan& plan : plans)
				if (plan.Group == key)
					rows.push_back(&plan);
			return rows;
		};
		const auto lineHeightOf = [](const std::vector<const RowPlan*>& line)
		{
			float height = 0.0f;
			for (const RowPlan* plan : line)
				height = std::max(height, plan->Height);
			return height;
		};

		// ---- 内容高度(折叠的组只占组头;展开的组 = 容器块,含子项)----
		float contentHeight = 6.0f;
		for (const GroupDesc& group : kGroups)
		{
			const std::vector<const RowPlan*> rows = rowsOfGroup(group.Key);
			if (rows.empty())
				continue;
			contentHeight += kGroupHeaderHeight + kGroupGap;
			if (!m_SectionOpen[group.Index] && needle.empty())
				continue;
			for (const std::vector<const RowPlan*>& line : BuildGridLines(rows, gridColumns))
				contentHeight += lineHeightOf(line);
		}
		// M4-S2:`Shader:` 引用的参数组(没有 shader/没有警告 = 0,既有布局逐像素不变)。
		contentHeight += ShaderParamSectionHeight(theme, contentRect.W);
		const float maxScroll = std::max(0.0f, contentHeight - contentRect.H);
		m_ScrollY = std::clamp(m_ScrollY, 0.0f, maxScroll);

		Wui::BeginScrollArea(ctx, contentRect, contentHeight, m_ScrollY, theme);
		float y = contentRect.Y + 4.0f - m_ScrollY;
		int drawnRows = 0;
		// M4-S2:引用了 shader 的材质,参数列**以 shader 声明的参数开头** —— 那些参数才是这份实例
		// 真正的可覆盖项(用户口径:`.wmat` = 实例覆盖值);旧的内建字段组跟在后面。
		// 没有 shader / 没有警告时这一句是 no-op(既有材质面板布局逐像素不变)。
		if (!m_Material->Params().empty() || !m_Material->ShaderWarning().empty()
			|| !m_Material->ParamWarnings().empty())
			y = DrawShaderParamSection(ctx, host, theme, contentRect, y);
		for (const GroupDesc& group : kGroups)
		{
			const std::vector<const RowPlan*> rows = rowsOfGroup(group.Key);
			if (rows.empty())
				continue;
			const std::vector<std::vector<const RowPlan*>> lines = BuildGridLines(rows, gridColumns);
			const bool open = m_SectionOpen[group.Index] || !needle.empty();
			// M3:组头的数字 = 本组里被**覆盖**的字段数(不是"与引擎默认不同")。
			const int modified = GroupOverrideCount(group.Key);
			const int rowCount = static_cast<int>(rows.size());
			// U22(用户 §5.2「折叠设计的怪怪的,分别区分不出来折叠内容属于哪里」):
			// 组 = **容器**:底 + 圆角描边 + 左侧归属竖条 + 子项缩进 + 组间留白。
			// 先把整块尺寸算出来(组头 + 标签条 + 属于本标签的行),再一次性画底。
			float blockHeight = kGroupHeaderHeight;
			if (open)
			{
				for (const std::vector<const RowPlan*>& line : lines)
					blockHeight += lineHeightOf(line);
			}
			const Wui::WuiRect blockRect { contentRect.X + 2.0f, y - 3.0f,
				std::max(40.0f, contentRect.W - 12.0f), blockHeight + 4.0f };
			// 滚动裁剪由 BeginScrollArea(ClipPush)负责,这里只跳过"整块在视口之外"的情况。
			const bool blockVisible = (blockRect.Y + blockRect.H > contentRect.Y)
				&& (blockRect.Y < contentRect.Y + contentRect.H);
			if (blockVisible)
			{
				Wui::PanelBackground(ctx, blockRect, theme.ContentBg, 6.0f);
				Wui::HighlightOutline(ctx, blockRect, theme.Border, 6.0f, 1.0f);
				Wui::PanelBackground(ctx,
					{ blockRect.X + 1.0f, blockRect.Y + 5.0f, 2.0f, std::max(6.0f, blockRect.H - 10.0f) },
					modified > 0 ? theme.Warning : theme.BorderStrong, 1.0f);
				// 容器节点(只读):脚本按它断言"组头与本组子项都落在这块容器里"(归属关系),
				// 也方便无障碍读屏讲清"这些行属于哪一组"。
				Wui::WuiAccessNode groupNode;
				groupNode.Id = Wui::HashId((std::string("material.group.") + group.Key).c_str());
				groupNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				groupNode.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				groupNode.Kind = "group";
				groupNode.Label = GroupLabel(group);
				groupNode.Value = std::to_string(rowCount)
					+ Wui::Tr("panel.material.a11y.items", " items, ")
					+ std::to_string(modified) + Wui::Tr("panel.material.a11y.modified", " modified");
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
				// 折叠后也要能看出这一组里有多少项、多少项覆盖(用户 §5.2 + M3 的继承语义)。
				const std::string countText = std::to_string(rowCount) + " "
					+ Wui::Tr("panel.material.group.items", "items")
					+ (modified > 0
						? std::string(" · ") + std::to_string(modified) + " "
							+ Wui::Tr("panel.material.group.overridden", "overridden")
						: std::string(" · ") + Wui::Tr("panel.material.group.inherited",
							"all inherited"));
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
						+ std::to_string(rowCount)
						+ Wui::Tr("panel.material.a11y.items", " items, ")
						+ std::to_string(modified) + Wui::Tr("panel.material.a11y.modified", " modified");
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
			for (const std::vector<const RowPlan*>& line : lines)
			{
				const float lineHeight = lineHeightOf(line);
				// 校验条目点击后的定位:在**可见性判断之前**处理 —— 目标行通常正在视口外,
				// 那正是要滚过去的情况(下一帧生效,行高是本帧算出来的)。
				for (const RowPlan* plan : line)
				{
					if (m_RevealField.empty() || plan->Key != m_RevealField)
						continue;
					if (y < contentRect.Y + 2.0f)
						m_ScrollY = std::max(0.0f, m_ScrollY - (contentRect.Y + 2.0f - y));
					else if (y + lineHeight > contentRect.Y + contentRect.H - 2.0f)
						m_ScrollY = std::min(maxScroll,
							m_ScrollY + (y + lineHeight - (contentRect.Y + contentRect.H - 2.0f)));
					if (--m_RevealFrames <= 0)
						m_RevealField.clear();
				}
				const bool visible = (y + lineHeight > contentRect.Y)
					&& (y < contentRect.Y + contentRect.H);
				if (!visible)
				{
					y += lineHeight;
					continue;
				}
				for (size_t cell = 0; cell < line.size(); ++cell)
				{
					++drawnRows;
					// 子项缩进:与组头左侧竖条对齐,让"这些行属于上面那一组"一眼可见。
					// U27:同一行的第 2/3 格按格宽 + PadSmall 依次排开,标签列宽全网格一致。
					const float cellX = contentRect.X + kGroupIndent
						+ static_cast<float>(cell) * (gridCellWidth + gridGap);
					DrawParameterRow(ctx, host, theme, *line[cell], cellX, y, gridCellWidth,
						gridColumns == 1 ? stacked : false, gridLabelWidth, gridColumns >= 2);
				}
				y += lineHeight;
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
			Wui::PanelBackground(ctx, track, Wui::WuiColor { 0.169f, 0.192f, 0.220f, 1.0f }, 1.0f);
			Wui::PanelBackground(ctx, { track.X, track.Y + offset, 2.0f, thumbHeight },
				Wui::WuiColor { 0.298f, 0.553f, 1.0f, 0.55f }, 1.0f);
		}
		// M4-TEX-P6a:空态只在"过滤后真的没有任何行"时出现。旧口径用 drawnRows == 0,于是
		// "shader 参数段把自己撑满视口、内建字段组全在滚动区外"时会在顶部误报
		// "No parameters in this material."(屏幕上同时有 16 个参数),还会压在参数段组头上(实测抓图)。
		if (drawnRows == 0 && plans.empty())
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

}
