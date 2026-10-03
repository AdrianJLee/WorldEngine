#include "EditorShell_Internal.h"

namespace World
{

using namespace EditorShellDetail;


	// ---- P3-1①:面板拖拽状态归零(唯一出口)----
	// "这次拖拽是谁":拖起手标签 / 跟随窗口 / 落点位置。松手那一帧也要清 ——
	// 残留的 m_TabDragPanel/m_DragPanel 会让面板一回到停靠树就被再次浮出(P3-1① 的 bug)。
void EditorShell::ClearPanelDragIdentity(){
		m_DragPanel.clear();
		m_LastDragPos = { 0.0f, 0.0f };
		m_TabDragPanel.clear();
		m_MovingFloat.clear();
		m_FloatGrabOffset = { 0.0f, 0.0f };
	}


	// 完整归零:身份成员 + 落点成员 + 挂靠标签拖拽。
	// 落点成员(m_DropTargetPanel/m_EdgeDockActive/…)是"最后一帧武装的目标",WuiContext 要到
	// **下一帧**才交付 AcceptDrop —— 所以在"释放沿"那一帧只能清身份成员(见状态机里结束分支的
	// 说明),只有落点已消费或显式取消(Esc)时才走这个完整版本。
void EditorShell::ClearPanelDragState(){
		ClearPanelDragIdentity();
		m_DropTargetPanel.clear();
		m_DropZone = Wui::DropZone::Center;
		m_EdgeDockActive = false;
		m_EdgeDropZone = Wui::DropZone::Center;
		m_LastDragTarget.clear();
		m_LastDragZone = Wui::DropZone::Center;
		m_DropPreviewActive = false;
		m_AttachTagPress.clear();
		m_AttachTagDrag.clear();
	}


	// 拖拽确定已经结束(drop 已消费 / 松手未落点 / Esc 取消 / 左键已抬起)时的收口。
void EditorShell::EndPanelDrag(Wui::WuiContext& ctx){
		ClearPanelDragState();
		// ctx 里的 dragging/pending/payload/drop 标记一起清:否则下一条命令/下一帧仍会看到
		// "payload 是 panel:xxx"的残留拖拽(正是"面板刚回到停靠树就再次浮出"的原料)。
		ctx.EndDrag();
	}


	// 面板当前形态标签(单一事实源):AiDetachPanel/AiAttachPanel 的幂等判定、state.dump 的
	// "attach" 段、AttachTag 无障碍节点的 value 都用它,避免三处各写一套判定。
const char* EditorShell::PanelStateLabel(const std::string& panel) const{
		if (IsIndependentPanel(panel))
		{
			// 独立窗口形态只有三种状态:顶栏标签(attached)/ 真 OS 窗口(floating)/ 关闭。
			if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
				return "attached";
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (!host->Contains(panel))
					continue;
				if (!host->IsHidden())
					return "floating";
				// 隐藏宿主:窗口里只要有一个标签在附加列表里,本面板就属于那枚顶栏标签
				// (窗口被拆成多个标签页时,state.dump / 无障碍节点都要说真话)。
				for (const std::string& id : host->Panels())
					if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), id) != m_AttachedPanels.end())
						return "attached";
				break;
			}
			return "hidden";
		}
		// 停靠形态面板:docked(在停靠树里)/ floating(主窗口内临时浮动)/ hidden。
		if (m_Layout.Contains(panel))
			return "docked";
		if (m_Layout.IsFloating(panel))
			return "floating";
		return "hidden";
	}


void EditorShell::TogglePanel(Wui::WuiContext& ctx, const std::string& panel){
		const std::string before = m_Layout.Serialize();
		// 已附加到主窗口(标签切换状态):菜单点击 = 关闭该窗口,
		// 隐藏其面板并把标签移出栏(此前只调 HideFloatPanel,会残留附加标签)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		if (attached != m_AttachedPanels.end())
		{
			// P4-U13e:prefab 面板有未保存改动 → 先弹确认;**不要**先把标签摘掉
			// (窗口还在,只是内容不画了 —— 那就是"静默丢"的另一种形态)。
			if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
				return;
			CloseFloatWindow(panel, true, &ctx);
			m_AttachedPanels.erase(attached);
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
			return;
		}
		// 独立窗口:开着 → 关闭(隐藏复用,不写回停靠树);关着 → 打开/复用同一窗口。
		if (IsIndependentPanel(panel))
		{
			if (m_Layout.IsFloating(panel))
			{
				HideFloatPanel(panel, &ctx);
				return;
			}
			// D10:默认打开 = **附加到主窗口**(用户 2026-09-19)。Window 菜单与 AI `ui.open`
			// 都走这条,所以这一处覆盖所有"声明为独立形态"的面板(gallery/input/scripts)。
			OpenPanelAttached(panel);
			RecordDockChange(ctx, "attach", panel, before);
			return;
		}
		// 停靠形态面板:临时浮动着 → 回停靠位(D3);已停靠 → 隐藏;两者都不是 → 回到停靠树。
		if (m_Layout.IsFloating(panel))
		{
			if (DockPanelBackToTree(panel))
				RecordDockChange(ctx, "dock", panel, before);
			return;
		}
		if (m_Layout.Contains(panel))
		{
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "hide", panel, before);
			return;
		}
		{
			// 优先回到该面板**上次所在的标签组**(RenderTabs 每帧刷新锚点表);
			// 该组已不存在时再退回 FirstPanel,树为空则重建根组。
			std::string anchor;
			if (const auto remembered = m_LastDockAnchors.find(panel); remembered != m_LastDockAnchors.end() &&
				m_Layout.Contains(remembered->second))
				anchor = remembered->second;
			else
				anchor = m_Layout.FirstPanel();
			if (anchor.empty())
			{
				Wui::DockLayout fresh;
				fresh.Root.Panels.push_back(panel);
				fresh.Root.Active = 0;
				m_Layout = std::move(fresh);
				RecordDockChange(ctx, "show", panel, before);
			}
			else if (m_Layout.AddTab(panel, anchor, Wui::DropZone::Center))
				RecordDockChange(ctx, "show", panel, before);
		}
	}


	// 停靠形态面板:从"临时拖出"回到停靠树(锚点 = 树里第一个面板;树为空则重建根 tab 组)。
bool EditorShell::DockPanelBackToTree(const std::string& panel){
		if (IsIndependentPanel(panel))
			return false;
		bool changed = false;
		if (m_Layout.IsFloating(panel))
		{
			m_Layout.Floating.erase(std::remove_if(m_Layout.Floating.begin(), m_Layout.Floating.end(),
				[&](const Wui::DockFloat& entry) { return entry.Panel == panel; }), m_Layout.Floating.end());
			changed = true;
		}
		if (m_Layout.Contains(panel))
			return changed;
		const Wui::PanelId anchor = m_Layout.FirstPanel();
		if (!anchor.empty())
			return m_Layout.AddTab(panel, anchor, Wui::DropZone::Center) || changed;
		const std::vector<Wui::DockFloat> floating = std::move(m_Layout.Floating);
		const std::vector<Wui::DockFloat> memory = std::move(m_Layout.FloatMemory);
		Wui::DockLayout fresh;
		fresh.Root.Panels.push_back(panel);
		fresh.Root.Active = 0;
		fresh.Floating = floating;
		fresh.FloatMemory = memory;
		m_Layout = std::move(fresh);
		return true;
	}


	// Window 菜单:打开独立形态面板 = 复用已隐藏的窗口(含它原有的其它标签)。
void EditorShell::OpenIndependentPanel(const std::string& panel){
		const Wui::WuiRect rect = FloatRectFor(panel);
		if (!m_Layout.IsFloating(panel))
			m_Layout.Floating.push_back({ panel, rect });
		m_LastFloatRects[panel] = rect;
		AddFloatWindow(panel, rect, "open");
	}


void EditorShell::OpenPanelAttached(const std::string& panel){
		// D10(用户 2026-09-19):**所有独立窗口默认附加到主窗口**。
		//  - 已经附加 → 只激活对应标签;
		//  - 用户此前把它拖成了独立窗口(布局里有浮动记录)→ 尊重现状,只前置焦点;
		//  - 否则先按独立窗口建出(复用已隐藏的窗口),随后立即挂靠到主窗口。
		// 显式分离(拖出/菜单)仍然可用 —— 这个函数只改"默认打开"的落点。
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panel;
			return;
		}
		if (m_Layout.IsFloating(panel))
		{
			FocusIndependentWindow(panel);
			return;
		}
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel);
		AttachIndependentWindowToSlot(panel);
	}


void EditorShell::ResetLayout(Wui::WuiContext& ctx){
		const std::string before = m_Layout.Serialize();
		// 重置也要按形态声明:独立窗口面板不进停靠树。
		std::vector<Wui::PanelId> dockedPanels;
		for (const PanelSpec& spec : kPanelSpecs)
			if (spec.Form == PanelForm::Docked)
				dockedPanels.push_back(spec.Id);
		m_Layout = Wui::DockLayout::Default(dockedPanels);
		RecordDockChange(ctx, "reset", "", before);
	}


