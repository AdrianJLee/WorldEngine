#include "EditorShell_Internal.h"

namespace World
{

using namespace EditorShellDetail;


	// ---- PLUG-T3b:插件贡献的面板(动态实例,id = "plugin.panel.<pluginId>.<id>")----

void EditorShell::EnsurePluginPanelsFromRegistry(){
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		// 注册表是插件加载完成后的权威清单(插件中途失败不会留半个注册)。
		for (const std::string& panelId : m_PluginEditorHost->PanelIds())
			EnsurePluginPanelFromId(panelId);
	}


void EditorShell::EnsurePluginPanelFromId(const std::string& panelId){
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (!IsPluginPanelId(panelId) || !m_PluginEditorHost->HasPanel(panelId))
			return;
		auto panel = std::make_unique<PluginPanel>(*this, panelId);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
		// 默认尺寸:插件面板要装下几行控件与按钮(与插件管理器同量级)。
		if (m_LastFloatRects.find(panelId) == m_LastFloatRects.end())
			m_LastFloatRects[panelId] = Wui::WuiRect { 200.0f, 140.0f, 520.0f, 360.0f };
		if (!m_Layout.FindFloatMemory(panelId, nullptr))
			m_Layout.FloatMemory.push_back({ panelId, m_LastFloatRects[panelId] });
	}


void EditorShell::ClosePluginPanelsNotInRegistry(){
		if (m_LauncherMode || !m_PluginEditorHost)
			return;
		// 插件卸载后:注册表里没有了 ⇒ 关掉已打开的窗口/标签/停靠记录(不留下空窗口)。
		const auto pluginPanelGone = [this](const std::string& panelId)
		{
			return IsPluginPanelId(panelId) && !m_PluginEditorHost->HasPanel(panelId);
		};
		const auto isGone = [&pluginPanelGone](const std::string& panelId)
		{
			return pluginPanelGone(panelId);
		};

		// 独立窗口(含附加态标签):先关窗口,再摘附加标签。
		std::vector<std::string> staleHosts;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			for (const std::string& panelId : host->Panels())
				if (isGone(panelId) && std::find(staleHosts.begin(), staleHosts.end(), panelId)
					== staleHosts.end())
					staleHosts.push_back(panelId);
		for (const std::string& panelId : staleHosts)
			CloseFloatWindow(panelId, true, m_Ctx);
		m_AttachedPanels.erase(std::remove_if(m_AttachedPanels.begin(), m_AttachedPanels.end(),
			[&isGone](const std::string& panelId) { return isGone(panelId); }), m_AttachedPanels.end());
		if (isGone(m_ActiveWindowTag))
			m_ActiveWindowTag.clear();
		for (const Wui::DockFloat& entry : m_Layout.Floating)
			if (isGone(entry.Panel))
				m_Layout.CloseFloating(entry.Panel);
		m_Layout.FloatMemory.erase(std::remove_if(m_Layout.FloatMemory.begin(), m_Layout.FloatMemory.end(),
			[&isGone](const Wui::DockFloat& entry) { return isGone(entry.Panel); }), m_Layout.FloatMemory.end());
		m_Panels.erase(std::remove_if(m_Panels.begin(), m_Panels.end(),
			[&pluginPanelGone](const std::string& panelId) { return pluginPanelGone(panelId); }), m_Panels.end());
		std::vector<Wui::PanelId> dockedPanels;
		m_Layout.AllPanels(&dockedPanels);
		for (const Wui::PanelId& panelId : dockedPanels)
			if (isGone(panelId))
				m_Layout.RemoveTab(panelId);
		// unordered_map 不支持 remove_if 的整体删除(pair 不可赋值)⇒ 显式遍历 + erase(it)。
		for (auto it = m_PanelRegistry.begin(); it != m_PanelRegistry.end(); )
		{
			if (pluginPanelGone(it->first))
				it = m_PanelRegistry.erase(it);
			else
				++it;
		}
	}


void EditorShell::DrawPluginPanel(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& panelId){
		Plugins::PluginManager* manager = GetPluginManager();
		if (!manager || !m_PluginEditorHost || !m_PluginEditorHost->HasPanel(panelId))
		{
			// 面板已经被注销(插件卸载/禁用)而窗口还没收掉:给一行可读空态而不是空白。
			Label(ctx, { rect.X + 8.0f, rect.Y + 8.0f },
				Wui::Tr("panel.plugin.unavailable", "This plugin panel is no longer available."),
				m_Theme.TextMuted, m_Theme.FontSizeBody);
			return;
		}
		m_CurrentPluginPanelRect = rect;
		if (manager->RenderEditorPanel(panelId) != 0)
			WLD_CORE_WARN("[plugin] editor panel '{0}' draw failed", panelId);
		m_CurrentPluginPanelRect = Wui::WuiRect { 0, 0, 0, 0 };
	}


bool EditorShell::InvokePluginEditorCommand(const std::string& commandName, std::string* message){
		if (!m_PluginEditorHost)
		{
			if (message) *message = Wui::Tr("plugin.command.unavailable", "No editor command surface.");
			return false;
		}
		std::string error;
		if (!m_PluginEditorHost->InvokeEditorCommand(commandName, &error))
		{
			if (message) *message = error;
			return false;
		}
		if (message) *message = "invoked " + commandName;
		return true;
	}


	// ---- P4-U13c:prefab 资产窗口(动态实例,id = "prefab:<逻辑路径>")----

void EditorShell::EnsurePrefabPanelFromId(const std::string& panelId){
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kPrefabPanelPrefix), kPrefabPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kPrefabPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<PrefabPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}


	// ---- W9-2:脚本编辑器面板(动态实例,id = "script:<逻辑路径>")----

void EditorShell::EnsureScriptPanelFromId(const std::string& panelId){
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kScriptPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ScriptEditorPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
		// 默认窗口尺寸:代码编辑器需要足够宽高(自动 gutter + 状态行 + 工具栏都在默认客户区内)。
		if (m_LastFloatRects.find(panelId) == m_LastFloatRects.end())
			m_LastFloatRects[panelId] = Wui::WuiRect { 220.0f, 150.0f, 900.0f, 620.0f };
		if (!m_Layout.FindFloatMemory(panelId, nullptr))
			m_Layout.FloatMemory.push_back({ panelId, m_LastFloatRects[panelId] });
	}


void EditorShell::OpenScriptEditor(const std::string& logicalPath){
		// 面板渲染中途(内容浏览器双击 / Scripts 面板按钮)不能立刻"建新窗口(新 Vulkan
		// 交换链)+ 改附加标签":用户实测双击 .lua 会触发 vkQueueSubmit 设备丢失、界面黑屏。
		// 统一推迟到下一帧 OnRender 开头(与 AI 通道的帧首执行同为安全点)。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		if (std::find(m_PendingScriptOpen.begin(), m_PendingScriptOpen.end(), normalized) == m_PendingScriptOpen.end())
		{
			m_PendingScriptOpen.push_back(std::move(normalized));
			WLD_CORE_INFO("[script-editor] open request deferred to frame boundary");
		}
	}


std::filesystem::path EditorShell::CurrentProjectRoot() const{
		// 运行期项目根唯一入口 = World::Paths::ProjectDir();有清单才算"打开着项目"
		// (启动器的哨兵目录 / 引擎内空的 projects/ 容器都没有清单 → 空)。
		std::error_code error;
		const std::filesystem::path root = World::Paths::ProjectDir();
		if (root.empty() || !std::filesystem::is_regular_file(root / "project.we.yaml", error))
			return {};
		return root;
	}


bool EditorShell::ResolveCppSourcePath(const std::string& path, std::filesystem::path& out, std::string* error) const{
		auto fail = [error](const std::string& text)
		{
			if (error)
				*error = text;
			return false;
		};
		std::string relative = path;
		// 旧逻辑路径前缀(`module:` + checkout 相对路径):老存档 / 老调用点仍可能带它,
		// 这里只当作"去掉前缀的相对路径",不再有独立的模块源码编辑通道。
		constexpr const char* kModuleSourcePrefix = "module:";
		if (relative.rfind(kModuleSourcePrefix, 0) == 0)
			relative.erase(0, std::strlen(kModuleSourcePrefix));
		if (relative.empty())
			return fail("empty C++ source path");

		std::error_code fileError;
		const std::filesystem::path candidate(relative);
		// 绝对路径(项目源码视图给的就是绝对路径)原样接受。
		if (candidate.is_absolute() || candidate.has_root_name())
		{
			if (std::filesystem::is_regular_file(candidate, fileError))
			{
				out = candidate;
				return true;
			}
			return fail("file not found: " + candidate.string());
		}

		// 相对路径依次按 项目根 → 仓库根 → 内容根 解析(第一条命中的算数)。
		const std::filesystem::path roots[] = {
			CurrentProjectRoot(),
			std::filesystem::path(WLD_REPO_ROOT),
			World::Paths::AssetRoot(),
		};
		std::string tried;
		for (const std::filesystem::path& root : roots)
		{
			if (root.empty())
				continue;
			const std::filesystem::path full = root / candidate;
			if (!tried.empty())
				tried += ", ";
			tried += full.string();
			if (std::filesystem::is_regular_file(full, fileError))
			{
				out = full;
				return true;
			}
		}
		return fail("C++ source not found: " + path + (tried.empty() ? "" : " (tried " + tried + ")"));
	}