void EditorShell::OnRender(Wui::WuiContext& ctx){
		m_Ctx = &ctx;
		// P4-UX1:主题模式(暗/浅/跟随系统)变化时刷新本地副本;独立窗口在下次创建/附加时
		// 取到新主题(它们的 callbacks.Theme 是创建期快照)。
		if (m_ThemeGeneration != Wui::ThemeGeneration())
		{
			m_Theme = Wui::CurrentTheme();
			m_ThemeGeneration = Wui::ThemeGeneration();
		}
		// W9-2 修复:上一帧被推迟的脚本编辑器打开请求,在帧边界统一执行(安全点)。
		if (!m_PendingScriptOpen.empty())
		{
			std::vector<std::string> pending;
			pending.swap(m_PendingScriptOpen);
			for (const std::string& path : pending)
				OpenScriptEditorNow(path);
		}
		// PECS-T11:内容浏览器「新建 ▶ Lua 脚本…」= 打开**同一个** Lua 向导(与 C++ 的
		// New C++ … 同构)。面板渲染期只置标记(拿不到安全的打开点),这里在帧边界统一打开。
		if (auto* browser = ContentBrowserPanelInstance())
		{
			if (browser->ConsumePendingLuaWizardRequest())
				OpenNewLuaSystemModal(ctx);
		}
		// PROJ-1/T1:"New Project…" 的 Browse…(原生文件夹对话框)与 Create(写盘)同样是
		// **帧边界**动作:上一帧只置标记,这里统一执行(渲染中途弹 Win32 模态/写盘会踩坑,
		// 与 m_PendingScriptOpen 同一条纪律)。
		if (m_NewProjectBrowsePending)
		{
			m_NewProjectBrowsePending = false;
			RunNewProjectBrowse(ctx);
		}
		if (m_NewProjectCreatePending)
		{
			m_NewProjectCreatePending = false;
			RunNewProjectCreate(ctx);
		}
		// PLUG-AUTH-1:"New Plugin…" 的落盘同样只在帧边界执行(上一帧只置标记)。
		if (m_NewPluginCreatePending)
		{
			m_NewPluginCreatePending = false;
			RunNewPluginCreate(ctx);
		}
		// PROJ-2/T1:File ▸ Open Project… / 启动器 ▸ 打开项目… 的"选择目录"原生对话框
		// 同样只在帧边界弹(上一帧只置标记;与 New Project 向导同一条纪律)。
		if (m_OpenProjectBrowsePending)
		{
			m_OpenProjectBrowsePending = false;
			RunOpenProjectBrowse(ctx);
		}
		// PROJ-3/T1:纯启动器模式 —— 窗口只有一页启动器;菜单栏 / 挂靠栏 / 停靠区 /
		// 状态栏 / 独立窗口全部不画(无障碍树里也就只有启动器页的节点)。
		if (m_LauncherMode)
		{
			RenderLauncherPage(ctx);
			return;
		}
		// M4-TEX P4:内容浏览器(双击 `.wtex` / 右键 Texture Settings…/Reimport/Reset)在渲染期间
		// 只能写"打开纹理设置"的请求 —— 可见性/布局不能在面板渲染中途改。这里在帧边界统一执行,
		// 与 Window 菜单、AI 通道 `ui.open` 落到同一条开关路径(AiActivatePanel)。
		{
			std::string textureAsset;
			Editor::TextureSettingsRequests::Kind textureKind = Editor::TextureSettingsRequests::Kind::Open;
			while (Editor::TextureSettingsRequests::Get().Take(textureAsset, textureKind))
			{
				const auto found = m_PanelRegistry.find("texture_settings");
				if (found == m_PanelRegistry.end() || !found->second)
					continue;
				static_cast<TextureSettingsPanel*>(found->second.get())->RequestOpenAsset(textureAsset,
					textureKind == Editor::TextureSettingsRequests::Kind::ResetDefaults);
				AiActivatePanel("texture_settings", nullptr);
			}
		}
		// AI 无障碍树:主窗口这一帧的节点从这里开始重新登记(见 WuiAccessibility)。
		Wui::WuiAccessibility::Get().BeginFrame("main", ctx.ViewportSize());
		// U25-M2:跨窗口拖放桥的落点登记每帧重建(独立窗口在本帧稍后登记自己).
		Editor::AssetDropBridge::Get().BeginFrame(ctx.Frame());
		// W9 review:文本焦点登记用宿主显式身份,不依赖无障碍开关。
		ctx.SetWindowKey("main");
		m_ViewportRect = {};
		ctx.ClearDropTarget();
		m_DropPreviewActive = false;
		// W9-2:Ctrl+Z/Y 与脚本编辑器的 buffer 撤销/重做冲突 —— 文本焦点活跃(上一帧快照)
		// 或焦点面板是脚本编辑器时,场景撤销/重做让位(WuiCodeEditor 自己消费这组键)。
		EditorPanel* const focusPanelAtFrameStart = FocusedPanel();
		const bool scriptEditorFocused = focusPanelAtFrameStart
			&& std::strncmp(focusPanelAtFrameStart->Id(), kScriptPanelPrefix, std::strlen(kScriptPanelPrefix)) == 0;
		const bool sceneUndoAllowed = !m_TextFocusLatched && !scriptEditorFocused;
		const bool undoKey = sceneUndoAllowed && ctx.Input().Ctrl && !ctx.Input().Shift
			&& ctx.IsKeyPressed(KeyCodes::Z);
		const bool redoKey = sceneUndoAllowed && ctx.Input().Ctrl
			&& (ctx.IsKeyPressed(KeyCodes::Y) || (ctx.Input().Shift && ctx.IsKeyPressed(KeyCodes::Z)));
		if (undoKey)
		{
			if (ctx.History().Undo())
				ctx.RecordOp("undo", "undo", ctx.History().UndoName(), "");
		}
		else if (redoKey)
		{
			if (ctx.History().Redo())
				ctx.RecordOp("undo", "redo", ctx.History().RedoName(), "");
		}
		const glm::vec2 viewport = ctx.ViewportSize();
		// D10-11(用户 2026-09-19"这种窗口也该抽象出来"):导入位置是**窗口级模态** ——
		// 开帧用 WuiModal 的输入封锁把整个客户区登记成遮挡区,后面画的所有面板照常显示但
		// 收不到命中(点击/悬停/拖放都走 HitTest);画模态本体之前 EndModalInputBlock 解开。
		// D10-15:shell 的四个模态(未保存/错误/打包/项目设置)同样是真模态,同一口径挡输入。
		// P4-UX10:启动询问是浮在停靠区上的**非模态**通知 —— 帧初先登记它的遮挡,
		// 下面的面板才不会吃到落在提示上的点击(提示内控件在绘制时仍然可命中)。
		if (!m_PendingFloatRestore.empty())
		{
			const float statusBarHeight = 22.0f;
			const Wui::WuiRect statusBarRect { 0, viewport.y - statusBarHeight, viewport.x, statusBarHeight };
			ctx.PushHoverBlocker(RestorePromptRect(statusBarRect));
		}
		const bool shellModalOpen = m_ImportModalOpen || m_Editor.ShowUnsavedModal()
			|| m_Editor.ShowErrorModal() || m_Editor.ShowCookingProgress()
			// P4-U13e:prefab 未保存改动的确认(关窗 / 进文档会话)也是窗口级模态。
			|| m_PrefabPendingAction != PrefabPendingAction::None
			// CPPT-6-ED-NEWSCRIPT:"新建 C++ 组件"模态同样封锁下层命中。
			|| m_NewCppScriptOpen
			// PECS-T9:"新建 Lua 系统"模态同样封锁下层命中。
			|| m_NewLuaSystemOpen
			// PROJ-1/T1:"新建项目"模态(名称/位置/模板/落盘)同样封锁下层命中。
			|| m_NewProjectOpen
			// PLUG-AUTH-1:"新建插件"模态(目标/模板/名称/ID/落盘)同样封锁下层命中。
			|| m_NewPluginOpen
			// PROJ-2/T1:项目启动器(启动态窗口级模态)同样封锁下层命中。
			|| m_Editor.ShowProjectLauncher();
		// P4-U6b:面板级模态(属性面板的"添加组件"居中窗口)与 shell 模态同一条封锁路径;
		// 渲染该面板之前会解开(RenderTabs),画完再封回去。
		const bool panelModalOpen = !m_PanelModalOwner.empty();
		if (shellModalOpen || panelModalOpen)
			Wui::BeginModalInputBlock(ctx);
		// 编辑器级四边停靠区:拖拽面板进入窗口边缘条带时,生成横跨整个编辑器的
		// 停靠区(而不是只切分鼠标所在的面板组)。需在渲染面板前判定,以便
		// RenderTabs 跳过面板内的落区逻辑。
		// P4-U13:prefab 编辑会话期间多一行横幅(在菜单栏下面),停靠区整体让出这一行。
		const bool prefabEditActive = m_Editor.IsEditingPrefab();
		const float prefabBarHeight = prefabEditActive ? kPrefabBarHeight : 0.0f;
		const float editorTop = 26.0f + m_AttachBarHeight + prefabBarHeight;
		// P4-UX6:底部留出状态栏(场景/选择/后端/帧率)——面板不再压到它上面。
		constexpr float statusBarHeight = 22.0f;
		const Wui::WuiRect statusBar { 0, viewport.y - statusBarHeight, viewport.x, statusBarHeight };
		const Wui::WuiRect editorArea { 0, editorTop, viewport.x,
			viewport.y - editorTop - statusBarHeight };
		std::string edgePayload;
		const bool edgeDragActive = ctx.IsDragActive(&edgePayload) && edgePayload.rfind("panel:", 0) == 0;
		// 独立窗口不参与四边停靠:它只能挂靠到顶部挂靠栏。
		const bool edgeDragIndependent = edgeDragActive
			&& IsIndependentPanel(edgePayload.substr(6));
		if (edgeDragActive && !edgeDragIndependent)
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
			if (ctx.IsHovered(editorArea))
			{
				// 四边停靠判定带(用户 2026-09-16:110px 太大,压到面板中部)。
				constexpr float edgeBand = 64.0f;
				const glm::vec2 mouse = ctx.Input().MousePos;
				if (mouse.x - editorArea.X <= edgeBand) m_EdgeDropZone = Wui::DropZone::Left;
				else if (editorArea.X + editorArea.W - mouse.x <= edgeBand) m_EdgeDropZone = Wui::DropZone::Right;
				else if (mouse.y - editorArea.Y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Top;
				else if (editorArea.Y + editorArea.H - mouse.y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Bottom;
				m_EdgeDockActive = m_EdgeDropZone != Wui::DropZone::Center;
				if (m_EdgeDockActive)
				{
					ctx.DropTarget(editorArea, "panel:");
					// 原面板落区状态让位,避免同时高亮/同时落位。
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
				}
			}
		}
		// 拖拽结束的落位在下一帧消费,因此拖拽不活跃时保留上一帧的边缘落区状态。
		// 窗口内浮动面板(停靠形态)画在停靠区之上,命中也要优先:先登记它们的矩形为
		// 遮挡区,下面的面板就不会同时响应;正在拖动的那一个除外(它跟随光标,且必须
		// 让下方的停靠落点能被命中)。
		for (const Wui::DockFloat& entry : m_Layout.Floating)
			if (!IsIndependentPanel(entry.Panel) && entry.Panel != m_MovingFloat)
				ctx.PushHoverBlocker(entry.Rect);
		// 已附加独立窗口时,主窗口作为"切换容器":当前标签是它,就显示它的内容。
		if (!m_ActiveWindowTag.empty())
		{
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), m_ActiveWindowTag);
			if (attached != m_AttachedPanels.end())
			{
				// M4-TEX P4(2026-09-25 补,口径同 P4-U6b):面板级模态的拥有者**不管渲染在停靠组里还是
				// 附加视图里**都必须能命中自己的模态 —— 画它之前解整窗输入封锁,画完封回去。
				// 此前只有 RenderTabs(停靠路径)做了这件事:附加视图里的面板模态的按钮
				// 全部落在封锁区里,点不动(实测:纹理设置的"回到默认设置"确认框)。
				const bool ownsPanelModal =
					!m_PanelModalOwner.empty() && m_PanelModalOwner == m_ActiveWindowTag;
				if (ownsPanelModal)
					Wui::EndModalInputBlock(ctx);
				RenderPanelContent(ctx, m_ActiveWindowTag, editorArea);
				if (ownsPanelModal)
					Wui::BeginModalInputBlock(ctx);
			}
			else
				m_ActiveWindowTag.clear();
		}
		if (m_ActiveWindowTag.empty())
			RenderNode(ctx, m_Layout.Root, editorArea);

		// 边缘落位提示必须在面板之后绘制:面板背景是不透明矩形,
		// 先画会被完全遮住(表现就是"没有目标位置提示")。
		if (m_EdgeDockActive)
		{
			Wui::WuiRect zone = editorArea;
			if (m_EdgeDropZone == Wui::DropZone::Left) zone.W = editorArea.W * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Right) { zone.X = editorArea.X + editorArea.W * 0.75f; zone.W = editorArea.W * 0.25f; }
			else if (m_EdgeDropZone == Wui::DropZone::Top) zone.H = editorArea.H * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Bottom) { zone.Y = editorArea.Y + editorArea.H * 0.75f; zone.H = editorArea.H * 0.25f; }
			// 预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
		}
		// 下层绘制结束:解除遮挡,浮动面板/菜单/弹窗仍按真实光标命中。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);   // 与上面的 BeginModalInputBlock 成对
		else
			ctx.ClearHoverBlockers();       // 无模态:清掉窗口内浮动面板的遮挡区(原行为)
		// 模态之下还有浮动面板与菜单栏要画,继续挡住它们的命中(它们照常显示,
		// 但点不到);到画模态前再解除(见 DrawModals 前后)。
		if (shellModalOpen)
			Wui::BeginModalInputBlock(ctx);

		std::string payload;
		bool dropConsumed = false;
		if (ctx.AcceptDrop(&payload, "panel:"))
		{
			dropConsumed = true;
			if (payload.rfind("panel:", 0) == 0)
			{
				const std::string panel = payload.substr(6);
				const bool floating = m_Layout.IsFloating(panel);
				// 独立窗口只能挂靠到顶部挂靠栏:落到停靠区/面板一律忽略,窗口留在原处。
				if (IsIndependentPanel(panel))
				{
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
					m_EdgeDockActive = false;
					m_EdgeDropZone = Wui::DropZone::Center;
					m_MovingFloat.clear();
				}
				else if (m_EdgeDockActive && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloatingToRoot(panel, m_EdgeDropZone)
						: m_Layout.DockToRoot(panel, m_EdgeDropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> editor/" + ZoneName(m_EdgeDropZone), before);
				}
				else if (!m_DropTargetPanel.empty() && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloating(panel, m_DropTargetPanel, m_DropZone)
						: m_Layout.MoveTab(panel, m_DropTargetPanel, m_DropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> " + m_DropTargetPanel + "/" + ZoneName(m_DropZone), before);
				}
			}
			// 只在落位消费后清空,避免下一帧 AcceptDrop 读取时目标已被清。
			// P3-1①:落点是拖拽的**正常出口**,这里一次性把外壳与 WuiContext 的拖拽
			// 状态全部归零(payload 刚被 AcceptDrop 消费,清掉不会丢落点)。
			EndPanelDrag(ctx);
		}
		// 拖拽结束且本帧未消费落点时,清除四边高亮:
		// 否则四边预览框会残留,表现成"启动/平时自动出现一个框"。
		if (!ctx.IsDragActive(nullptr))
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
		}

		// ---- 面板拖拽状态机:停靠面板一旦进入拖拽即"拖出"为浮动窗口 ----
		// 拖到落点上释放会重新停靠(DockFloating*),否则保持浮动并跟随鼠标。
		// P3-1①:状态机的四个出口(落点消费 / 松手未落点 / Esc 取消 / 左键已抬)都必须
		// 让拖拽状态归零 —— 否则残留的 m_TabDragPanel/m_DragPanel 会在面板**回到停靠树**
		// 的那一帧再次执行拖出分支(实测表现:浮窗刚被 ✕ 关掉又跳回来,第一次点 ✕ 被吃掉)。
		// 左键**物理**状态是不依赖事件送达的判据:释放落在别的窗口/窗口外时,本窗口的
		// 释放事件可能永远不到(与 WuiInputCollector::SyncButtonsWithSystem、
		// FloatWindowHost::Render 同一口径)。
		const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		{
			// Esc = 取消(左键可能还按着):一次把 WuiContext 与外壳状态都归零,
			// 之后即使按键还按着,也不会因为"按住即报"的 DragStart 被重新武装(见 RenderTabs)。
			std::string escPayload;
			const bool panelDragInFlight = !m_DragPanel.empty()
				|| (ctx.IsDragActive(&escPayload) && escPayload.rfind("panel:", 0) == 0);
			if (panelDragInFlight && ctx.WasKeyPressed(KeyCodes::Escape))
				EndPanelDrag(ctx);
		}
		std::string activePayload;
		if (leftButtonDown && ctx.IsDragActive(&activePayload) && activePayload.rfind("panel:", 0) == 0)
		{
			m_DragPanel = activePayload.substr(6);
			m_LastDragPos = ctx.Input().MousePos;
			// 只有"本次拖拽确实起手于该面板的标签页"时才允许拖出为独立窗口;
			// 否则残留的拖拽状态会在挂靠后立刻把面板再次浮出(表现为多出一个窗口)。
			if (!dropConsumed && m_AttachCooldownFrames <= 0 && m_Layout.Contains(m_DragPanel)
				&& m_TabDragPanel == m_DragPanel)
			{
				// T03 修订(用户 2026-09-16):子面板(停靠形态)拖出只在**主窗口内浮动**,
				// 不创建 OS 窗口 —— 独立 OS 窗口只属于声明为 Independent 的面板
				// (Widget Gallery / Input Map)。因此这里全部用主窗口客户区坐标。
				Wui::WuiRect source { m_LastDragPos.x - 40.0f, m_LastDragPos.y - 12.0f, 480.0f, 320.0f };
				if (const auto remembered = m_LastFloatRects.find(m_DragPanel); remembered != m_LastFloatRects.end())
				{
					source.W = remembered->second.W;
					source.H = remembered->second.H;
				}
				else
				{
					// 首次拖出:沿用原停靠区的尺寸作为初始浮动尺寸。
					std::vector<std::pair<Wui::PanelId, Wui::WuiRect>> rects;
					m_Layout.ComputeRects({ 0, 26, viewport.x, viewport.y - 26 }, &rects);
					for (const auto& entry : rects)
					{
						if (entry.first != m_DragPanel)
							continue;
						source.W = std::max(360.0f, entry.second.W * 0.75f);
						source.H = std::max(260.0f, entry.second.H * 0.75f);
						break;
					}
				}
				// 初始位置夹在编辑区里,标题栏必须可见(与 RenderFloatWindow 的约束一致)。
				source.W = std::min(source.W, std::max(320.0f, viewport.x - 16.0f));
				source.H = std::min(source.H, std::max(240.0f, viewport.y - editorTop - 16.0f));
				source.X = std::max(8.0f, std::min(source.X, std::max(8.0f, viewport.x - source.W - 8.0f)));
				source.Y = std::max(editorTop, std::min(source.Y, std::max(editorTop, viewport.y - source.H - 8.0f)));
				const std::string before = m_Layout.Serialize();
				if (m_Layout.Float(m_DragPanel, source))
				{
					RecordDockChange(ctx, "float", m_DragPanel, before);
					// 拖动期间面板跟随光标:抓取点固定在标题栏左侧(贴近原标签位置)。
					m_MovingFloat = m_DragPanel;
					m_FloatGrabOffset = { 40.0f, 12.0f };
					m_LastFloatRects[m_DragPanel] = source;
					// P3-1①:拖出已经完成,消费掉"本次拖拽的起手标签"标记 —— 同一个拖拽里
					// 面板若又回到停靠树(例如点浮窗 ✕ 收回停靠位),不允许再浮出一次。
					m_TabDragPanel.clear();
				}
				else if (std::getenv("WLD_TRACE_UI"))
				{
					WLD_CORE_WARN("[float] drag-out float() rejected for '{0}'", m_DragPanel);
				}
			}
			else if (std::getenv("WLD_TRACE_UI"))
			{
				// 诊断:拖拽已激活但没有走拖出分支(供脚本/人工排查用,按面板去重打印)。
				static std::string lastSkipped;
				if (lastSkipped != m_DragPanel)
				{
					lastSkipped = m_DragPanel;
					WLD_CORE_INFO("[float] drag-out skipped: panel={0} consumed={1} cooldown={2} contained={3} tabDrag={4}",
						m_DragPanel, dropConsumed ? 1 : 0, m_AttachCooldownFrames,
						m_Layout.Contains(m_DragPanel) ? 1 : 0, m_TabDragPanel);
				}
			}
			else if (!m_DragPanel.empty() && m_Layout.Contains(m_DragPanel))
			{
				// 拖出条件未满足:保持原状(用于人工排查,不打印高频日志)。
			}
		}
		else if (leftButtonDown && ctx.IsDragActive(nullptr))
		{
			// 其他类型的拖拽(file: 等)不参与面板浮动。
			m_DragPanel.clear();
			m_TabDragPanel.clear();
		}
		// P3-1① 修订(用户 2026-09-20 报"子栏不能拖拽了"):出口收口只在**物理左键已经抬起**
		// 时执行。WuiContext 的拖拽是两段式:按下沿的 BeginDrag 只进入 pending(记下
		// m_DragPressPos),移动超过 4px 后(WuiContext::EndFrame / 下一次 BeginDrag)才置
		// m_Dragging —— 期间 IsDragActive() 恒为 false。而 m_TabDragPanel / m_MovingFloat 在
		// **按下沿当帧**就已经写入;若这时按"拖拽结束"收口,ClearPanelDragIdentity() 会清掉
		// 起手标记、ctx.EndDrag() 会直接撤销 pending —— 面板再也进不了拖拽(实测表现:按住
		// 标签拖动,面板完全不动)。pending 期间这些身份成员是拖拽必需的,所以收口条件加上
		// !leftButtonDown:松手 / 释放事件丢失(由 WuiInputCollector::SyncButtonsWithSystem
		// 补发释放)时左键已抬起,收口照常发生,P3-1 的"拖出后一次点 ✕ 就关闭"不受影响。
		else if (!leftButtonDown
			&& (!m_DragPanel.empty() || !m_MovingFloat.empty() || !m_TabDragPanel.empty()))
		{
			// 拖拽结束(松手 / 释放事件丢失):清"拖拽身份"成员 —— 拖拽起点标记残留会让
			// 已经收回停靠的面板立刻再次浮出。**落点成员必须留到下一帧**:WuiContext 下一帧
			// 才交付 AcceptDrop,落位用的就是这一帧武装好的目标;这里清掉会把落点整个吃掉。
			ClearPanelDragIdentity();
			// ctx 侧的拖拽结束由 WuiContext::EndFrame 负责:它要把"本帧已武装的落点"转成
			// 可消费的 m_DropAccepted(见 WuiContext.cpp 的 release 分支)。在"释放沿"这一帧
			// 调 EndDrag 会把这次落点整个吃掉(拖回停靠位失效),所以只在 ctx 已经不在拖拽时
			// 清残留 payload。
			if (!ctx.IsDragActive(nullptr))
				ctx.EndDrag();
		}

		// 浮动面板绘制在停靠区之上、菜单/模态之下。
		RenderFloating(ctx);

		// 菜单栏最后绘制:其弹出面板需要盖在所有停靠面板之上。
		DrawMenuBar(ctx);
		// 挂靠栏:横条形式(排在菜单栏下方),独立窗口可挂靠至此。
		DrawAttachBar(ctx);
		// P4-U13:prefab 编辑横幅(菜单栏之下、停靠区之上;激活时才存在这一行)。
		if (prefabEditActive)
			DrawPrefabBar(ctx, 26.0f + m_AttachBarHeight);

		// D10-11/D10-15:模态绘制前解除遮挡(对话框自身要能命中);画完再清一次,
		// 确保本帧结束时不留 blocker(下一帧开帧也会清,这里是双保险)。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);
		else
			ctx.ClearHoverBlockers();
		DrawModals(ctx);
		ctx.ClearHoverBlockers();

		std::string dragPayload;
		if (ctx.IsDragActive(&dragPayload))
		{
			std::string label = dragPayload;
			if (dragPayload.rfind("panel:", 0) == 0) label = "停靠面板: " + dragPayload.substr(6);
			else if (dragPayload.rfind("file:", 0) == 0) label = "移动文件: " + dragPayload.substr(5);
			ctx.PushOverlay();
			Label(ctx, ctx.Input().MousePos + glm::vec2 { 14, 14 }, label, m_Theme.Text, 13.0f);
			ctx.PopOverlay();
		}

		// 遍历结束后再执行标签关闭请求:这样"关闭组内最后一个标签"引起的塌缩
		// 不会打断正在进行的渲染遍历(修"关闭 Saves/Levels 等标签崩溃")。
		for (const Wui::PanelId& panel : m_PendingPanelCloses)
		{
			const std::string before = m_Layout.Serialize();
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "close", panel, before);
		}
		m_PendingPanelCloses.clear();
		// PLUG-T3b:插件卸载后把对应面板的窗口/标签/布局记录收干净(注册表已没有它)。
		ClosePluginPanelsNotInRegistry();

		// P3-1①:挂靠标签拖拽(不经过 WuiContext)的收口 —— 左键已经抬起,而
		// DrawAttachBar 这一帧没收到释放沿(释放落在别的窗口/窗口外)时,残留会让顶栏
		// 一直停在"正在拖标签"的状态。正常路径上 DrawAttachBar 已消费并清空,这里不会再触发;
		// 只收口"已经在拖"的状态:m_AttachTagPress 要留给 DrawAttachBar 的"单击=切换视图"
		// 释放沿消费(脚本注入的点击没有物理按键,不能在这里被判成残留)。
		if (!leftButtonDown && !m_AttachTagDrag.empty())
		{
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}

		// W9-2:本帧(含所有独立窗口)结束时的文本焦点快照。UI 帧开始时各窗口的登记
		// 已被 BeginFrame 清空,所以帧内 Ctrl+Z/Y 判定必须用"上一帧结束"的这份状态。
		// P4-UX4:悬停提示最后画 —— 面板/模态都已登记完,这里统一画到 overlay 层。
		DrawStatusBar(ctx, statusBar);
		// P4-UX10:启动询问贴状态栏上方(非模态;不压暗、不挡操作)。
		DrawRestorePrompt(ctx, statusBar);
		// M4-S2:菜单栏下拉的延后批次在**模态之后**补画(它们与 U29 的下拉弹层同属"永远在最上层"
		// 的那一类);tooltip 仍在最后画,所以提示不会被菜单压住。
		FlushDeferredMenuDraws(ctx);
		Wui::DrawTooltip(ctx, m_Theme);
		// U26:独立 OS 窗口放在**最后**渲染 —— 它们会把无障碍树的"当前窗口"切到自己,
		// 放在前面会让菜单栏/状态栏等主窗口节点挂错窗口键(见 RenderIndependentWindows)。
		RenderIndependentWindows(ctx);
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}


	// PROJ-3/T1:纯启动器模式的整页渲染。与普通编辑器**共用同一批模态绘制函数**
	// (DrawModals:新建项目向导 / 项目启动器 / 错误框),但不画菜单栏、挂靠栏、停靠区、
	// 状态栏与独立窗口 —— 启动器页就是窗口的全部内容;停靠布局在构造期就没有装载,
	// 也不会有任何"顺手保存布局"的路径(见 SaveLayout 的启动器模式直接返回)。
void EditorShell::RenderLauncherPage(Wui::WuiContext& ctx){
		// 无障碍帧照常开始:启动器节点就是 a11y 树的全部内容(AI 通道 ui.tree 读的是它)。
		Wui::WuiAccessibility::Get().BeginFrame("main", ctx.ViewportSize());
		Editor::AssetDropBridge::Get().BeginFrame(ctx.Frame());
		ctx.SetWindowKey("main");
		ctx.ClearDropTarget();

		DrawModals(ctx);
		ctx.ClearHoverBlockers();

		// 状态提示(打开项目失败 / Runtime 缺失等)在启动器页也要看得见:用窗口底部
		// 一条当它的落点(与普通路径的状态栏同一份实现,只是这里没有真状态栏)。
		const glm::vec2 viewport = ctx.ViewportSize();
		DrawStatusNotice(ctx, { 0.0f, viewport.y - 22.0f, viewport.x, 22.0f });
		Wui::DrawTooltip(ctx, m_Theme);

		// 文本焦点快照与普通路径同源(下一个 UI 帧的输入判定要用)。
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}


	// P4-UX6:状态栏 —— 一眼看到"当前场景有没有改、选中了什么、跑在哪个后端、多少帧"。
	// 全部取自既有状态(文档/选择/Input().FPS),不做任何额外计算或分配。
	// M4-S2 遗留②:菜单栏下拉的延后收口。调用点在**所有面板与模态之后、tooltip 之前**
	// (OnUiFrame 的帧末)。没有菜单打开时批次为空,零成本。