void EditorShell::OpenInVisualStudioNow(const std::filesystem::path& absolute){
		std::string message;
		if (m_Editor.OpenInVisualStudio(absolute, &message))
			return;
		PushNotice(Wui::TrFormat("notice.vsopen.failed", "Could not open in Visual Studio: {reason}",
			{ { "reason", message.empty() ? absolute.string() : message } }));
		WLD_CORE_WARN("[vsopen] open failed for '{0}': {1}", absolute.string(), message);
	}


void EditorShell::OpenScriptEditorNow(const std::string& logicalPath){
		// 用户决定(2026-09-18 C):打开即**默认直接附加到主窗口** —— OS 窗口保持隐藏,
		// 主窗口顶栏出现切换标签(点击在主界面/脚本内容之间切换);用户仍可把它拖出成独立窗口。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		// CPPT-7/PROJ-8:C++ 源码(含旧的 `module:` 逻辑路径)不进内置编辑器 —— 改走外部
		// Visual Studio(策略与固定日志见 EditorLayer::OpenInVisualStudio)。
		if (IsCppSourcePath(normalized))
		{
			std::filesystem::path resolved;
			std::string resolveError;
			if (ResolveCppSourcePath(normalized, resolved, &resolveError))
			{
				OpenInVisualStudioNow(resolved);
			}
			else
			{
				PushNotice(Wui::TrFormat("notice.vsopen.failed",
					"Could not open in Visual Studio: {reason}", { { "reason", resolveError } }));
				WLD_CORE_WARN("[vsopen] cannot resolve '{0}': {1}", normalized, resolveError);
			}
			return;
		}
		const std::string panelId = std::string(kScriptPanelPrefix) + normalized;
		EnsureScriptPanelFromId(panelId);

		// 如果面板已存在但先前处于未成功读取磁盘(!IsDiskBacked),重新从磁盘加载一次
		const auto found = m_PanelRegistry.find(panelId);
		if (found != m_PanelRegistry.end() && found->second)
		{
			if (auto* scriptPanel = dynamic_cast<ScriptEditorPanel*>(found->second.get()))
			{
				if (!scriptPanel->IsDiskBacked())
					scriptPanel->LoadFromDisk();
			}
		}

		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panelId) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panelId; // 已打开:重复打开只是激活该标签
			return;
		}
		// 尚未附加:先按独立窗口建出(必要时复用已隐藏的窗口),随后立即挂靠到主窗口。
		if (!FindFloatHost(panelId))
			OpenIndependentPanel(panelId);
		AttachIndependentWindowToSlot(panelId);
	}


void EditorShell::CloseEditorPanel(const std::string& panel){
		if (!m_Ctx || panel.empty())
			return;
		// 与 Window 菜单/顶栏标签关闭同一条路径:已附加 → 摘标签并隐藏窗口;
		// 已浮动 → 隐藏复用;未打开 → 不做事(按钮只在面板可见时存在)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		FloatWindowHost* host = FindFloatHost(panel);
		const bool visibleWindow = host && !host->IsHidden();
		if (attached == m_AttachedPanels.end() && !visibleWindow)
			return;
		TogglePanel(*m_Ctx, panel);
	}


	// ---- P4-U13e:prefab 资产窗口的"未保存改动"守卫 ----
	//
	// 关窗 / 进文档会话都会丢掉窗口里未落盘的编辑,所以先弹项目现成的确认模态问一次
	// (丢弃 / 取消),用户点"丢弃"后才执行被延迟的那个动作。守卫只认 prefab 面板,
	// 其它面板原样透传(不影响既有路径)。

PrefabPanel* EditorShell::PrefabPanelById(const std::string& panelId) const{
		const auto found = m_PanelRegistry.find(panelId);
		if (found == m_PanelRegistry.end())
			return nullptr;
		return dynamic_cast<PrefabPanel*>(found->second.get());
	}


bool EditorShell::InterceptPrefabUnsaved(const std::string& panelId, PrefabPendingAction action, const std::string& logicalPath){
		if (m_PrefabGuardBypass || panelId.empty())
			return false;
		PrefabPanel* panel = PrefabPanelById(panelId);
		if (!panel || !panel->HasUnsavedChanges())
			return false;
		if (m_PrefabPendingAction == action && m_PrefabPendingPanel == panelId)
			return true;   // 已经在等用户回答:不要重复排队
		m_PrefabPendingPanel = panelId;
		m_PrefabPendingLogical = logicalPath;
		m_PrefabPendingAction = action;
		// 独立窗口的 × 会先把 OS 窗口标成 should-close(此后不再渲染,内容会冻在上一帧)。
		// 这里把那个待关闭状态撤销:用户点"取消"后窗口还能继续用(容器唯一会清 should-close
		// 的公开入口是"显示但不激活")。挂靠态的面板 OS 窗口本来就在隐藏复用,跳过。
		if (FloatWindowHost* host = FindFloatHost(panelId); host && !host->IsHidden())
			host->ShowWithoutActivation();
		WLD_CORE_INFO("[prefab] '{0}' has unsaved edits: waiting for the discard confirmation", panelId);
		return true;
	}


void EditorShell::RunPendingPrefabAction(Wui::WuiContext& ctx){
		const std::string panel = m_PrefabPendingPanel;
		const std::string logical = m_PrefabPendingLogical;
		const PrefabPendingAction action = m_PrefabPendingAction;
		m_PrefabPendingPanel.clear();
		m_PrefabPendingLogical.clear();
		m_PrefabPendingAction = PrefabPendingAction::None;
		// 丢弃面板内的编辑(下一次绘制会从磁盘重读),然后执行被拦下的动作。
		if (PrefabPanel* prefab = PrefabPanelById(panel))
			prefab->DiscardUnsavedChanges();
		m_PrefabGuardBypass = true;
		if (action == PrefabPendingAction::Close && !panel.empty())
			TogglePanel(ctx, panel);
		else if (action == PrefabPendingAction::OpenDocument && !logical.empty())
			m_Editor.OpenPrefab(logical);
		m_PrefabGuardBypass = false;
	}


void EditorShell::DrawPrefabUnsavedModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.prefab.unsaved");
		if (m_PrefabPendingAction != PrefabPendingAction::None)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();

		Wui::WuiRect panel;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.prefab.unsaved.title", "Unsaved Prefab Changes");
		frameDesc.Size = { 500.0f, 160.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			return;
		const bool opening = m_PrefabPendingAction == PrefabPendingAction::OpenDocument;
		Label(ctx, { panel.X + 16.0f, panel.Y + 44.0f }, opening
				? Wui::Tr("modal.prefab.unsaved.line1_open", "The full editor loads this asset from disk.")
				: Wui::Tr("modal.prefab.unsaved.line1_close", "This window has unsaved prefab edits."),
			m_Theme.Text, 14.0f);
		Label(ctx, { panel.X + 16.0f, panel.Y + 64.0f }, opening
				? Wui::Tr("modal.prefab.unsaved.line2_open", "Continuing discards the edits made in this window.")
				: Wui::Tr("modal.prefab.unsaved.line2_close", "Closing discards them; Save writes them into the asset."),
			m_Theme.Text, 14.0f);
		Label(ctx, { panel.X + 16.0f, panel.Y + 86.0f }, m_PrefabPendingPanel, m_Theme.TextMuted, 12.0f);
		const std::string discardLabel = Wui::Tr("modal.prefab.unsaved.discard", "Discard");
		const std::string cancelLabel = Wui::Tr("modal.prefab.unsaved.cancel", "Cancel");
		const Wui::WuiId discardId = Wui::HashId("modal.prefab.unsaved.discard");
		const Wui::WuiId cancelId = Wui::HashId("modal.prefab.unsaved.cancel");
		// 按钮几何与 Wui::ModalButtons 的两按钮口径一致(等分、整行居中、gap 8;内边距/按钮高
		// 取 ModalFooter 的公开常量)。按钮自己画而不是走 ModalButtons,原因只有一个:
		// 本帧如果先渲染过可见的独立窗口,WuiAccessibility 的"当前窗口"会停在那个浮窗上
		// (每个窗口在自己的 BeginFrame 里设置它),而模态是画在主窗口里的 —— 跟着"当前窗口"
		// 登记的话,脚本 ui.invoke 会按浮窗去投递点击(实测:点不到)。这里显式写死 main。
		const float buttonGap = 8.0f;
		const float available = std::max(80.0f, panel.W - Wui::ModalFooterPadding * 2.0f);
		const float buttonWidth = std::clamp((available - buttonGap) * 0.5f, 48.0f, 240.0f);
		const float rowWidth = buttonWidth * 2.0f + buttonGap;
		const float buttonX = panel.X + (panel.W - rowWidth) * 0.5f;
		const float buttonY = panel.Y + panel.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const Wui::WuiRect discardRect { buttonX, buttonY, buttonWidth, Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { buttonX + buttonWidth + buttonGap, buttonY, buttonWidth,
			Wui::ModalFooterHeight };
		const auto registerMainButton = [](Wui::WuiId id, const Wui::WuiRect& rect, const std::string& label)
		{
			Wui::WuiAccessNode node;
			node.Id = id;
			node.Window = "main";
			node.Panel = "shell";
			node.Kind = "button";
			node.Label = label;
			node.Rect = rect;
			node.Enabled = true;
			node.Visible = true;
			node.Interactive = true;
			Wui::WuiAccessibility::Get().Register(node);
		};
		registerMainButton(discardId, discardRect, discardLabel);
		registerMainButton(cancelId, cancelRect, cancelLabel);
		const bool discardClicked = Wui::Button(ctx, discardId, discardRect, discardLabel, m_Theme);
		const bool cancelClicked = Wui::Button(ctx, cancelId, cancelRect, cancelLabel, m_Theme);
		if (discardClicked)
		{
			RunPendingPrefabAction(ctx);
		}
		else if (cancelClicked || escapePressed)
		{
			// 取消:窗口/资产原样不动,未保存改动保留。
			m_PrefabPendingPanel.clear();
			m_PrefabPendingLogical.clear();
			m_PrefabPendingAction = PrefabPendingAction::None;
		}
		Wui::EndModalFrame(ctx);
	}


	// W5-L1:文档场景外部改动提示(视口面板读;按钮触发重开,未保存时走确认模态)。
bool EditorShell::ExternalSceneChanged() const{
		return m_Editor.ExternalSceneChanged();
	}


void EditorShell::ReopenExternalScene(){
		m_Editor.ReopenExternalScene();
	}


bool EditorShell::InstantiateModelFile(const std::string& logicalPath, std::string* message){
		return m_Editor.InstantiateModelFile(logicalPath, message);
	}


bool EditorShell::SaveProjectRenderSettings(const Asset::RenderingSettings& settings, std::string* message){
		// 写盘口径与其它"回写清单"的地方一致:先 Load(保留 id/场景/包列表等字段),
		// 只覆盖 rendering,再 Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Rendering = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存渲染设置到 " + manifestPath.filename().string();
		return true;
	}


bool EditorShell::SaveProjectPhysicsSettings(const Asset::PhysicsSettingsData& settings, std::string* message){
		// 与渲染设置同一套"回写清单"口径:Load → 只覆盖 physics → Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Physics = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存物理设置到 " + manifestPath.filename().string();
		return true;
	}


bool EditorShell::SaveProjectImportDefaults(const Asset::ModelImportSettings& settings, std::string* message){
		// P4-U4:与渲染/物理同一套"回写清单"口径(Load → 只覆盖 imports → Save)。
		// 额外一步:写进进程级默认,让**本次会话之后的新导入**立刻按新默认走。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.ImportDefaults = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		Asset::ModelImportSettings::SetProjectDefaults(settings);
		if (message)
			*message = "已保存导入默认值到 " + manifestPath.filename().string();
		return true;
	}


	// P4-UX11:项目启动项(renderer / start_scene / content_root)。
	// 与渲染/物理同一套"回写清单"口径:Load(保留其它字段与注释风格) → 只覆盖这三项 → Save。
bool EditorShell::SaveProjectStartupSettings(const std::string& renderer, const std::string& startScene, const std::string& contentRoot, std::string* message){
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		if (!renderer.empty())
			manifest.Renderer = renderer;
		manifest.StartScene = startScene;
		if (!contentRoot.empty())
			manifest.ContentRoot = contentRoot;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存启动项到 " + manifestPath.filename().string();
		return true;
	}


std::vector<std::string> EditorShell::ListProjectScenes(){
		// 内容根下的 .wd 场景(逻辑路径,与 manifest 的 start_scene 同一口径:相对 content_root)。
		std::vector<std::string> scenes;
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			return scenes;
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
			return scenes;
		const std::filesystem::path root = manifest.ResolveContentRoot(manifestPath);
		std::error_code code;
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(root, code))
		{
			if (code)
				break;
			if (!entry.is_regular_file(code) || entry.path().extension() != ".wd")
				continue;
			scenes.push_back(std::filesystem::relative(entry.path(), root, code).generic_string());
		}
		std::sort(scenes.begin(), scenes.end());
		return scenes;
	}


void EditorShell::ApplyProjectRendererChange(const std::string& renderer){
		// 运行中热切换会串资源(GL 名字/描述符集跨上下文复用) → EditorLayer 会写回请求并重启编辑器。
		m_Editor.ApplyRendererChange(renderer);
	}


bool EditorShell::SaveProjectPackages(const std::vector<std::string>& packages, std::string* message){
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Packages = packages;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存发行包列表(" + std::to_string(packages.size()) + " 项)";
		return true;
	}


bool EditorShell::ImportModelFile(const std::string& sourcePath, std::string* message, std::string* outLogicalModel){
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel);
	}


bool EditorShell::ImportModelFileTo(const std::string& sourcePath, const std::string& destinationLogicalDir, std::string* message, std::string* outLogicalModel){
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel, destinationLogicalDir);
	}