void EditorShell::FlushDeferredMenuDraws(Wui::WuiContext& ctx){
		if (m_DeferredMenuCommands.empty())
			return;
		ctx.PushOverlay();
		std::vector<Wui::WuiDrawCommand>& target = ctx.Commands();
		for (const Wui::WuiDrawCommand& command : m_DeferredMenuCommands)
			target.push_back(command);
		ctx.PopOverlay();
		m_DeferredMenuCommands.clear();
	}


void EditorShell::DrawStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect){
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelHeader, 0.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { rect.X, rect.Y, rect.W, 1.0f },
			m_Theme.Border, 0.0f });

		EditorDocument& document = m_Editor.GetDocument();
		const std::string sceneName = document.HasPath()
			? document.GetPath().filename().string() : std::string("Untitled");
		std::string left = std::string(Wui::Tr("status.scene", "Scene")) + ": " + sceneName;
		if (document.IsDirty())
			left += " *";
		Entity selected = GetSelectedEntity();
		left += "   |   " + std::string(Wui::Tr("status.selection", "Selection")) + ": ";
		if (selected && selected.HasComponent<TagComponent>())
			left += selected.GetComponent<TagComponent>().Tag;
		else
			left += Wui::Tr("status.selection.none", "none");
		if (!m_ActiveWindowTag.empty())
			left += "   |   " + std::string(Wui::Tr("status.view", "View")) + ": " + PanelTitle(m_ActiveWindowTag);

		const float fps = ctx.Input().FPS;
		char right[96] = {};
		std::snprintf(right, sizeof(right), "%s   %.1f FPS (%.1f ms)", Renderer::GetBackendName().c_str(),
			static_cast<double>(fps), fps > 0.0f ? 1000.0 / static_cast<double>(fps) : 0.0);
		const std::string rightText = right;
		const float rightWidth = ctx.MeasureTextWidth(rightText, 12.0f);
		const float textY = rect.Y + (rect.H - 14.0f) * 0.5f;
		Wui::Label(ctx, { rect.X + 10.0f, textY }, left, m_Theme.TextMuted, 12.0f);

		// ---- CPPT-3:Game 模块热重载状态(loaded / unloaded / reloading / rolled-back)----
		// 常驻显示:模块未加载窗口(启动失败 / module.unload / reloading)必须显式提示 ——
		// 该窗口里 Lua 存根生成被拒绝、C++ 组件下拉为空,用户要能一眼看出"是模块没加载"。
		// 无障碍:`cppmodule.status`(kind=status,value = 稳定字面量)供 AI/探针断言;
		// 发生过模块动作后追加 `cppmodule.result` 反馈节点(module.reload 的 ok/回滚/计数)。
		{
			const EditorLayer::CppModuleStatus& module = m_Editor.GetCppModuleStatus();
			const std::string moduleTitle = std::string(Wui::Tr("status.cppmodule", "C++ module")) + ": "
				+ CppModuleStateText();
			const std::string moduleHint = CppModuleHintText();
			std::string chipText = moduleHint.empty() ? moduleTitle : (moduleTitle + " — " + moduleHint);
			const float leftWidth = ctx.MeasureTextWidth(left, 12.0f);
			const float rightReserve = rightWidth + 24.0f;
			float chipX = rect.X + 10.0f + leftWidth + 12.0f;
			float chipWidth = ctx.MeasureTextWidth(chipText, 12.0f) + 12.0f;
			// 空间不足(窄窗口 + 长场景名)时按字节截断,不盖住右侧后端/FPS 文本。
			const float limit = rect.X + rect.W - rightReserve;
			if (chipX + chipWidth > limit)
			{
				std::string cut = chipText;
				while (cut.size() > 8 && ctx.MeasureTextWidth(cut + "…", 12.0f) + 12.0f > limit - chipX)
				{
					cut.pop_back();
					while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0u) == 0x80u)
						cut.pop_back();
				}
				chipText = cut.empty() ? std::string() : (cut + "…");
				chipWidth = std::max(0.0f, limit - chipX);
			}
			const bool moduleLoaded = m_Editor.IsCppModuleLoaded();
			const Wui::WuiRect chip { chipX, rect.Y + 2.0f, std::max(0.0f, chipWidth), rect.H - 4.0f };
			if (!chipText.empty() && chip.W > 4.0f)
				Wui::Label(ctx, { chip.X + 6.0f, textY }, chipText,
					moduleLoaded ? m_Theme.TextMuted : m_Theme.Warning, 12.0f);
			// a11y Tooltip = 本地化提示 + 引擎消息 + 最近一次诊断(按行截断,避免超长)。
			std::string moduleTooltip = CppModuleHintText();
			if (!module.Message.empty())
				moduleTooltip += (moduleTooltip.empty() ? "" : "\n") + module.Message;
			size_t shown = 0;
			for (const std::string& diagnostic : module.Diagnostics)
			{
				// CPPT-3-FIX1:迁移诊断(类型变化 / 字段删除 / 新增字段)逐条进 tooltip;
				// 上限 10 条避免无限长,超出只报剩余条数。
				if (shown >= 10)
					break;
				++shown;
				moduleTooltip += "\n" + diagnostic;
			}
			if (module.Diagnostics.size() > shown)
			{
				moduleTooltip += "\n" + std::to_string(module.Diagnostics.size() - shown) + " "
					+ Wui::Tr("status.cppmodule.diagnostics.more",
						"more migration diagnostics (see the AI channel module.status)");
			}
			Wui::WuiAccessNode moduleNode;
			moduleNode.Id = Wui::HashId("cppmodule.status");
			moduleNode.Window = "main";
			moduleNode.Panel = "shell";
			moduleNode.Kind = "status";
			moduleNode.Label = Wui::Tr("status.cppmodule.label", "C++ module status");
			moduleNode.Value = m_Editor.CppModuleStateName();
			moduleNode.Tooltip = moduleTooltip;
			moduleNode.Rect = chip;
			moduleNode.Enabled = false;
			moduleNode.Interactive = false;
			moduleNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(moduleNode);
			// 反馈节点:只在发生过模块动作后出现(module.reload / module.unload 的最终结果)。
			if (module.Sequence > 0)
			{
				std::string summary = std::string("ok=") + (module.Ok ? "true" : "false")
					+ " rolledBack=" + (module.RolledBack ? "true" : "false")
					+ " abi=" + std::to_string(module.AbiVersion)
					+ " drained=" + std::to_string(module.InstancesDrained)
					+ " restored=" + std::to_string(module.InstancesRestored)
					+ " diagnostics=" + std::to_string(module.Diagnostics.size());
				Wui::WuiAccessNode resultNode;
				resultNode.Id = Wui::HashId("cppmodule.result");
				resultNode.Window = "main";
				resultNode.Panel = "shell";
				resultNode.Kind = "status";
				resultNode.Label = Wui::Tr("status.cppmodule.result", "C++ module reload result");
				resultNode.Value = summary;
				resultNode.Tooltip = moduleTooltip;
				resultNode.Rect = chip;
				resultNode.Enabled = false;
				resultNode.Interactive = false;
				resultNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(resultNode);
			}
		}
		Wui::Label(ctx, { rect.X + rect.W - rightWidth - 10.0f, textY }, rightText,
			m_Theme.TextMuted, 12.0f);
		// 无障碍:脚本/AI 通道可直接读整条状态(不依赖像素)。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.status");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "status";
		node.Label = "editor status bar";
		node.Value = left + "   |   " + rightText;
		node.Rect = rect;
		node.Enabled = false;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
		DrawStatusNotice(ctx, rect);
	}


	// CPPT-3:Game 模块状态显示文案(稳定字面量 → 本地化显示;`rolled-back` 用连字符口径)。
std::string EditorShell::CppModuleStateText() const{
		switch (m_Editor.GetCppModuleStatus().State)
		{
			case EditorLayer::CppModuleState::Loaded: return Wui::Tr("status.cppmodule.loaded", "loaded");
			case EditorLayer::CppModuleState::Unloaded: return Wui::Tr("status.cppmodule.unloaded", "unloaded");
			case EditorLayer::CppModuleState::Reloading: return Wui::Tr("status.cppmodule.reloading", "reloading");
			case EditorLayer::CppModuleState::RolledBack: return Wui::Tr("status.cppmodule.rolled_back", "rolled-back");
		}
		return Wui::Tr("status.cppmodule.unloaded", "unloaded");
	}


	// 未加载窗口的显式提示(unloaded / reloading 都提示"先重建、再重载");loaded = 无提示。
std::string EditorShell::CppModuleHintText() const{
		switch (m_Editor.GetCppModuleStatus().State)
		{
			case EditorLayer::CppModuleState::Unloaded:
				return Wui::Tr("status.cppmodule.hint.unloaded",
					"Game.dll is not loaded — use File ▶ Build & Reload C++ Module");
			case EditorLayer::CppModuleState::Reloading:
				return Wui::Tr("status.cppmodule.hint.reloading",
					"Game.dll unloaded for rebuild — use File ▶ Build & Reload C++ Module to build and load it");
			case EditorLayer::CppModuleState::RolledBack:
				return Wui::Tr("status.cppmodule.hint.rolled_back",
					"the new build was rejected; the previous Game.dll is loaded again (see diagnostics)");
			case EditorLayer::CppModuleState::Loaded:
			{
				// CPPT-3-FIX1:加载成功但发生了属性迁移(类型变化 / 字段删除 / 新增字段)时,
				// 状态栏常驻显示诊断条数(逐条文本在 tooltip / AI module.status / 加载提示里)。
				const size_t diagnostics = m_Editor.GetCppModuleStatus().Diagnostics.size();
				if (diagnostics > 0)
					return std::to_string(diagnostics) + " " + Wui::Tr("status.cppmodule.hint.diagnostics",
						"field migration diagnostics — hover here for the details");
				break;
			}
		}
		return std::string();
	}


	// CPPT-3-FIX1:模块动作的可见反馈:菜单项与 AI `module.reload` 共用一条(此前只有菜单单独
	// Notify,AI 通道没有提示;诊断条数/首条文本要能被用户看到,不能只藏在 a11y 节点里)。
void EditorShell::NotifyCppModuleResult(){
		const EditorLayer::CppModuleStatus& module = m_Editor.GetCppModuleStatus();
		std::string text = CppModuleStateText();
		if (!module.Message.empty())
			text += " — " + module.Message;
		if (!module.Diagnostics.empty())
		{
			// 诊断文本是引擎的英文 canonical 口径(含实体/字段名),不翻译;只翻译前缀。
			text += " — " + std::to_string(module.Diagnostics.size()) + " "
				+ Wui::Tr("notice.cppmodule.diagnostics", "field migration diagnostics") + ": "
				+ module.Diagnostics.front();
		}
		Notify(text);
	}


void EditorShell::RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area){
		if (node.IsTabs())
			RenderTabs(ctx, node, area);
		else
			RenderSplit(ctx, node, area);
	}


	// P4-UX10:状态栏提示(人类交互细节都是刻意的,别当装饰):
	//   · 鼠标停在提示上 = **暂停倒计时**,不会读到一半消失(用户明确要求);
	//   · 鼠标移开 = 再给 1.2s 宽限,然后 0.25s 淡出(即使悬停超过 4s 再移开也一样);
	//   · 悬停时右侧出现 ×,点提示或按 Esc 立即关闭(可撤销/可控);
	//   · 悬停期间光标变手型,告诉用户"这块是可以点的"。
void EditorShell::DrawStatusNotice(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar){
		if (!m_Notice.Active || m_Notice.Text.empty())
			return;
		if (ctx.WasKeyPressed(KeyCodes::Escape))
		{
			m_Notice.Active = false;
			return;
		}
		const double now = ShellNowSeconds();
		const float textWidth = ctx.MeasureTextWidth(m_Notice.Text, 12.0f);
		const Wui::WuiRect chip { statusBar.X + statusBar.W - 210.0f - textWidth, statusBar.Y + 2.0f,
			textWidth + 30.0f, statusBar.H - 4.0f };
		const bool hovered = ctx.IsHovered(chip);
		if (hovered)
			ctx.SetCursor(Wui::WuiCursor::Hand);
		if (!m_Notice.Timing.Update(now, hovered, 4.0, 1.2, 0.25))
		{
			m_Notice.Active = false;
			return;
		}
		const float alpha = m_Notice.Timing.Alpha;

		const Wui::WuiColor background { m_Theme.ActiveBg.R, m_Theme.ActiveBg.G, m_Theme.ActiveBg.B,
			(hovered ? 0.95f : 0.75f) * alpha };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, chip, background, 3.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, chip,
			{m_Theme.Accent.R, m_Theme.Accent.G, m_Theme.Accent.B, alpha}, 3.0f, 1.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { chip.X + 10.0f, chip.Y + 3.0f, 0, 0 },
			{m_Theme.Text.R, m_Theme.Text.G, m_Theme.Text.B, alpha}, 0.0f, 1.0f, m_Notice.Text, 12.0f, false });
		if (hovered)
		{
			const Wui::WuiRect close { chip.X + chip.W - 18.0f, chip.Y + 2.0f, 14.0f, chip.H - 4.0f };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { close.X + 3.0f, close.Y + 2.0f, 0, 0 },
				m_Theme.TextMuted, 0.0f, 1.0f, "x", 12.0f, false });
			if (ctx.IsClicked(close) || ctx.IsClicked(chip))
			{
				m_Notice.Active = false;
				return;
			}
		}
		// 无障碍:脚本/读屏能读到这条提示,也能点它关掉。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.notice");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "notice";
		node.Label = Wui::Tr("notice.label", "Status notice");
		node.Value = m_Notice.Text;
		node.Rect = chip;
		node.Enabled = true;
		node.Interactive = true;
		node.Visible = true;
		node.Tooltip = Wui::Tr("notice.tooltip", "Click to dismiss (Esc). It stays while the pointer is on it.");
		Wui::WuiAccessibility::Get().Register(node);
	}


	// P4-UX10:启动询问 —— **非模态通知**,贴在状态栏正上方(与状态栏同一条右边界)。
	// 之前这里做成了模态对话框 + 全屏遮罩,用户反馈"怎么有个选项霸屏"(2026-09-20):
	// 询问必须让人能一边干活一边决定,所以改成通知条 —— 不压暗、不挡面板操作,
	// 只挡自己那一小块(登记 hover blocker);不回答就一直留着,回答/关闭即消失。
Wui::WuiRect EditorShell::RestorePromptRect(const Wui::WuiRect& statusBar) const{
		constexpr float width = 520.0f;
		constexpr float height = 84.0f;
		return { statusBar.X + statusBar.W - width - 12.0f, statusBar.Y - height - 8.0f, width, height };
	}


void EditorShell::DrawRestorePrompt(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar){
		if (m_PendingFloatRestore.empty())
			return;

		const Wui::WuiRect panel = RestorePromptRect(statusBar);
		const float height = panel.H;
		// 询问条也按时间收(用户 2026-09-20:"这个提示不会随时间消失"):停留 15s(要读要选,
		// 比状态栏提示的 4s 长得多),悬停暂停、移开给宽限、最后整条淡出。
		const double now = ShellNowSeconds();
		if (m_RestorePromptTiming.ShownAt <= 0.0)
			m_RestorePromptTiming.ShownAt = now;   // 首帧才开始计时(0 会被当成"已经过了很久")
		const bool promptHovered = ctx.IsHovered(panel);
		if (!m_RestorePromptTiming.Update(now, promptHovered, 15.0, 1.5, 0.3))
		{
			// 超时 = 本次不恢复(和 × / Esc 同义):记录保留,下次启动还会问。
			m_PendingFloatRestore.clear();
			m_RestorePromptTiming = TimedNotice {};
			PushNotice(Wui::Tr("notice.restore.skipped", "Skipped restoring last session's windows"));
			WLD_CORE_INFO("[float] restore prompt timed out (windows kept in layout for next start)");
			return;
		}
		const float alpha = m_RestorePromptTiming.Alpha;
		// 淡出阶段连控件一起淡:复制一份主题,把颜色 alpha 缩掉再交给按钮/勾选框。
		Wui::WuiTheme promptTheme = m_Theme;
		if (alpha < 0.999f)
		{
			auto fade = [alpha](Wui::WuiColor& color) { color.A *= alpha; };
			fade(promptTheme.PanelBg); fade(promptTheme.PanelHeader); fade(promptTheme.Border);
			fade(promptTheme.Text); fade(promptTheme.TextMuted); fade(promptTheme.TextDisabled);
			fade(promptTheme.Accent); fade(promptTheme.ButtonBg); fade(promptTheme.ButtonHover);
			fade(promptTheme.ActiveBg);
		}
		ctx.PushOverlay();
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, panel, promptTheme.PanelBg, 5.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, panel, promptTheme.Border, 5.0f, 1.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { panel.X, panel.Y, panel.W, 3.0f },
			promptTheme.Accent, 2.0f });

		const size_t count = m_PendingFloatRestore.size();
		std::string names;
		for (size_t i = 0; i < count && i < 3; ++i)
			names += std::string(i == 0 ? "" : ", ") + std::string(PanelTitle(m_PendingFloatRestore[i].Panels.front()));
		if (count > 3)
			names += Wui::Tr("modal.restore.more", " …");
		Wui::Label(ctx, { panel.X + 12.0f, panel.Y + 10.0f },
			Wui::Tr("modal.restore.body", "Last session left these windows open:"), promptTheme.Text, 13.0f);
		Wui::Label(ctx, { panel.X + 12.0f, panel.Y + 28.0f }, names, promptTheme.TextMuted, 12.0f);

		// × 关闭 = 本次不恢复(下次再问;想彻底不问就勾"记住")。
		const Wui::WuiRect closeRect { panel.X + panel.W - 20.0f, panel.Y + 6.0f, 14.0f, 14.0f };
		const bool closeHovered = ctx.IsHovered(closeRect);
		if (closeHovered)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, closeRect, promptTheme.ButtonHover, 2.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { closeRect.X + 3.0f, closeRect.Y - 1.0f, 0, 0 },
			closeHovered ? promptTheme.Text : promptTheme.TextMuted, 0.0f, 1.0f, "x", 12.0f, false });

		const float rowY = panel.Y + height - 28.0f;
		const bool tabsClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.tabs"),
			{ panel.X + 12.0f, rowY, 132.0f, 22.0f },
			Wui::Tr("modal.restore.tabs", "Restore as Tabs"), promptTheme);
		const bool layoutClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.layout"),
			{ panel.X + 150.0f, rowY, 140.0f, 22.0f },
			Wui::Tr("modal.restore.layout", "Restore Layout"), promptTheme);
		const bool noneClicked = Wui::Button(ctx, Wui::HashId("restore.prompt.none"),
			{ panel.X + 296.0f, rowY, 104.0f, 22.0f },
			Wui::Tr("modal.restore.none", "Don't Restore"), promptTheme);
		const Wui::LocalizedLabel remember = Wui::TrLabel("modal.restore.remember", "Remember");
		Wui::Checkbox(ctx, Wui::HashId("restore.prompt.remember"),
			{ panel.X + panel.W - 112.0f, rowY + 1.0f, 104.0f, 20.0f },
			remember.Text, remember.Term, m_RestoreAskRemember, promptTheme);

		const bool escape = ctx.WasKeyPressed(KeyCodes::Escape);
		const bool dismiss = closeHovered && ctx.Input().MouseClicked[0];
		ctx.PopOverlay();

		// 无障碍:整条询问登记成节点(动作按钮/勾选框由控件自己登记,脚本都能点)。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.restore.prompt");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "notice";
		node.Label = Wui::Tr("modal.restore.title", "Restore Independent Windows");
		node.Value = names;
		node.Rect = panel;
		node.Enabled = true;
		node.Interactive = false;
		node.Visible = true;
		node.Tooltip = Wui::Tr("modal.restore.hint",
			"Top-bar tabs keep the desktop clean and never steal focus; floating windows come back without focus too.");
		Wui::WuiAccessibility::Get().Register(node);

		if (!(tabsClicked || layoutClicked || noneClicked || dismiss || escape))
			return;

		std::vector<PendingFloatRestore> pending = std::move(m_PendingFloatRestore);
		m_PendingFloatRestore.clear();
		Editor::EditorPreferences& preferences = Editor::EditorPreferences::Get();
		if (tabsClicked)
		{
			RestoreIndependentWindows(pending, true);
			if (m_RestoreAskRemember)
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::Tabs);
		}
		else if (layoutClicked)
		{
			RestoreIndependentWindows(pending, false);
			if (m_RestoreAskRemember)
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::Layout);
		}
		else
		{
			// 不恢复 / × / Esc:本次跳过;勾了"记住"才清掉记录并停止再问。
			if (m_RestoreAskRemember)
			{
				preferences.SetRestoreWindows(Editor::RestoreWindowsMode::None);
				m_Layout.Floating.clear();
				SaveLayout();
			}
			PushNotice(Wui::Tr("notice.restore.skipped", "Skipped restoring last session's windows"));
			WLD_CORE_INFO("[float] restore declined ({0} windows kept in layout)", pending.size());
		}
	}


void EditorShell::RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area){
		const bool row = node.Direction == Wui::WuiDirection::Row;
		const size_t count = node.Children.size();
		if (count == 0)
			return;
		const float total = row ? area.W : area.H;
		float cursor = 0;
		for (size_t i = 0; i < count; ++i)
		{
			float size = 0;
			if (count == 1) size = total;
			else if (count == 2) size = i == 0 ? total * node.Ratio : total - total * node.Ratio;
			else size = total / static_cast<float>(count);

			Wui::WuiRect childArea = row
				? Wui::WuiRect { area.X + cursor, area.Y, size, area.H }
				: Wui::WuiRect { area.X, area.Y + cursor, area.W, size };
			RenderNode(ctx, node.Children[i], childArea);
			cursor += size;

			if (i + 1 < count)
			{
				// 分隔条走组件(Splitter):高亮/光标/命中统一。
				const Wui::WuiRect splitterArea = row
					? Wui::WuiRect { area.X + cursor - 2, area.Y, 4, area.H }
					: Wui::WuiRect { area.X, area.Y + cursor - 2, area.W, 4 };
				const Wui::SplitterResult split = Wui::Splitter(ctx, splitterArea, row, m_Theme, m_DragSplitNode == &node);
				if (split.Hovered && ctx.IsClicked(splitterArea))
				{
					m_SplitterDragging = true;
					m_DragSplitNode = &node;
					m_DragSplitRow = row;
					m_SplitterBeforeJson = m_Layout.Serialize();
				}
			}
		}

		if (m_DragSplitNode == &node && m_SplitterDragging)
		{
			const float mouse = row ? ctx.Input().MousePos.x - area.X : ctx.Input().MousePos.y - area.Y;
			node.Ratio = std::max(0.05f, std::min(0.95f, mouse / std::max(1.0f, total)));
			if (ctx.Input().MouseReleased[0])
			{
				m_SplitterDragging = false;
				m_DragSplitNode = nullptr;
				RecordDockChange(ctx, "resize", "", m_SplitterBeforeJson);
			}
		}
	}