EditorPanel* EditorShell::FocusedPanel(){
		// ① 主窗口处于"已附加面板"模式(顶栏标签):该面板就是焦点面板。
		if (!m_ActiveWindowTag.empty())
		{
			const auto found = m_PanelRegistry.find(m_ActiveWindowTag);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ② 独立窗口在前台:该窗口当前标签的面板。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden() || !host->IsFocused())
				continue;
			const auto found = m_PanelRegistry.find(host->ActivePanel());
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ③ 文本焦点所属面板(脚本编辑器 / 搜索框 / 重命名框等)。
		const std::string& textPanel = Wui::WuiTextFocus::Get().Panel();
		if (!textPanel.empty())
		{
			const auto found = m_PanelRegistry.find(textPanel);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		return nullptr;
	}


void EditorShell::DockBackIndependentWindow(const std::string& panel){
		// 收回整窗:记住用户调好的尺寸,销毁独立窗口并把它的全部标签挂回停靠树。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁(运行期销毁窗口会崩)

		const Wui::PanelId anchor = m_Layout.FirstPanel();
		for (const std::string& id : panels)
		{
			// 停靠树为空时无法挂载:仅隐藏该面板(CloseFloating 会留下跨会话位置记忆)。
			if (!anchor.empty() && m_Layout.DockFloating(id, anchor, Wui::DropZone::Center))
				WLD_CORE_INFO("Independent window docked back: {0}", id);
			else
				m_Layout.CloseFloating(id);
		}
	}


void EditorShell::AttachIndependentWindowToSlot(const std::string& panel){
		// 挂靠整窗:窗口的全部面板作为槽位所在标签组的成员回到主窗口,OS 窗口销毁。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		// 只有声明为"独立窗口"的面板能挂靠到顶部挂靠栏;停靠形态的临时浮动不允许挂靠。
		if (!IsIndependentPanel(panel))
		{
			WLD_CORE_INFO("[float] panel '{0}' is a docked panel; attach ignored", panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		// 附加 = 与主窗口建立"标签切换"关系:窗口与其面板保持不变,只隐藏 OS 窗口;
		// 主窗口顶栏出现该标签,点击即在 主界面 / 该窗口内容 之间切换。
		host->SetHidden(true);
		// 顶栏标签 = 一枚 OS 窗口(不是一枚面板):先把同一窗口里其它标签页的旧记录摘掉,
		// 再补上本次的面板 —— 否则会出现"一个窗口两枚 chip",拖出其中一枚会把整个窗口拔走。
		for (const std::string& id : panels)
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
				m_AttachedPanels.end());
		m_AttachedPanels.push_back(panel);
		m_ActiveWindowTag = panel;
		WLD_CORE_INFO("Independent window attached as switch tab: {0} (still in dock tree: {1})",
			panel, m_Layout.Contains(panel) ? "yes" : "no");

		// 不再把面板并入停靠树(那是旧"挂靠"语义);附加只建立标签切换关系。
		m_AttachSlotHighlight = false;
		m_DragPanel.clear();
		m_MovingFloat.clear();
		m_TabDragPanel.clear();
		m_LastDragPos = { 0, 0 };
		m_AttachCooldownFrames = 45;
		WLD_CORE_INFO("Independent window attached to slot: {0} ({1} panels)", panel, panels.size());
		SaveLayout();   // 立刻落盘"这次是挂靠形态",下次启动才能按形态恢复/询问(P4-UX10)
	}


	// 隐藏单个面板(标签栏 x / Window 菜单):从所属窗口摘除,窗口为空则销毁。
void EditorShell::HideFloatPanel(const std::string& panel, Wui::WuiContext* ctx){
		// P4-U13e:prefab 面板有未保存改动时先问一次(丢弃 / 取消),不静默丢。
		if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
			return;
		const std::string before = m_Layout.Serialize();
		if (FloatWindowHost* host = FindFloatHost(panel))
		{
			// 写回最后位置:窗口内其余标签仍由它们自己的布局记录继续跟踪。
			const Wui::WuiRect rect = host->ScreenRect();
			if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
				entry->Rect = rect;
			m_LastFloatRects[panel] = rect;
			host->RemovePanel(panel);
			if (host->Empty())
				host->SetHidden(true); // 复用:隐藏而非销毁
		}
		m_LastFloatScreenRects.erase(panel);
		// 独立形态:隐藏即从浮动记录移除(Window 菜单可重开);
		// 停靠形态:临时拖出的标签关闭 = 回停靠位(D3)。
		const bool changed = IsIndependentPanel(panel)
			? m_Layout.CloseFloating(panel)
			: DockPanelBackToTree(panel);
		if (changed && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}


	// 整窗关闭(用户关闭/渲染失败):窗口内全部面板隐藏,布局写回并记录操作。
void EditorShell::CloseFloatWindow(const std::string& panel, bool recordChange, Wui::WuiContext* ctx){
		// P4-U13e:同上(OS 窗口关闭 / 挂靠标签 × 都从这里过)。
		if (InterceptPrefabUnsaved(panel, PrefabPendingAction::Close))
			return;
		const std::string before = m_Layout.Serialize();
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
		{
			// 宿主已不存在:仅清理浮动记录,避免残留"看不见的窗口"。
			if (m_Layout.CloseFloating(panel) && recordChange && ctx)
				RecordDockChange(*ctx, "hide", panel, before);
			// 停靠形态的"临时浮动"关闭后应回停靠位(独立形态保持隐藏)。
			if (!IsIndependentPanel(panel))
				DockPanelBackToTree(panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁
		bool changed = false;
		for (const std::string& id : panels)
		{
			if (Wui::DockFloat* entry = m_Layout.FindFloat(id))
				entry->Rect = rect;
			// 独立形态:关闭 = 隐藏复用(移除浮动记录,便于 Window 菜单重开)。
			// 停靠形态:临时拖出不跨会话,直接回停靠树(D3)。
			if (IsIndependentPanel(id))
			{
				m_LastFloatRects[id] = rect;
				changed = m_Layout.CloseFloating(id) || changed;
			}
			else
			{
				changed = DockPanelBackToTree(id) || changed;
			}
		}
		if (changed && recordChange && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}


void EditorShell::DrawMenuBar(Wui::WuiContext& ctx){
		const glm::vec2 viewport = ctx.ViewportSize();

		// 菜单栏属于"当前窗口":切到已附加的独立窗口内容时,显示它自己的菜单栏
		// (Widget 目前为空),而不是主窗口的 File/Window 菜单。
		if (!m_ActiveWindowTag.empty())
		{
			Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
			return;
		}

		// Header = 分组小标题行:只显示、不响应悬停/点击(菜单里区分"独立窗口/停靠面板")。
		// Tooltip 只给"看名字猜不到全部含义"的项(悬停解释 + 同步进无障碍节点的 Tooltip)。
		struct MenuEntry
		{
			std::string Label;
			bool Checked;
			std::function<void()> Action;
			bool Header = false;
			std::string Tooltip;
			// CPPT-3:显式无障碍 id(空 = 沿用"菜单名 + 本地化标签"的既有派生口径)。
			// 需要跨语言稳定 id 的项(如 menu.file.build_reload_cpp_module)必须显式给。
			Wui::WuiId ExplicitId = 0;
			// PROJ-2/T1:失效的"最近项目"行灰显(可读不可点);其余菜单项默认可用。
			bool Enabled = true;
		};

		const Wui::WuiId menuFile = Wui::HashId("menu.file");
		const Wui::WuiId menuWindow = Wui::HashId("menu.window");
		const Wui::WuiId menuRun = Wui::HashId("menu.run");
		if (!m_MenuBar)
		{
			m_MenuBar = std::make_shared<Wui::WuiBox>();
			m_MenuBar->Direction = Wui::WuiDirection::Row;
			m_MenuBar->Gap = 4;
			m_FileButton = std::make_shared<Wui::WuiButton>();
			// P4-U7:菜单栏按钮给稳定 id —— 对象式按钮按 id 进无障碍树,AI 通道
			// (`ui.invoke`)才能打开菜单;没有 id 的按钮只登记焦点、脚本点不到。
			m_FileButton->SetId(menuFile);
			m_FileButton->Label = Wui::Tr("menu.file", "File");
			m_FileButton->OnClick = [this, menuFile]
				{
					const bool opening = m_OpenMenu != menuFile;
					m_OpenMenu = opening ? menuFile : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_FileButton->Rect();
						m_Ctx->OpenPopup(menuFile);
					}
					else
						m_Ctx->ClosePopup(menuFile);
				};
			m_MenuBar->Add(m_FileButton, { 60, 60, 0, 22, 0 });
			m_WindowButton = std::make_shared<Wui::WuiButton>();
			m_WindowButton->SetId(menuWindow);
			m_WindowButton->Label = Wui::Tr("menu.window", "Window");
			m_WindowButton->OnClick = [this, menuWindow]
				{
					const bool opening = m_OpenMenu != menuWindow;
					m_OpenMenu = opening ? menuWindow : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_WindowButton->Rect();
						m_Ctx->OpenPopup(menuWindow);
					}
					else
						m_Ctx->ClosePopup(menuWindow);
				};
			m_MenuBar->Add(m_WindowButton, { 78, 78, 0, 22, 0 });
			// PROJ-2/T1:运行菜单 —— 独立 Runtime 进程的"启动项目"入口(不重启编辑器)。
			m_RunButton = std::make_shared<Wui::WuiButton>();
			m_RunButton->SetId(menuRun);
			m_RunButton->Label = Wui::Tr("menu.run", "Run");
			m_RunButton->OnClick = [this, menuRun]
				{
					const bool opening = m_OpenMenu != menuRun;
					m_OpenMenu = opening ? menuRun : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_RunButton->Rect();
						m_Ctx->OpenPopup(menuRun);
					}
					else
						m_Ctx->ClosePopup(menuRun);
				};
			m_MenuBar->Add(m_RunButton, { 64, 64, 0, 22, 0 });
		}

		// 菜单栏下移一行:顶部第一行现在是挂靠栏。
		Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
		// P4-U8:菜单栏/挂靠栏/模态都属于**外壳**(不属于最后渲染的那个面板)。
		// 不显式切面板时,菜单项的无障碍节点会挂着上一个面板 id(实测:View 菜单项被标成
		// panel="properties"),脚本按面板过滤就找不到它。
		Wui::WuiAccessibility::Get().SetPanel("shell");
		Wui::LayoutWidgetTree(m_MenuBar, { 8, 28, viewport.x - 16, 22 });
		Wui::WuiPaintContext paint(ctx);
		m_MenuBar->Paint(paint);

		// M4-S2 遗留①(另一半):任何**不是**"点外面关闭"的收口路径(别的弹层打开时的
		// CloseAllPopups、面板里的 Esc、脚本 ui.close…)都会把弹层关掉而 m_OpenMenu 仍留着;
		// 那会让同一个按钮下一次点击被判成"已经在开" → 又是按两次。每帧按弹层真实状态对齐:
		// 开着的那个是唯一事实源,弹层没了就把归属清零。放在按钮绘制之后(刚打开那一帧
		// 弹层已 Open,不会被误清)。
		if (m_OpenMenu != 0 && !ctx.IsPopupOpen(m_OpenMenu))
			m_OpenMenu = 0;

		auto drawMenu = [&](const Wui::WuiId menuId, const std::string& menuIdName, const std::vector<MenuEntry>& entries)
		{
			if (ctx.IsPopupOpen(menuId))
			{
				ctx.PushOverlay();
				// M4-S2 遗留②:菜单栏下拉的**绘制命令**延后到帧末(模态画完之后),与 U29 给
				// Combo/SearchableCombo 的 `WuiDeferredPopupScope` 同一口径 —— 菜单栏在模态之前
				// 绘制,否则模态一开就把下拉盖住(同类 z-order 风险)。命中测试、Tooltip、
				// RegisterOverlayRect 与关闭判定全部留在原地当场生效(命令只是搬走像素)。
				std::vector<Wui::WuiDrawCommand>& menuOverlay = ctx.Commands();
				const size_t menuCommandMark = menuOverlay.size();
				const Wui::WuiRect panel { m_MenuHeaderRect.X, m_MenuHeaderRect.Y + m_MenuHeaderRect.H + 2, 240, static_cast<float>(entries.size() * 22 + 8) };
				DrawPanelSurface(ctx, panel, m_Theme);
				// P4-U7:菜单矩形登记为覆盖层 —— 下一帧面板内容不会吃掉落在菜单上的点击
				// (实测:菜单项那一下会同时穿透到下面的面板控件)。
				ctx.RegisterOverlayRect(panel);
				for (size_t i = 0; i < entries.size(); ++i)
				{
					const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
					if (entries[i].Header)
					{
						// 分组标题:淡色小字,不可点击。
						Label(ctx, { item.X + 4.0f, item.Y + 4.0f }, entries[i].Label, m_Theme.TextMuted, 12.0f);
						// 分组标题也要进无障碍树 —— 可见但读不到,读屏/脚本就看不出菜单的分组结构。
						Wui::WuiAccessNode header;
						header.Id = Wui::HashId((menuIdName + ".group." + std::to_string(i)).c_str());
						header.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						header.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						header.Kind = "text";
						header.Label = entries[i].Label;
						header.Rect = item;
						header.Interactive = false;
						Wui::WuiAccessibility::Get().Register(header);
						continue;
					}
					const Wui::WuiId itemId = entries[i].ExplicitId != 0
						? entries[i].ExplicitId
						: Wui::HashId((menuIdName + "." + entries[i].Label).c_str());
					// P4-U8:悬停解释(勾选项的"关掉是什么效果"这类信息读不出来,只能靠提示)。
					if (!entries[i].Tooltip.empty())
						Wui::Tooltip(ctx, item, entries[i].Tooltip);
					if (MenuItem(ctx, itemId, item, entries[i].Label, entries[i].Checked, entries[i].Enabled, m_Theme))
					{
						entries[i].Action();
						ctx.RecordOp("menu", "item", entries[i].Label, menuIdName);
						ctx.CloseAllPopups();
						m_OpenMenu = 0;
					}
					// 无障碍:MENU 项说明必须同时进节点 —— 读屏/脚本看不到悬停提示。
					if (!entries[i].Tooltip.empty())
					{
						Wui::WuiAccessNode node;
						node.Id = itemId;
						node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						node.Kind = "menu-item";
						node.Label = entries[i].Label;
						node.Value = entries[i].Checked ? "checked" : "unchecked";
						node.Tooltip = entries[i].Tooltip;
						node.Rect = item;
						node.Enabled = entries[i].Enabled;
						node.Interactive = entries[i].Enabled;
						Wui::WuiAccessibility::Get().Register(node);
					}
				}
				ctx.ClosePopupsOnOutsideClick({ menuId }, panel);
				// M4-S2 遗留①:点外面关闭后必须**同步清零 m_OpenMenu** —— 否则下一次按同一个
				// 菜单按钮时 `opening = (m_OpenMenu != menuFile)` 判成"已经开着" → 只发关闭、
				// 菜单不出现,用户得按两次(实测)。这里按**弹层真实状态**收口(不看返回值:
				// ClosePopupsOnOutsideClick 在"弹层是本帧刚打开"时会跳过关闭但同样返回 true)。
				if (m_OpenMenu == menuId && !ctx.IsPopupOpen(menuId))
					m_OpenMenu = 0;
				if (ctx.IsKeyPressed(KeyCodes::Escape))
				{
					ctx.ClosePopup(menuId);
					m_OpenMenu = 0;
				}
				// 只搬"这一帧仍然开着"的菜单:菜单项动作(打开模态等)会在本帧调 CloseAllPopups,
				// 那一帧的命令必须丢弃 —— 否则刚弹出的模态会被上一帧的下拉盖一帧。
				if (ctx.IsPopupOpen(menuId))
				{
					m_DeferredMenuCommands.insert(m_DeferredMenuCommands.end(),
						menuOverlay.begin() + static_cast<std::ptrdiff_t>(menuCommandMark), menuOverlay.end());
				}
				menuOverlay.resize(menuCommandMark);
				ctx.PopOverlay();
			}
		};

		std::vector<MenuEntry> fileEntries;
		fileEntries.push_back({ Wui::Tr("menu.file.new", "New"), false, [this] { m_Editor.NewScene(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.open", "Open"), false, [this] { m_Editor.OpenScene(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.save", "Save"), false, [this] { m_Editor.SaveScene(); } });
		// PROJ-2/T1:File ▸ Open Project…(选目录 → 校验 project.we.yaml → 重启到该项目)。
		// 原生对话框推迟到下一帧开头(见 OnRender 的 m_OpenProjectBrowsePending 分支)。
		fileEntries.push_back({ Wui::Tr("menu.file.open_project", "Open Project…"), false,
			[this]
			{
				if (m_Ctx)
					RequestOpenProjectBrowse(*m_Ctx);
			}, false,
			Wui::Tr("menu.file.open_project.tooltip",
				"Open a project from any folder: pick the project root (the folder that contains "
				"project.we.yaml), validate the manifest, then the editor restarts into it."),
			Wui::HashId("menu.file.open_project") });
		// PROJ-1/T1:任意位置新建**标准**项目(干净骨架,不含示例)。稳定 id
		// menu.file.new_project:AI 通道/自动化按 id 点它,不依赖标签语言。
		fileEntries.push_back({ Wui::Tr("menu.file.new_project", "New Project…"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewProjectModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_project.tooltip",
					"Create a standard project skeleton (no samples) at any location: project name + "
					"location → manifest, content root and a minimal runnable scene (camera + directional "
					"light, can be turned off); then open it in Explorer, restart the editor into it, or "
					"launch it."),
				Wui::HashId("menu.file.new_project") });
		// PLUG-AUTH-1:File ▸ New Plugin…(引擎插件 / 项目插件;6 类模板;同一个脚手架)。
		// 稳定 id menu.file.new_plugin:AI 通道/自动化按 id 点它,不依赖标签语言。
		fileEntries.push_back({ Wui::Tr("menu.file.new_plugin", "New Plugin…"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewPluginModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_plugin.tooltip",
					"Create a plugin package (manifest + source + CMake) from one of the built-in "
					"templates: engine plugins land in <engine>/plugins, project plugins in "
					"<project>/plugins. The manifest is validated by the plugin loader before the "
					"files land."),
				Wui::HashId("menu.file.new_plugin") });
		// PROJ-2/T1:Recent Projects 分区(最多 10 条;失效项灰显,仍带完整路径与原因提示)。
		// 只在有记录时出现;列表在弹出后节流刷新(见 RefreshRecentProjectsIfStale)。
		if (ctx.IsPopupOpen(menuFile))
		{
			RefreshRecentProjectsIfStale();
			const size_t recentCount = std::min<size_t>(m_RecentProjects.size(), 10);
			if (recentCount > 0)
			{
				MenuEntry header;
				header.Label = Wui::Tr("menu.file.recent", "Recent Projects");
				header.Checked = false;
				header.Header = true;
				fileEntries.push_back(std::move(header));
			}
			for (size_t i = 0; i < recentCount; ++i)
			{
				const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
				MenuEntry item;
				item.Label = entry.Name.empty() ? entry.Path : entry.Name;
				item.Checked = false;
				item.Enabled = entry.Valid;
				const std::string path = entry.Path;
				item.Action = [this, path] { m_Editor.RelaunchWithProject(std::filesystem::u8path(path)); };
				item.Tooltip = entry.Valid ? path : path + " — " + entry.InvalidReason;
				item.ExplicitId = Wui::HashId(("menu.file.recent." + std::to_string(i)).c_str());
				fileEntries.push_back(std::move(item));
			}
		}
		fileEntries.push_back({ Wui::Tr("menu.file.import", "Import glTF..."), false,
			[this] { m_Editor.ImportModelDialog(); } });
		// PECS-T8/T11:组件/系统/空 共用同一个「新建 C++ …」向导(id 与既有自动化口径保持不变)。
		fileEntries.push_back({ Wui::Tr("menu.file.new_cpp_script", "New C++ …"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewCppScriptModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_cpp_script.tooltip",
					"Create a pure-ECS C++ component (data-only struct, src/Components/), a system "
					"(logic, src/Systems/; automatically registered in <project>/src/GameProject.cpp) or a "
					"blank header (src/<Name>.h) and open it in Visual Studio. Build the project, then use "
					"File ▶ Build & Reload C++ Module to load it."),
				Wui::HashId("menu.file.new_cpp_script") });
		// PECS-T9/T11:Lua 创建向导 —— 一个入口 + 类型下拉(系统 / 脚本库 / 空;与「新建 C++ …」同构)。
		fileEntries.push_back({ Wui::Tr("menu.file.new_lua", "New Lua …"), false,
				[this]
				{
					if (m_Ctx)
						OpenNewLuaSystemModal(*m_Ctx);
				}, false,
				Wui::Tr("menu.file.new_lua.tooltip",
					"Create a Luau script and open it in the built-in script editor. Kind: system "
					"(<content>/scripts/systems/ — the only folder auto-loaded on Play), library "
					"(<content>/scripts/lib/) or blank (<content>/scripts/). No build step needed."),
				Wui::HashId("menu.file.new_lua") });
		// CPPSRC-1(用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」):
		// 项目 C++ 的唯一展示面 = 内容浏览器的 `Project C++` 根 —— 这一项不再打开 Scripts 面板
		// 的列表段,而是把内容浏览器切到源码根(同一套网格/列表/类型列/搜索)。
		fileEntries.push_back({ Wui::Tr("menu.file.project_sources", "Project Sources…"), false,
				[this]
				{
					if (!m_Ctx)
						return;
					FocusContentBrowserProjectSources();
				}, false,
				Wui::Tr("menu.file.project_sources.tooltip",
					"Show this project's C++ sources (<project>/src) in the Content Browser — same grid or "
					"list, type column and search as assets; double-click a file to open it in Visual Studio."),
				Wui::HashId("menu.file.project_sources") });
		// PROJ-11/T1 + PROJ-12/T2:已存在的项目(向导旧版本建出来的)同步
		// CMakeLists.txt / build.cmd / CMakePresets.json(旧版备份 .bak 后升级)、
		// 两个 exe 启动器、.we/engine-root.txt、.vs/launch.vs.json
		// (PROJ-15/T1:锚定项目自己的 ProjectRun,.vs/ProjectSettings.json 设成当前启动项;
		// 根级旧版 launch.vs.json 清掉)与 .gitignore。
		// 不碰 src 与 assets。
		fileEntries.push_back({ Wui::Tr("menu.file.generate_build_entry",
				"Generate Build Entry Points (CMake + build.cmd)"), false,
				[this]
				{
					if (!m_Ctx)
						return;
					const std::filesystem::path projectRoot = CurrentProjectRoot();
					if (projectRoot.empty())
					{
						PushNotice(Wui::Tr("notice.build_entry.no_project",
							"No project is open — open or create one first, then generate its build entry points."));
						return;
					}
					m_BuildEntryResult = Editor::ProjectScaffolder::EnsureBuildEntryPoints(projectRoot);
					m_BuildEntryOpen = true;
					m_Ctx->RecordOp("project", "build-entry", projectRoot.filename().u8string(),
						m_BuildEntryResult.Ok ? "ok" : "failed");
				}, false,
				Wui::Tr("menu.file.generate_build_entry.tooltip",
					"For the current project: sync CMakeLists.txt / build.cmd / CMakePresets.json from the "
					"engine template (an older version is backed up as <name>.bak-<timestamp> and existing "
					"backups are never overwritten; legacy root .cmd launchers move into .we/ as "
					"legacy-*.bak, or stay in place when there is no replacement launcher exe), refresh "
					"<project>-Edit.exe / <project>-Play.exe, write .we/engine-root.txt (plus a .cmd fallback "
					"in .we/ only when a launcher exe is missing) and .vs/launch.vs.json (one Visual Studio "
					"CMake launch item anchoring the project's own ProjectRun target, which opens the editor "
					"by default; .vs/ProjectSettings.json gets CurrentProjectSetting pointed at it while the "
					"keys Visual Studio already wrote are kept; a leftover root-level launch.vs.json from "
					"the older generator is removed), "
					"then top up .gitignore. Never touches src/ or assets/."),
				Wui::HashId("menu.file.generate_build_entry") });
		// HOTR-P3-T7:一步完成"卸载 + 构建 + 加载" —— 与 AI `module.build_reload` 是同一条
		// EditorLayer::BuildAndReloadCppModule 入口(用户在编辑器内不再需要切到 VS/CMake 构建)。
		// 2026-10-01 用户口径:旧的"分步 Reload C++ Module"菜单项已移除(AI 侧仍保留
		// `module.unload`/`module.reload` 两段式给自动化用)。
		fileEntries.push_back({ Wui::Tr("menu.file.build_reload_cpp_module", "Build & Reload C++ Module (Game.dll)"),
				false,
				[this] { m_Editor.BuildAndReloadCppModule(); }, false,
				Wui::Tr("menu.file.build_reload_cpp_module.tooltip",
					"Unload Game.dll, run this project's own build.cmd in the background, then load the "
					"new build (a build failure keeps the module unloaded and shows the build output in "
					"the log). One build at a time."),
				Wui::HashId("menu.file.build_reload_cpp_module") });
		fileEntries.push_back({ Wui::Tr("menu.file.project_settings", "Project Settings"), false, [this]
				{
					// P4-UX11:项目设置只有一处入口 = 独立窗口的 Settings 面板(渲染/物理/启动与内容)。
					// 旧的"只有渲染后端一项"的模态框已删除,避免两套界面互相打架。
					if (m_Ctx)
						TogglePanel(*m_Ctx, "settings");
				} });
		fileEntries.push_back({ Wui::Tr("menu.file.editor_settings", "Editor Preferences"), false, [this, &ctx]
				{
					if (!FindFloatHost("prefs") && !m_Layout.Contains("prefs"))
						OpenIndependentPanel("prefs");
					TogglePanel(ctx, "prefs");
				} });
		fileEntries.push_back({ Wui::Tr("menu.file.lua_stubs", "Generate Lua API Stubs"), false,
			[this] { m_Editor.GenerateLuaStubsAction(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.cooking", "Cooking"), false,
			[this] { m_Editor.StartCookingAction(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.export_ops", "Export Operation Log"), false,
			[this] { m_Editor.ExportOperationLog(); } });
		fileEntries.push_back({ Wui::Tr("menu.file.exit", "Exit"), false, [this] { m_Editor.CloseAction(); } });
		drawMenu(menuFile, "menu.file", fileEntries);

		// PROJ-2/T1:运行 ▸ 启动项目(Runtime)—— 独立 Runtime 进程跑当前项目的 start_scene,
		// 不重启编辑器、不需要先进 Play。
		drawMenu(menuRun, "menu.run", {
			{ Wui::Tr("menu.run.launch_project", "Launch Project (Runtime)"), false,
				[this] { LaunchCurrentProjectRuntime(); }, false,
				Wui::Tr("menu.run.launch_project.tooltip",
					"Start the current project in a separate Runtime process (manifest start_scene). "
					"The editor keeps running; no restart and no Play session needed."),
				Wui::HashId("menu.run.launch_project") },
		});

		// 菜单分组:独立窗口(自带 OS 窗口)与停靠面板分开列,并给出不可点击的分组标题 ——
		// 之前两类平铺在一起,用户看不出"Gallery/Input Map 是独立窗口,其余是停靠标签"。
		std::vector<MenuEntry> windowEntries;
		// U25-M2:写材质的入口 —— "从零新建一份材质"(模板 + 名称 + 目录 + 实时落点),
		// 与内容浏览器空白右键的 New ▸ Material… 共用同一个向导(实现全在内容浏览器面板)。
		windowEntries.push_back({ Wui::Tr("menu.window.new_material", "New Material…"), false,
			[this] {
				std::string message;
				if (!OpenNewMaterialWizard(false, &message))
					Notify(message.empty() ? std::string("could not open the new-material wizard") : message);
			}, false,
			Wui::Tr("menu.window.new_material.tooltip",
				"New Material…: pick a template, name it, choose a folder — then it opens in the "
				"material editor.") });
		const auto appendPanels = [this, &windowEntries, &ctx](bool independent)
		{
			for (const std::string& panel : m_Panels)
			{
				if (IsIndependentPanel(panel) != independent)
					continue;
				const bool visible = m_Layout.Contains(panel) || m_Layout.IsFloating(panel) ||
					std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end();
				windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
			}
		};
		windowEntries.push_back({ Wui::Tr("menu.window.independent", "Independent Windows"), false, {}, true });
		appendPanels(/*independent=*/true);
		windowEntries.push_back({ Wui::Tr("menu.window.docked", "Docked Panels"), false, {}, true });
		appendPanels(/*independent=*/false);
		windowEntries.push_back({ Wui::Tr("menu.window.reset_layout", "Reset Layout"), false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu(menuWindow, "menu.window", windowEntries);

		// P4-U8a:菜单栏不再有 View 菜单 —— "看"的开关**全部搬进视口自己的悬浮 `视图 ▼`**
		// (用户 2026-09-21:「视图菜单栏的功能能否放到 view 里」;Unity 的 Scene 视图工具条/
		// Unreal 的 Show 菜单就是这个位置)。实现见 ViewportPanel::OnRender。
	}


	// ---- PROJ-11/T1:File ▸ 生成项目构建入口 的结果模态 ----
	// 落盘动作已在菜单回调里同步执行(纯文件复制,毫秒级);这里只显示结果 +
	// 两条可复制的指引(build.cmd → 双击 <项目名>-Play.exe / 运行 ▸ 启动项目)。
void EditorShell::DrawBuildEntryModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.build_entry");
		if (!m_BuildEntryOpen)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			return;
		}
		ctx.SetModal(modalId);

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.build_entry.title", "Project Build Entry Points");
		// PROJ-12F/T2:多出"备份(时间戳)"与"旧 .cmd 迁移到 .we/"两行,框加高。
		frameDesc.Size = { 720.0f, 420.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被更高优先级的路径接管:同步清掉状态,避免下一帧再抢焦点。
			m_BuildEntryOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		float cursorY = frame.Y + 44.0f;
		const auto join = [](const std::vector<std::string>& values)
		{
			std::string text;
			for (const std::string& value : values)
			{
				if (!text.empty())
					text += ", ";
				text += value;
			}
			return text;
		};

		const std::string projectName = m_BuildEntryResult.ProjectRoot.filename().u8string();
		const std::string projectRoot = m_BuildEntryResult.ProjectRoot.u8string();
		// 复制到剪贴板的完整指引(与下面画出来的两步同文)。
		const std::string guidance = Wui::TrFormat("modal.build_entry.guidance",
			"Project root ({root}): run build.cmd; double-click {name}-Edit.exe / {name}-Play.exe. "
			"Other entry files live in .we/ (engine-root.txt records the engine root; a .cmd fallback "
			"appears there only when a launcher exe is missing; legacy root .cmd launchers are moved "
			"there as legacy-*.bak, never deleted). Visual Studio has one launch item "
			"(.vs/launch.vs.json): ProjectRun.exe — it builds Game first, then opens the editor; add "
			"`--play` (or double-click {name}-Play.exe) to run the game instead.",
			{ { "root", projectRoot }, { "name", projectName } });

		if (!m_BuildEntryResult.Ok)
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.error",
				"Could not generate the build entry points: {reason}",
				{ { "reason", m_BuildEntryResult.Error } }), m_Theme.Accent, 13.0f);
			const Wui::ModalButtonDesc buttons[1] = {
				{ Wui::Tr("modal.build_entry.close", "Close"),
					Wui::HashId("modal.build_entry.close"), true },
			};
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 1, m_Theme);
			if (clicked == 0 || escapePressed)
			{
				m_BuildEntryOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		if (!m_BuildEntryResult.Created.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.created",
				"Created: {files}", { { "files", join(m_BuildEntryResult.Created) } }),
				m_Theme.Text, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Upgraded.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.upgraded",
				"Upgraded: {files}",
				{ { "files", join(m_BuildEntryResult.Upgraded) } }),
				m_Theme.Text, 13.0f);
			cursorY += 20.0f;
		}
		// PROJ-12F/T2:备份名带时间戳(绝不覆盖旧备份)—— 列出实际文件,便于用户找回。
		// 名字里带时间戳 + 项目前缀,一行一份(多份挤一行会被右边缘裁掉);超过 4 份折叠。
		if (!m_BuildEntryResult.Backups.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.backups",
				"Previous versions backed up (timestamped, never overwritten):"),
				m_Theme.TextMuted, 12.0f);
			cursorY += 18.0f;
			constexpr size_t kMaxListedBackups = 4;
			const size_t listedBackups = std::min(kMaxListedBackups, m_BuildEntryResult.Backups.size());
			for (size_t index = 0; index < listedBackups; ++index)
			{
				Wui::Label(ctx, { labelX + 16.0f, cursorY }, m_BuildEntryResult.Backups[index],
					m_Theme.TextMuted, 12.0f);
				cursorY += 16.0f;
			}
			if (listedBackups < m_BuildEntryResult.Backups.size())
			{
				Wui::Label(ctx, { labelX + 16.0f, cursorY }, Wui::TrFormat("modal.build_entry.backups_more",
					"… and {count} more (see the editor log)",
					{ { "count", std::to_string(m_BuildEntryResult.Backups.size() - listedBackups) } }),
					m_Theme.TextMuted, 12.0f);
				cursorY += 16.0f;
			}
		}
		// PROJ-12F/T2:根级旧 .cmd 是"先备份再迁走",不是静默删除。
		if (!m_BuildEntryResult.Migrated.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.migrated",
				"Legacy .cmd launchers moved into .we/ (backed up as legacy-*.bak): {files}",
				{ { "files", join(m_BuildEntryResult.Migrated) } }),
				m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Refreshed.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.refreshed",
				"Launchers/entry files written or refreshed: {files}",
				{ { "files", join(m_BuildEntryResult.Refreshed) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Removed.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.removed",
				"Cleaned up (generated files no longer needed): {files}",
				{ { "files", join(m_BuildEntryResult.Removed) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		if (!m_BuildEntryResult.Skipped.empty())
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.skipped",
				"Already up to date (not rewritten): {files}",
				{ { "files", join(m_BuildEntryResult.Skipped) } }), m_Theme.TextMuted, 13.0f);
			cursorY += 20.0f;
		}
		for (const std::string& warning : m_BuildEntryResult.Warnings)
		{
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.warning",
				"Warning: {text}", { { "text", warning } }), m_Theme.Accent, 12.0f);
			cursorY += 18.0f;
		}

		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.layout",
			"Layout: project root = 2 exe launchers + build.cmd + CMakePresets.json; .vs/ = Visual "
			"Studio launch item + current-item setting (launch.vs.json → ProjectRun.exe, "
			"ProjectSettings.json); .we/ = engine root (+ .cmd fallback)."),
			m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.next_steps", "Next steps"),
			m_Theme.Text, 13.0f);
		cursorY += 20.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.build_entry.step_build",
			"1) In the project root, run: build.cmd"), m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.build_entry.step_launch",
			"2) Double-click {name}-Play.exe to play (or Run ▶ Launch Project); in Visual Studio pick "
			"ProjectRun.exe (it builds Game, then opens the editor; add `--play` to run the game).",
			{ { "name", projectName } }), m_Theme.TextMuted, 12.0f);

		const Wui::ModalButtonDesc buttons[3] = {
			{ Wui::Tr("modal.build_entry.copy", "Copy instructions"),
				Wui::HashId("modal.build_entry.copy"), true },
			{ Wui::Tr("modal.build_entry.explorer", "Open project folder"),
				Wui::HashId("modal.build_entry.explorer"), true },
			{ Wui::Tr("modal.build_entry.close", "Close"),
				Wui::HashId("modal.build_entry.close"), true },
		};
		const int clicked = Wui::ModalButtons(ctx, frame, buttons, 3, m_Theme);
		if (clicked == 0)
		{
			if (ctx.SetClipboard)
				ctx.SetClipboard(guidance);
			PushNotice(Wui::Tr("notice.build_entry.copied", "Build instructions copied to the clipboard"));
		}
		else if (clicked == 1)
		{
			const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
				m_BuildEntryResult.ProjectRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
			{
				PushNotice(Wui::TrFormat("notice.build_entry.explorer_failed",
					"Could not open the project folder: {path}", { { "path", projectRoot } }));
				WLD_CORE_WARN("[build-entry] ShellExecuteW failed ({0}) for '{1}'",
					static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)), projectRoot);
			}
		}
		else if (clicked == 2 || escapePressed)
		{
			m_BuildEntryOpen = false;
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}