void EditorShell::RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area){
		const float tabH = 24;
		// 位置记忆:记下每个停靠标签所在组的首个面板(菜单重新打开该面板时回到这一组)。
		if (!node.Panels.empty())
			for (const std::string& panel : node.Panels)
				m_LastDockAnchors[panel] = node.Panels.front();
		// 停靠标签栏统一走组件(WuiChrome::DockTabBar),主窗口与独立窗口外观/交互一致。
		std::vector<Wui::DockTab> tabs;
		tabs.reserve(node.Panels.size());
		for (size_t i = 0; i < node.Panels.size(); ++i)
			tabs.push_back({ Wui::HashId(("tab." + node.Panels[i]).c_str()), PanelTitle(node.Panels[i]), i == node.Active });
		const Wui::DockTabBarResult tabResult = Wui::DockTabBar(ctx, { area.X, area.Y, area.W, tabH }, tabs, m_Theme);

		if (tabResult.Clicked >= 0 && static_cast<size_t>(tabResult.Clicked) < node.Panels.size())
		{
			m_Layout.Activate(node.Panels[tabResult.Clicked]);
			// 单纯点击标签不是拖拽:清掉可能残留的拖拽来源记录。
			m_TabDragPanel.clear();
		}
		if (tabResult.Closed >= 0 && static_cast<size_t>(tabResult.Closed) < node.Panels.size())
		{
			// 只登记请求:此刻正在遍历停靠树,直接删标签会让本组(乃至父级分栏)
			// 塌缩,RenderTabs/RenderSplit 手里的 node/children 引用立即失效。
			// 真正删除在 OnRender 末尾统一执行(见 m_PendingPanelCloses)。
			m_PendingPanelCloses.push_back(node.Panels[tabResult.Closed]);
		}
		if (tabResult.DragStart >= 0 && static_cast<size_t>(tabResult.DragStart) < node.Panels.size())
		{
			const std::string& panel = node.Panels[tabResult.DragStart];
			// P3-1①:拖拽只能在**按下沿**起手。DockTabBar 的 DragStart 是"按住即报",
			// 若照单全收,一次点击在"面板刚回到停靠树、标签正好画在光标下、按键还没抬"时
			// (典型场景:点浮窗 ✕ → 面板收回停靠位)会被当成新的拖拽,面板立刻被再次浮出 ——
			// 这正是"第一次点 ✕ 被'重新浮出'吃掉"的机制。独立窗口的标签早就是按下沿起手
			// (见 FloatWindowHost::RenderTabBar),这里补齐同一条规则。
			if (ctx.Input().MouseClicked[0])
			{
				ctx.BeginDrag(Wui::HashId(("tab." + panel).c_str()), "panel:" + panel);
				// 记录本次拖拽的真实来源:只在还没有来源时记一次。拖动过程中经过别的标签页时
				// DockTabBar 仍会报 DragStart,若覆盖会把真实来源记错 —— 拖出分支的
				// "m_TabDragPanel == m_DragPanel" 守卫随即拒绝,表现是面板拖不出来。
				if (m_TabDragPanel.empty())
					m_TabDragPanel = panel;
			}
		}

		const Wui::WuiRect content { area.X, area.Y + tabH, area.W, area.H - tabH };
		if (!node.Panels.empty())
		{
			// P4-U6b:面板级模态的拥有者自己需要能命中 —— 先解封锁,画完再封回去,
			// 这样它之后渲染的面板(以及本帧的其它交互)仍然被挡住。
			const std::string& active = node.Panels[node.Active];
			const bool ownsPanelModal = !m_PanelModalOwner.empty() && m_PanelModalOwner == active;
			if (ownsPanelModal)
				Wui::EndModalInputBlock(ctx);
			RenderPanelContent(ctx, active, content);
			if (ownsPanelModal)
				Wui::BeginModalInputBlock(ctx);
		}

		std::string dragPayload;
		// 独立窗口不能在停靠面板上落区(只能挂靠到顶部挂靠栏)。
		if (!m_EdgeDockActive && ctx.IsDragActive(&dragPayload) && dragPayload.rfind("panel:", 0) == 0
			&& !IsIndependentPanel(dragPayload.substr(6)) && ctx.IsHovered(area))
		{
			const glm::vec2 rel = ctx.Input().MousePos - glm::vec2 { area.X, area.Y };
			const float lx = area.W > 0 ? rel.x / area.W : 0;
			const float ly = area.H > 0 ? rel.y / area.H : 0;
			Wui::DropZone targetZone = Wui::DropZone::Center;
			if (lx < 0.25f) targetZone = Wui::DropZone::Left;
			else if (lx > 0.75f) targetZone = Wui::DropZone::Right;
			else if (ly < 0.25f) targetZone = Wui::DropZone::Top;
			else if (ly > 0.75f) targetZone = Wui::DropZone::Bottom;
			// 中心区(面板主体或标签栏)= 合并进该组的标签页(用户 2026-09-16:
			// 拖到另一个子面板上就应该变成同组标签,原来只认 24px 标签栏,很难命中)。
			ctx.DropTarget(area, "panel:"); // 武装落点:仅面板拖拽在此生效
			m_DropZone = targetZone;
			Wui::WuiRect zone = area;
			if (m_DropZone == Wui::DropZone::Left) zone.W = area.W * 0.25f;
			else if (m_DropZone == Wui::DropZone::Right) { zone.X = area.X + area.W * 0.75f; zone.W = area.W * 0.25f; }
			else if (m_DropZone == Wui::DropZone::Top) zone.H = area.H * 0.25f;
			else if (m_DropZone == Wui::DropZone::Bottom) { zone.Y = area.Y + area.H * 0.75f; zone.H = area.H * 0.25f; }
			else zone.H = tabH; // 中心落点:高亮该组标签栏(它会变成这里的一个标签页)
			// 落区预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
			if (!node.Panels.empty())
			{
				const std::string target = node.Panels[node.Active];
				if (target != m_LastDragTarget || m_DropZone != m_LastDragZone)
					ctx.RecordOp("drag", "hover", target, ZoneName(m_DropZone));
				m_LastDragTarget = target;
				m_LastDragZone = m_DropZone;
				m_DropTargetPanel = node.Panels[node.Active];
			}
		}
	}


void EditorShell::RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect){
		Wui::PanelBackground(ctx, rect, m_Theme.PanelBg);
		// 无障碍树:此后登记的控件归属该面板(ui.tree/state.dump 靠它区分面板)。
		Wui::WuiAccessibility::Get().SetPanel(id);
		ctx.SetPanelId(id);
		const auto it = m_PanelRegistry.find(id);
		if (it != m_PanelRegistry.end())
			it->second->OnRender(ctx, rect, *this);
		else
			Label(ctx, { rect.X + 8, rect.Y + 8 }, PanelTitle(id), m_Theme.TextMuted, 14.0f);
	}


	// 挂靠栏:横跨主窗口的一条,按"窗口"列出一行(chip 显示活动标签名与标签数),
	// 独立窗口被拖到这条栏上(窗口中心进入栏内并停稳)或点 Attach 会整窗挂靠回主窗口。
void EditorShell::DrawAttachBar(Wui::WuiContext& ctx){
		const glm::vec2 viewport = ctx.ViewportSize();
		// 顶部第一行即挂靠栏:也是无边框主窗口的拖动/关闭区域。
		const Wui::WuiRect bar { 0, 0.0f, viewport.x, m_AttachBarHeight };
		const Wui::WuiColor fill = m_AttachSlotHighlight
			? Wui::WuiColor { 0.3f, 0.5f, 0.9f, 0.45f }
			: m_Theme.PanelHeader;
		Wui::BarSurface(ctx, bar, fill, m_Theme.Border);

		// 标准窗口控制(最小化/最大化/关闭)在最右侧。
		const Wui::WindowControl control = Wui::WindowControls(ctx,
			{ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }, m_Theme,
			Application::HasInstance() && Application::Get().GetWindow().IsMaximized());
		if (control == Wui::WindowControl::Minimize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().Minimize();
			return;
		}
		if (control == Wui::WindowControl::Maximize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().MaximizeOrRestore();
			return;
		}
		if (control == Wui::WindowControl::Close)
		{
			m_Editor.CloseAction();
			return;
		}

		// 标签栏只列出"窗口":Main + 已附加到主窗口的独立窗口(切换关系)。
		float x = bar.X + 6.0f;
		{
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 90.0f, bar.H - 6.0f };
			const bool active = m_ActiveWindowTag.empty();
			// 标签 chip 走组件:活动/悬停底色与关闭 x 的外观统一。
			if (Wui::AttachTag(ctx, tab, "Main", active, false, m_Theme).Clicked)
				m_ActiveWindowTag.clear();
			// P3-1②:Main 标签同样登记(稳定 id `shell.attach.main`)—— 脚本可以在
			// ui.attach 把视图切到某个附加窗口之后,再读/点这一枚标签切回主界面。
			RegisterAttachNode("main", tab, active ? "active" : "inactive", "Main", true);
			x += 96.0f;
		}
		x += 4.0f;

		// 已附加的独立窗口标签:点击切换;× 关闭该窗口;按住可拖出为独立窗口;
		// 多个附加窗口之间可左右拖动换位。
		std::string closeRequest;
		struct AttachTagHit { std::string Panel; Wui::WuiRect Rect; };
		std::vector<AttachTagHit> tagHits;
		for (const std::string& panel : m_AttachedPanels)
		{
			const bool active = m_ActiveWindowTag == panel;
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 140.0f, bar.H - 6.0f };
			const Wui::AttachTagResult tag = Wui::AttachTag(ctx, tab, PanelTitle(panel), active, true, m_Theme);
			// P3-1②:同一 id 在浮动态由下面的浮窗分支登记(value=floating),这里登记附加态。
			RegisterAttachNode(panel, tab, "attached", PanelTitle(panel), true);
			if (tag.CloseClicked)
				closeRequest = panel;
			// 按下(非关闭键)记录起点;移动超过阈值进入拖动。
			if (ctx.Input().MouseDown[0] && tag.Hovered && !tag.CloseHovered
				&& m_AttachTagDrag.empty())
			{
				m_AttachTagPress = panel;
				m_AttachTagPressPos = ctx.Input().MousePos;
			}
			if (m_AttachTagPress == panel && m_AttachTagDrag.empty() && ctx.Input().MouseDown[0]
				&& glm::length(ctx.Input().MousePos - m_AttachTagPressPos) > 3.0f)
				m_AttachTagDrag = panel;
			if (m_AttachTagDrag == panel)
			{
				Wui::HighlightOutline(ctx, tab, m_Theme.Accent, 2.0f, 2.0f);
				// 脱出提示:跟随光标的标签名 + 栏外时提示将变为独立窗口。
				const bool outside = !ctx.IsHovered(bar);
				ctx.PushOverlay();
				Label(ctx, { ctx.Input().MousePos.x + 14.0f, ctx.Input().MousePos.y + 14.0f },
					outside ? std::string(PanelTitle(panel)) + "  →  独立窗口" : std::string(PanelTitle(panel)),
					m_Theme.Text, 13.0f);
				ctx.PopOverlay();
			}
			tagHits.push_back({ panel, tab });
			x += 144.0f;
		}

		// 同一窗口的其它标签页:顶栏只给窗口一枚 chip(拖出=整个窗口),但它们的形态
		// 也要能从无障碍树读到 —— 注册成只读节点(Interactive=false,共用同一 chip 矩形)。
		for (const AttachTagHit& hit : tagHits)
		{
			FloatWindowHost* host = FindFloatHost(hit.Panel);
			if (!host)
				continue;
			for (const std::string& panel : host->Panels())
			{
				if (panel == hit.Panel)
					continue;
				RegisterAttachNode(panel, hit.Rect, "attached-tab", PanelTitle(panel), false);
			}
		}

		// P3-1②:可见的独立窗口 = 浮动态。节点与顶栏标签共用 id(`shell.attach.<panel>`),
		// value=floating;rect 由窗口屏幕矩形换算到主窗口客户区,只用于"读状态",因此
		// Interactive=false(点它不应该落到主窗口上,也不参与 ui.invoke)。
		{
			int mainX = 0, mainY = 0;
			if (Application::HasInstance())
				Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (host->IsHidden())
					continue;
				const Wui::WuiRect screen = host->ScreenRect();
				const Wui::WuiRect local { screen.X - static_cast<float>(mainX),
					screen.Y - static_cast<float>(mainY), screen.W, screen.H };
				for (const std::string& panel : host->Panels())
					RegisterAttachNode(panel, local, "floating", PanelTitle(panel), false);
			}
		}

		if (!closeRequest.empty())
		{
			// P4-U13e:prefab 面板有未保存改动 → 先弹确认;窗口先留着,标签也不能摘。
			if (InterceptPrefabUnsaved(closeRequest, PrefabPendingAction::Close))
				closeRequest.clear();
		}
		if (!closeRequest.empty())
		{
			// × = 关闭:隐藏该窗口的面板(可从 Window 菜单重新打开),并把标签移出栏。
			// 一窗口一枚标签:关掉的是整个窗口,该窗口所有标签页的记录一起摘掉。
			std::vector<std::string> closingPanels;
			if (FloatWindowHost* host = FindFloatHost(closeRequest))
				closingPanels = host->Panels();
			if (closingPanels.empty())
				closingPanels.push_back(closeRequest);
			CloseFloatWindow(closeRequest, true, &ctx);
			for (const std::string& id : closingPanels)
			{
				m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
					m_AttachedPanels.end());
				if (m_ActiveWindowTag == id)
					m_ActiveWindowTag.clear();
			}
		}
		// 拖动结束:在栏内 → 换位;在栏外 → 拖出为独立窗口。
		if (m_AttachTagDrag.empty() && !m_AttachTagPress.empty() && ctx.Input().MouseReleased[0])
		{
			// 未拖动 = 单击:切换显示该窗口内容。
			m_ActiveWindowTag = m_AttachTagPress;
			m_AttachTagPress.clear();
		}
		if (!m_AttachTagDrag.empty() && ctx.Input().MouseReleased[0])
		{
			const std::string dragged = m_AttachTagDrag;
			if (ctx.IsHovered(bar))
			{
				size_t target = m_AttachedPanels.size();
				for (size_t i = 0; i < tagHits.size(); ++i)
					if (ctx.Input().MousePos.x < tagHits[i].Rect.X + tagHits[i].Rect.W * 0.5f)
					{
						target = i;
						break;
					}
				const auto current = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), dragged);
				if (current != m_AttachedPanels.end())
				{
					const size_t from = static_cast<size_t>(current - m_AttachedPanels.begin());
					m_AttachedPanels.erase(current);
					if (target > from && target > 0)
						--target;
					m_AttachedPanels.insert(m_AttachedPanels.begin()
						+ static_cast<std::ptrdiff_t>(std::min(target, m_AttachedPanels.size())), dragged);
				}
			}
			else if (FloatWindowHost* host = FindFloatHost(dragged))
			{
				// 拖出:恢复为独立窗口,窗口放到光标附近(屏幕坐标)。
				const std::vector<std::string> hostPanels = host->Panels();
				int mainX = 0, mainY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&mainX, &mainY);
				host->SetScreenPosition(static_cast<float>(mainX) + ctx.Input().MousePos.x - 60.0f,
					static_cast<float>(mainY) + ctx.Input().MousePos.y - 12.0f);
				// 拖出来的是**这个窗口**,窗口显示用户拖的那一页(标签页可能被切过)。
				host->ActivatePanel(dragged);
				host->SetHidden(false);
				// 整窗离槽:把该窗口里所有标签页的附加记录一次性摘干净(不留下悬空 chip)。
				for (const std::string& id : hostPanels)
				{
					m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
						m_AttachedPanels.end());
					if (m_ActiveWindowTag == id)
						m_ActiveWindowTag.clear();
				}
				ctx.RecordOp("float", "detach", dragged, "");
				SaveLayout();   // 形态变了(挂靠 → 浮窗),立刻落盘(P4-UX10)
			}
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}
		if (!ctx.Input().MouseDown[0] && m_AttachTagDrag.empty())
			m_AttachTagPress.clear();
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(bar) && !ctx.IsHovered({ 0, 0, x, bar.H })
			&& !ctx.IsHovered({ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }))
			Application::Get().GetWindow().BeginSystemDrag();
	}


	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板),绘制在停靠区之上。
void EditorShell::RenderFloating(Wui::WuiContext& ctx){
		if (m_AttachCooldownFrames > 0)
			--m_AttachCooldownFrames;
		// 每帧复位挂靠栏高亮,只在独立窗口真正悬停其上时点亮,避免残留。
		m_AttachSlotHighlight = false;

		// 槽位屏幕矩形(客户区 -> 屏幕):用于判断独立窗口是否停到了槽位上。
		const glm::vec2 viewport = ctx.ViewportSize();
		// 挂靠栏的屏幕矩形(横条):客户区坐标 -> 屏幕坐标。
		// 注意:viewport 是**设计单位**(= 物理像素 / UiScale),而这里要和 GetCursorPos /
		// 窗口屏幕矩形(都是物理像素)比较 —— 必须乘回 UiScale,否则 UI 缩放 1.3 时
		// 命中区只有真实栏高的 77%,"拖到栏上"会时灵时不灵。
		const float uiScale = Wui::UiScale();
		m_AttachSlotScreenRect = { 0, 0.0f, viewport.x * uiScale, m_AttachBarHeight * uiScale };
		int windowX = 0, windowY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		m_AttachSlotScreenRect.X += static_cast<float>(windowX);
		m_AttachSlotScreenRect.Y += static_cast<float>(windowY);

		// 独立 OS 窗口不在这里渲染 —— 见 RenderIndependentWindows(帧末调用,理由见该函数)。
		// 停靠形态的"临时浮动"面板:在主窗口内绘制(OS 窗口只属于 Independent 面板)。
		for (size_t i = 0; i < m_Layout.Floating.size(); ++i)
		{
			const std::string panel = m_Layout.Floating[i].Panel;
			if (IsIndependentPanel(panel))
				continue; // 独立面板有自己的 OS 窗口
			bool closed = false;
			RenderFloatWindow(ctx, m_Layout.Floating[i], &closed);
			if (closed)
			{
				// 停靠形态的浮动窗口关闭 = 回停靠位(D3),不是隐藏面板。
				HideFloatPanel(panel, &ctx);
				break; // 容器已改变,下一帧继续绘制其余窗口
			}
		}
		// 置顶在绘制结束后应用,避免遍历中修改容器。
		if (!m_BringFloatFront.empty())
		{
			m_Layout.BringFloatToFront(m_BringFloatFront);
			m_BringFloatFront.clear();
		}
		// 落点预览画在所有浮动面板之上:拖动中的面板正好盖在目标上。
		if (m_DropPreviewActive)
			Wui::DropZoneOverlay(ctx, m_DropPreviewRect, 0.30f, 3.0f);
	}


	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板)。
	//
	// **为什么放在主窗口 UI 的最后一步**(U26 修):每个窗口渲染前都会调
	// `WuiAccessibility::BeginFrame(windowKey)` 把无障碍树的"当前窗口"切到自己,而
	// 主窗口的菜单栏 / 状态栏 / 外壳模态都在停靠面板之后才绘制。若独立窗口在本帧中段
	// 渲染,之后登记的主窗口节点(菜单栏 File/Window、菜单项、状态栏…)就会挂着**浮窗的
	// window 键** —— `ui.invoke` 会把点击投进浮窗,菜单从此点不开(实测:U25 探针在
	// `window=main` 下找不到 menu.window,唯一那份指向材质浮窗,点它只打中浮窗的标签栏),
	// 跨窗口拖放的落点登记同样晚于源面板渲染(见 AssetDropBridge 的帧老化)。
	// 移到帧末后,主窗口 UI 全程处于 BeginFrame("main") 之后,独立窗口只影响自己的节点。
void EditorShell::RenderIndependentWindows(Wui::WuiContext& ctx){
		// 每个独立窗口渲染自己的 OS 窗口(含标签栏);窗口被关闭 = 隐藏其全部面板。
		// 标签栏 x 只登记关闭请求,统一在遍历结束后处理,避免边遍历边改 m_FloatHosts。
		std::vector<std::string> closeRequests;
		// 收集新发起的标签拖拽(跨窗口附加);同帧只接受一个。
		// 独立窗口标签被拖过阈值 → 交给系统移动循环(阻塞到松手,见 PerformIndependentWindowDrag)。
		for (const std::unique_ptr<FloatWindowHost>& candidate : m_FloatHosts)
		{
			if (const std::string drag = candidate->TakePendingTabDrag(); !drag.empty())
			{
				PerformIndependentWindowDrag(drag);
				break;
			}
		}
		for (size_t i = 0; i < m_FloatHosts.size(); )
		{
			FloatWindowHost& host = *m_FloatHosts[i];
			// 隐藏的宿主(复用中)不渲染。
			if (host.IsHidden())
			{
				++i;
				continue;
			}
			// 空宿主:隐藏保留(运行期销毁窗口在 Vulkan 下会崩),等待下次复用。
			if (host.Panels().empty())
			{
				host.SetHidden(true);
				++i;
				continue;
			}
			bool alive = true;
			try
			{
				alive = host.Render();
			}
			catch (const std::exception& error)
			{
				WLD_CORE_ERROR("[float] render failed for '{0}': {1}", host.Panel(), error.what());
				alive = false;
			}
			if (!alive)
			{
				const std::string panel = host.Panel();
				CloseFloatWindow(panel, true, &ctx);
				continue; // CloseFloatWindow 会移除该 host
			}
			if (const std::string closing = host.TakeCloseRequest(); !closing.empty())
				closeRequests.push_back(closing);

			// 位置/尺寸变化写回布局(供重启恢复):窗口内每个标签写同一屏幕矩形。
			const Wui::WuiRect rect = host.ScreenRect();
			for (const std::string& panel : host.Panels())
				if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
					entry->Rect = rect;

			// 位置记忆写回布局(拖动由系统移动循环负责,这里只记录最终矩形)。
			const std::string windowKey = host.Panels().front();
			m_LastFloatScreenRects[windowKey] = rect;
			++i;
		}
		for (const std::string& panel : closeRequests)
			HideFloatPanel(panel, &ctx);
		// 挂靠栏高亮不再在这里复位:本函数已经排到 DrawAttachBar 之后,复位会擦掉
		// 当帧刚算出的高亮;残留由下一帧 RenderFloating 开头的复位统一清掉。
		//
		// 独立窗口渲染会把当前 GL 上下文切到各自窗口,这里恢复主窗口上下文 ——
		// 否则主窗口后续的呈现/交换会作用在错误的上下文上(表现为主窗口不再刷新)。
		if (Application::HasInstance())
			Application::Get().GetWindow().MakeCurrent();
	}


	// 窗口内浮动面板(停靠形态面板拖出后的形态)的绘制与交互:
	// 标题栏拖动 = 移动(拖动载荷仍是 "panel:",拖到停靠落点上即回停靠);
	// 右下角 = 缩放;左上 × = 关闭浮动窗口(停靠形态 = 回停靠位,D3)。
	// 独立窗口(Independent)不走这条路径,它们由 FloatWindowHost 的 OS 窗口渲染。
void EditorShell::RenderFloatWindow(Wui::WuiContext& ctx, Wui::DockFloat& window, bool* closed){
		const float titleH = 24.0f;
		const glm::vec2 viewport = ctx.ViewportSize();
		const float topLimit = 26.0f + m_AttachBarHeight;
		Wui::WuiRect& rect = window.Rect;

		// 视口约束:窗口不能完全跑出编辑区,标题栏必须可见。
		rect.W = std::max(240.0f, std::min(rect.W, std::max(240.0f, viewport.x - 16.0f)));
		rect.H = std::max(160.0f, std::min(rect.H, std::max(160.0f, viewport.y - topLimit - 16.0f)));
		rect.X = std::max(8.0f, std::min(rect.X, std::max(8.0f, viewport.x - rect.W - 8.0f)));
		rect.Y = std::max(topLimit, std::min(rect.Y, std::max(topLimit, viewport.y - rect.H - 8.0f)));
		if (std::getenv("WLD_TRACE_UI"))
		{
			// 诊断/自动化:窗口内浮动面板的实际矩形(客户区坐标),供脚本点击标题栏与关闭按钮。
			static int traced = 0;
			if (traced < 60)
			{
				++traced;
				WLD_CORE_INFO("[float] in-window '{0}' rect=({1},{2},{3},{4})",
					window.Panel, rect.X, rect.Y, rect.W, rect.H);
			}
		}

		ctx.PushOverlay();
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelBg, 5.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect, m_Theme.Border, 5.0f, 1.0f });
		const Wui::WuiRect title { rect.X, rect.Y, rect.W, titleH };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, title, m_Theme.PanelHeader, 5.0f });
		Label(ctx, { title.X + 10.0f, title.Y + 4.0f }, PanelTitle(window.Panel), m_Theme.Text, 14.0f);

		// 关闭按钮
		const Wui::WuiRect close { title.X + title.W - 22.0f, title.Y + 5.0f, 14.0f, 14.0f };
		const bool overClose = ctx.IsHovered(close);
		if (overClose)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, close, m_Theme.ButtonHover, 2.0f });
		Label(ctx, { close.X + 3.0f, close.Y - 2.0f }, "x", m_Theme.TextMuted, 13.0f);
		const bool closeClicked = ctx.IsClicked(close);

		// 标题栏拖动:置顶 + 跟随鼠标;松手落在停靠落点上则由外壳的落位逻辑回停靠。
		if (!closeClicked && !overClose && ctx.IsClicked(title))
		{
			m_BringFloatFront = window.Panel;
			m_MovingFloat = window.Panel;
			m_FloatGrabOffset = ctx.Input().MousePos - glm::vec2 { rect.X, rect.Y };
			m_FloatChangeBefore = m_Layout.Serialize();
			ctx.BeginDrag(Wui::HashId(("float." + window.Panel).c_str()), "panel:" + window.Panel);
		}
		if (m_MovingFloat == window.Panel && ctx.IsDragActive(nullptr))
		{
			rect.X = std::max(8.0f, std::min(ctx.Input().MousePos.x - m_FloatGrabOffset.x,
				std::max(8.0f, viewport.x - rect.W - 8.0f)));
			rect.Y = std::max(topLimit, std::min(ctx.Input().MousePos.y - m_FloatGrabOffset.y,
				std::max(topLimit, viewport.y - rect.H - 8.0f)));
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}

		// 右下角缩放
		const Wui::WuiRect grip { rect.X + rect.W - 16.0f, rect.Y + rect.H - 16.0f, 16.0f, 16.0f };
		if (ctx.IsHovered(grip))
		{
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, grip, m_Theme.ButtonHover, 3.0f });
			ctx.SetCursor(Wui::WuiCursor::ResizeEW);
		}
		if (ctx.IsClicked(grip))
		{
			m_BringFloatFront = window.Panel;
			m_FloatResize = window.Panel;
			m_FloatResizeStart = ctx.Input().MousePos;
			m_FloatResizeRect = rect;
			m_FloatChangeBefore = m_Layout.Serialize();
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseDown[0])
		{
			const glm::vec2 delta = ctx.Input().MousePos - m_FloatResizeStart;
			rect.W = std::max(240.0f, m_FloatResizeRect.W + delta.x);
			rect.H = std::max(160.0f, m_FloatResizeRect.H + delta.y);
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseReleased[0])
		{
			m_FloatResize.clear();
			if (!m_FloatChangeBefore.empty())
			{
				RecordDockChange(ctx, "float-resize", window.Panel, m_FloatChangeBefore);
				m_FloatChangeBefore.clear();
			}
		}
		// 尺寸记忆:下次拖出沿用用户调好的大小(位置按当次拖拽点重新计算)。
		m_LastFloatRects[window.Panel] = { 0.0f, 0.0f, rect.W, rect.H };

		// 自动化钩子(开发验证):WLD_FLOAT_RECT_FILE=<路径> 时把浮动面板的客户区矩形
		// 写进该文件(矩形变化才写),供脚本点击标题栏/关闭按钮。
		if (const char* rectFile = std::getenv("WLD_FLOAT_RECT_FILE"))
		{
			char buffer[192];
			std::snprintf(buffer, sizeof(buffer), "panel=%s\nx=%.1f\ny=%.1f\nw=%.1f\nh=%.1f\n",
				window.Panel.c_str(), rect.X, rect.Y, rect.W, rect.H);
			static std::string lastWritten;
			if (lastWritten != buffer)
			{
				lastWritten = buffer;
				std::ofstream(rectFile, std::ios::trunc) << buffer;
			}
		}

		// 内容区(标题栏之下)
		const Wui::WuiRect body { rect.X + 1.0f, rect.Y + titleH, rect.W - 2.0f, rect.H - titleH - 1.0f };
		RenderPanelContent(ctx, window.Panel, body);
		ctx.PopOverlay();

		if (closeClicked && closed)
			*closed = true;
	}


	// P4-UX9:拖动独立窗口(在标签栏/空白区按下并越过阈值后进入)。
	//
	// 手感设计(用户 2026-09-20:"独立窗口拖拽手感差,鼠标滑动一快就会出现偏移"):
	//   ① 窗口移动交给**系统移动循环**(SC_MOVE):系统按输入频率移动窗口,与渲染帧率无关
	//      —— 本引擎 Debug 下一帧 50ms,"每帧轮询光标"必然发飘,再怎么调都追不上;
	//      它同时自带 Esc 取消与系统吸附,是 Windows 上拖标题栏的工业标准做法。
	//   ② 进入循环前把窗口"预置"到 光标 - 按下瞬间的抓取偏移:补偿"按下 → 识别到拖动"
	//      之间已经发生的位移。否则系统会把那段位移吸收进抓取偏移,快速甩动时窗口不跟手。
	//   ③ 拖动期间光标进入挂靠栏 → 窗口被压到栏下方(SetSystemDragParkZone):
	//      "窗口停在栏下"就是"松手即挂靠"的可见提示,目标不会被窗口自己挡住。
	//   ④ 松手按真实落点判定:挂靠栏上 → 整窗挂靠;Esc → 回到拖动前的位置(取消);
	//      其它位置 → 就停在那里(位置由 OnRender 里既有的写回逻辑进布局)。