void EditorShell::DrawModals(Wui::WuiContext& ctx){
		// ---- P4-U13e:prefab 窗口的未保存改动(关窗 / 进文档会话)----
		DrawPrefabUnsavedModal(ctx);

		// ---- D10-10:导入位置(窗口级模态) ----
		// 放在这里(其余模态之前)是故意的:导入失败时 EditorLayer 会弹它自己的 Error 模态,
		// 那份错误框必须画在选择器**之上**才看得见(与 D10-9 面板内选择器时期的行为一致);
		// 选择器保持打开并把失败原因写进 import.dest.status。
		RenderImportDestinationModal(ctx);

		// ---- CPPT-6-ED-NEWSCRIPT:新建 C++ 组件(窗口级模态) ----
		DrawNewCppScriptModal(ctx);

		// ---- PECS-T9:新建 Lua 系统(窗口级模态;与上面同源但是独立模态) ----
		DrawNewLuaSystemModal(ctx);

		// ---- PROJ-1/T1:新建项目(窗口级模态;成功态换成两个动作按钮) ----
		DrawNewProjectModal(ctx);

		// ---- PLUG-AUTH-1:新建插件(窗口级模态;成功态换成动作按钮) ----
		DrawNewPluginModal(ctx);

		// ---- PROJ-11/T1:生成项目构建入口的结果(动作已在菜单回调里执行完) ----
		DrawBuildEntryModal(ctx);

		// ---- PROJ-2/T1:项目启动器(启动态窗口级模态;其它模态在前时让位) ----
		DrawProjectLauncherModal(ctx);

		const Wui::WuiId unsaved = Wui::HashId("modal.unsaved");
		if (m_Editor.ShowUnsavedModal()) ctx.SetModal(unsaved);
		else if (ctx.Modal() == unsaved) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = unsaved;
			frameDesc.Title = "Unsaved Changes";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, "The current scene has unsaved changes.", m_Theme.Text, 14.0f);
				// 三按钮:Save / Don't Save / Cancel(Esc = Cancel),文案与顺序逐字保留。
				const Wui::ModalButtonDesc buttons[3] = {
					{ "Save", Wui::HashId("modal.unsaved.save"), true },
					{ "Don't Save", Wui::HashId("modal.unsaved.nosave"), true },
					{ "Cancel", Wui::HashId("modal.unsaved.cancel"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 3, m_Theme);
				if (clicked == 0)
				{
					m_Editor.ResolveUnsavedModal(true);
					ctx.ClearModal();
				}
				else if (clicked == 1)
				{
					m_Editor.ResolveUnsavedModal(false);
					ctx.ClearModal();
				}
				else if (clicked == 2 || escapePressed)
				{
					m_Editor.CancelUnsavedModal();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId error = Wui::HashId("modal.error");
		if (m_Editor.ShowErrorModal()) ctx.SetModal(error);
		else if (ctx.Modal() == error) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = error;
			frameDesc.Title = "Error";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, m_Editor.ErrorText(), m_Theme.Text, 14.0f);
				const Wui::ModalButtonDesc buttons[1] = {
					{ "OK", Wui::HashId("modal.error.ok"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
				if (clicked == 0 || escapePressed)   // Esc = OK
				{
					m_Editor.ShowErrorModal() = false;
					m_Editor.ErrorText().clear();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId cooking = Wui::HashId("modal.cooking");
		if (m_Editor.ShowCookingProgress()) ctx.SetModal(cooking);
		else if (ctx.Modal() == cooking) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = cooking;
			frameDesc.Title = "Packaging";
			frameDesc.Size = { 420.0f, 140.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				if (m_Editor.CookingFinished())
				{
					const std::string message = m_Editor.CookingSucceeded() ? "Packaging finished." : "Packaging failed: " + m_Editor.CookingError();
					Label(ctx, { panel.X + 16, panel.Y + 48 }, message, m_Theme.Text, 14.0f);
					const Wui::ModalButtonDesc buttons[1] = {
						{ "OK", Wui::HashId("modal.cooking.ok"), true },
					};
					const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
					if (clicked == 0 || escapePressed)   // 完成态才关(Esc = OK)
					{
						m_Editor.ShowCookingProgress() = false;
						ctx.ClearModal();
					}
				}
				else
				{
					Label(ctx, { panel.X + 16, panel.Y + 52 }, "Packaging...", m_Theme.Text, 14.0f);
					// 进度中不允许 Esc:不消费 escapePressed。
				}
				Wui::EndModalFrame(ctx);
			}
		}

		// P4-UX11:旧的"项目设置"模态框已删除 —— 项目设置统一进独立窗口的 Settings 面板
		// (File ▸ Project Settings 改为打开该面板),避免两套界面互相打架。
	}

namespace Editor
{
AssetDropBridge& AssetDropBridge::Get(){
			static AssetDropBridge bridge;
			return bridge;
		}


void AssetDropBridge::BeginFrame(uint64_t frame){
			m_Frame = frame;
			// 只老化、不清空:目标窗口(独立 OS 窗口)在本帧的渲染发生在源面板之后,
			// 释放那一帧必须还能看到上一帧登记的落点矩形(见 EditorPanel.h 的契约注释)。
			m_Targets.erase(std::remove_if(m_Targets.begin(), m_Targets.end(),
				[frame](const Target& target)
				{
					return target.Frame + kTargetLifetimeFrames < frame;
				}), m_Targets.end());
			// 投递过的 drop 只给目标几帧时间取走:面板被关掉/不可见时不无限堆积。
			const uint64_t oldest = frame > 4 ? frame - 4 : 0;
			m_Pending.erase(std::remove_if(m_Pending.begin(), m_Pending.end(),
				[oldest](const Drop& drop) { return drop.Frame < oldest; }), m_Pending.end());
		}


void AssetDropBridge::Register(const Target& target){
			if (target.Owner.empty() || target.W <= 0.0f || target.H <= 0.0f)
				return;
			Target entry = target;
			entry.Frame = m_Frame;
			for (Target& existing : m_Targets)
			{
				if (existing.Owner == target.Owner && existing.Sink == target.Sink
					&& existing.Key == target.Key)
				{
					existing = entry;   // 每帧刷新:矩形会随布局/窗口移动变化
					return;
				}
			}
			m_Targets.push_back(std::move(entry));
		}


bool AssetDropBridge::DeliverFromScreen(float screenX, float screenY, const std::string& payload){
			for (const Target& target : m_Targets)
			{
				if (!target.PayloadPrefix.empty() && payload.rfind(target.PayloadPrefix, 0) != 0)
					continue;
				// 过期登记不参与命中(面板已隐藏/关闭后不能还吃 drop)。
				if (target.Frame + kTargetLifetimeFrames < m_Frame)
					continue;
				if (screenX < target.X || screenY < target.Y
					|| screenX > target.X + target.W || screenY > target.Y + target.H)
					continue;
				Drop drop;
				drop.Owner = target.Owner;
				drop.Sink = target.Sink;
				drop.Key = target.Key;
				drop.Payload = payload;
				drop.Frame = m_Frame;
				WLD_CORE_INFO("[wui-drop] bridge '{0}' -> panel '{1}' ({2}:{3})", payload, target.Owner,
					target.Sink, target.Key);
				m_Pending.push_back(std::move(drop));
				return true;
			}
			return false;
		}


bool AssetDropBridge::TakeDrop(const std::string& owner, const std::string& sink, const std::string& key, Drop* out){
			for (size_t index = 0; index < m_Pending.size(); ++index)
			{
				const Drop& drop = m_Pending[index];
				if (drop.Owner != owner || drop.Sink != sink)
					continue;
				if (!key.empty() && drop.Key != key)
					continue;
				if (out)
					*out = drop;
				m_Pending.erase(m_Pending.begin() + static_cast<std::ptrdiff_t>(index));
				return true;
			}
			return false;
		}

}
}