void EditorShell::PerformIndependentWindowDrag(const std::string& panel){
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		Window* window = host->NativeWindow();
		if (!window)
			return;

		const glm::vec2 grab = host->TakePendingTabDragGrab();
		const Wui::WuiRect startRect = host->ScreenRect();
		POINT cursor { 0, 0 };
		GetCursorPos(&cursor);

		int mainX = 0, mainY = 0;
		float mainW = static_cast<float>(cursor.x + 1280);
		if (Application::HasInstance())
		{
			Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			mainW = static_cast<float>(Application::Get().GetWindow().GetWidth());
		}
		const float barPixels = m_AttachBarHeight * Wui::UiScale();

		window->SetPosition(static_cast<int>(static_cast<float>(cursor.x) - grab.x),
			static_cast<int>(static_cast<float>(cursor.y) - grab.y));
		// 投放提示:光标进挂靠栏 → 栏上亮一层半透明提示(不改窗口位置,窗口全程跟手)。
		window->SetSystemDragDropHint(
			{ static_cast<float>(mainX), static_cast<float>(mainY), mainW, barPixels });
		host->SetTabDragActive(true);
		window->BeginSystemDrag();          // 阻塞:系统移动循环,回到这里就是松手
		window->SetSystemDragDropHint({ 0.0f, 0.0f, 0.0f, 0.0f });
		host->SetTabDragActive(false);

		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
		{
			// Esc = 取消(系统只保证回到循环起点,也就是预置后的位置;这里再放回按下前的位置)。
			host->SetScreenPosition(startRect.X, startRect.Y);
			WLD_CORE_INFO("[float] drag cancelled by Esc: {0}", panel);
			return;
		}

		POINT released { 0, 0 };
		GetCursorPos(&released);
		const bool overAttachBar = IsIndependentPanel(panel)
			&& m_AttachSlotScreenRect.W > 0.0f
			&& static_cast<float>(released.x) >= m_AttachSlotScreenRect.X
			&& static_cast<float>(released.x) <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
			&& static_cast<float>(released.y) >= m_AttachSlotScreenRect.Y
			&& static_cast<float>(released.y) <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
		// 落点诊断(拖拽是模态循环,出问题时日志是唯一现场)。
		WLD_CORE_INFO("[float] drag end: panel={0} released=({1},{2}) slot=({3:.0f},{4:.0f},{5:.0f},{6:.0f}) attach={7}",
			panel, released.x, released.y, m_AttachSlotScreenRect.X, m_AttachSlotScreenRect.Y,
			m_AttachSlotScreenRect.W, m_AttachSlotScreenRect.H, overAttachBar ? 1 : 0);
		if (overAttachBar)
		{
			AttachIndependentWindowToSlot(panel);
			return;
		}
		if (m_Ctx)
			m_Ctx->RecordOp("float", "move", panel, "");
	}


void EditorShell::AddFloatWindow(const std::string& panel, const Wui::WuiRect& screenRect, const char* origin, bool startHidden){
		WLD_CORE_INFO("[float] AddFloatWindow panel={0} origin={1} rect=({2},{3},{4},{5})",
			panel, origin, screenRect.X, screenRect.Y, screenRect.W, screenRect.H);
		// 去重(T03):同一面板最多一个窗口——已存在的窗口(可见或隐藏)直接复用,
		// 避免"同一面板被创建两次"这类布局乱象。
		if (FloatWindowHost* existing = FindFloatHost(panel))
		{
			existing->SetScreenPosition(screenRect.X, screenRect.Y);
			existing->ActivatePanel(panel);
			existing->SetHidden(startHidden);
			WLD_CORE_INFO("[float] reused existing window for panel={0}", panel);
			return;
		}
		// 复用已隐藏的独立窗口:运行期销毁窗口在 Vulkan 下会崩,因此"关闭/挂靠"只隐藏。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			// P4-UX9:只复用**空窗**(面板全部关掉了的"壳")。以前会把新面板塞进一个还挂着
			// 别的面板的隐藏窗口里,于是"一个 OS 窗口 + 两枚顶栏标签",拖出左侧那枚会把整个
			// 窗口(含另一个面板)一起拔出来,另一枚标签的状态就悬空了(用户 2026-09-20 复现)。
			if (!host->IsHidden() || !host->Panels().empty())
				continue;
			host->SetScreenPosition(screenRect.X, screenRect.Y);
			host->AddPanel(panel, true);
			host->SetHidden(startHidden);
			WLD_CORE_INFO("[float] reused hidden window for panel={0}", panel);
			return;
		}
		// 独立窗口 = 容器 + 标签栏;标题与内容由面板注册表提供,容器不感知具体面板类型。
		FloatWindowHost::Callbacks callbacks;
		callbacks.Theme = m_Theme;
		callbacks.Title = [this](const std::string& id) { return std::string(PanelTitle(id)); };
		callbacks.Content = [this](Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& id)
		{
			RenderPanelContent(ctx, id, rect);
		};
		callbacks.TabDragStart = [](const std::string& panel)
		{
			WLD_CORE_INFO("[float] tag drag started: {0}", panel);
		};
		callbacks.DockToMain = [this](const std::string& id) { AttachIndependentWindowToSlot(id); };
		callbacks.CloseWindow = [this](const std::string& id) { CloseFloatWindow(id, false, m_Ctx); };
		callbacks.CanAttach = [this](const std::string& id) { return IsIndependentPanel(id); };
		try
		{
			m_FloatHosts.push_back(std::make_unique<FloatWindowHost>(panel, PanelTitle(panel), screenRect, std::move(callbacks)));
			if (startHidden)
				m_FloatHosts.back()->SetHidden(true);   // 恢复成顶栏标签:不闪窗口
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("[float] create failed for '{0}': {1}", panel, error.what());
			m_Layout.CloseFloating(panel);
		}
	}


	// P4-UX10:按策略恢复上次的独立窗口。
	// forceTabs = 一律恢复成顶栏标签(不弹 OS 窗口、不抢焦点);否则按每项上次形态。
void EditorShell::RestoreIndependentWindows(const std::vector<PendingFloatRestore>& items, bool forceTabs){
		uint32_t restored = 0;
		uint32_t asTabs = 0;
		for (const PendingFloatRestore& item : items)
		{
			if (item.Panels.empty())
				continue;
			// 动态面板(材质/脚本/模型)按 id 补建实例。
			for (const std::string& panel : item.Panels)
			{
				EnsureMaterialPanelFromId(panel);
				EnsureScriptPanelFromId(panel);
				EnsureModelPanelFromId(panel);
				EnsurePrefabPanelFromId(panel);
				EnsurePluginPanelFromId(panel);   // PLUG-T3b:插件面板跨会话恢复
			}
			const bool attach = forceTabs || item.Attached;
			// 先隐藏着建出来:恢复成 chip 时不会"闪一下窗口",恢复成浮窗时下一步再显示。
			AddFloatWindow(item.Panels.front(), item.Rect, "restore", true);
			FloatWindowHost* host = m_FloatHosts.empty() ? nullptr : m_FloatHosts.back().get();
			if (!host)
				continue;
			for (size_t i = 1; i < item.Panels.size(); ++i)
				host->AddPanel(item.Panels[i], false);
			if (attach)
			{
				AttachIndependentWindowToSlot(item.Panels.front());
				++asTabs;
			}
			else
			{
				host->ShowWithoutActivation();   // 浮窗按上次位置回来,但不抢焦点
			}
			++restored;
		}
		if (restored > 0)
		{
			std::string text = Wui::Tr("notice.restore", "Restored last session's windows") + ": "
				+ std::to_string(restored);
			if (asTabs == restored)
				text += "  ·  " + Wui::Tr("notice.restore.tabs", "they are in the top bar");
			PushNotice(text);
		}
	}


	// 状态栏提示:给"刚刚发生了什么"一个不打断的表达。
void EditorShell::PushNotice(const std::string& text){
		m_Notice.Text = text;
		m_Notice.Active = true;
		m_Notice.Timing = TimedNotice {};
		m_Notice.Timing.ShownAt = ShellNowSeconds();
		WLD_CORE_INFO("[notice] {0}", text);
	}


	// 提示节拍(状态栏提示与恢复询问条共用):
	//   · 悬停 = 冻结倒计时(用户要读/要点,不能读一半消失);
	//   · 移开 = 再给宽限,然后淡出 —— 悬停超过停留时长再移开也是这个节奏;
	//   · 返回 false 表示"该收了",由调用方决定收起来后做什么。
bool EditorShell::TimedNotice::Update(double now, bool hovered, double staySeconds, double graceSeconds, double fadeSeconds){
		if (hovered)
		{
			ShownAt = now;
			LeaveAt = 0.0;
		}
		else if (Hovered)
		{
			LeaveAt = now;
		}
		Hovered = hovered;
		if (hovered)
		{
			Alpha = 1.0f;
			return true;
		}
		const bool leaving = LeaveAt > 0.0;
		const double elapsed = now - (leaving ? LeaveAt : ShownAt);
		const double limit = leaving ? graceSeconds : staySeconds;
		if (elapsed >= limit + fadeSeconds)
			return false;
		Alpha = elapsed <= limit ? 1.0f
			: 1.0f - static_cast<float>((elapsed - limit) / fadeSeconds);
		Alpha = std::clamp(Alpha, 0.0f, 1.0f);
		return true;
	}

}
