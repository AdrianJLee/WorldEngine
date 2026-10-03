#include "EditorShell_Internal.h"

namespace World
{

using namespace EditorShellDetail;


	// ---- AI 控制通道 ----

bool EditorShell::AiTogglePanel(const std::string& panel){
		if (panel.empty() || !IsDeclaredPanel(panel))
			return false;
		// 材质面板是动态实例:先按 id 建出面板对象,再走菜单同一条开关路径。
		if (panel.rfind("material:", 0) == 0)
		{
			EnsureMaterialPanelFromId(panel);
			// 与 OpenMaterialEditor 同一条默认尺寸:材质面板的窄布局会把贴图下拉挤到窗口外
			// (实测 480x340 时 Albedo 组合框在 y=782 → 用户根本看不到)。
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		if (panel.rfind(kModelPanelPrefix, 0) == 0)
		{
			EnsureModelPanelFromId(panel);
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// P4-U13c:动态 prefab 资产窗口 —— 未打开 → 走 OpenPrefabWindow(默认附加到主窗口);
		// 已打开(附加标签或可见独立窗口)→ 走菜单同一条开关路径关闭。
		if (panel.rfind(kPrefabPanelPrefix, 0) == 0)
		{
			EnsurePrefabPanelFromId(panel);
			if (!m_Ctx)
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenPrefabWindow(panel.substr(std::strlen(kPrefabPanelPrefix)));
			return true;
		}
		// W9-2:动态脚本面板 —— 未打开 → 走 OpenScriptEditor(默认附加到主窗口);
		// 已打开(附加标签或可见独立窗口)→ 走菜单同一条开关路径关闭。
		if (panel.rfind(kScriptPanelPrefix, 0) == 0)
		{
			EnsureScriptPanelFromId(panel);
			if (!m_Ctx)
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenScriptEditorNow(panel.substr(std::strlen(kScriptPanelPrefix)));
			return true;
		}
		// PLUG-T3b:插件面板 —— 未建实例先补建,未打开走默认"附加到主窗口",已打开走切换。
		if (IsPluginPanelId(panel))
		{
			EnsurePluginPanelFromId(panel);
			if (!m_Ctx || !IsDeclaredPanel(panel))
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenPanelAttached(panel);
			return true;
		}
		if (!m_Ctx)
			return false;
		TogglePanel(*m_Ctx, panel);
		return true;
	}


	// P3-1②:脚本化"分离 / 挂回"。与顶部挂靠栏拖拽、窗口菜单走同一对既有路径:
	//   分离 = OpenIndependentPanel(复用已隐藏的窗口 / 必要时新建),附加态先把顶栏标签摘掉;
	//   挂回 = AttachIndependentWindowToSlot(OS 窗口隐藏 + 顶栏出现切换标签)。
	// 已处于目标状态时幂等,可读结果写进 message(供脚本直接断言,不必解析布局 JSON)。
bool EditorShell::AiDetachPanel(const std::string& panel, std::string* message){
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		// 形态规则(T03):只有声明为独立窗口的面板才有"附加态/浮动态";停靠形态面板的
		// 拖出是主窗口内的临时浮动,不在这两个命令的语义里(用 ui.open/拖拽改它的位置)。
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		const char* const state = PanelStateLabel(panel);
		if (std::strcmp(state, "floating") == 0)
		{
			// 幂等:已经是目标状态(真 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already floating: " + panel + " (independent OS window)";
			return true;
		}
		const bool wasAttached = std::strcmp(state, "attached") == 0;
		const std::string before = m_Layout.Serialize();
		OpenIndependentPanel(panel); // 复用已隐藏的窗口;从未打开过则新建(位置走 FloatRectFor 记忆)
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
		{
			// 窗口没建起来:附加态/标签保持原样,命令可重试(不制造"既没标签也没窗口"的半状态)。
			return fail("cannot show independent window for '" + panel + "'");
		}
		if (wasAttached)
		{
			// 摘掉顶栏标签(与顶栏 ✕ 之后的清理同一份状态):窗口本体保持不变。
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel),
				m_AttachedPanels.end());
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
		}
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "detach", panel, before);
		// 与顶栏拖出同一条操作记录(category/action/target 口径一致)。
		if (m_Ctx)
			m_Ctx->RecordOp("float", "detach", panel, wasAttached ? "attached" : "opened");
		SaveLayout();   // 形态变了(挂靠 → 浮窗),立刻落盘(P4-UX10)
		const Wui::WuiRect rect = host->ScreenRect();
		if (message)
		{
			std::ostringstream text;
			text << (wasAttached ? "detached " : "opened floating ") << panel
				<< " (independent window at " << static_cast<int>(rect.X) << "," << static_cast<int>(rect.Y)
				<< " " << static_cast<int>(rect.W) << "x" << static_cast<int>(rect.H) << ")";
			*message = text.str();
		}
		return true;
	}


	// P4-UX15:把停靠面板切到前台。以前只有 ui.focus(独立窗口),后台标签不渲染 →
	// 树里没有它的节点,内容浏览器这类停靠面板无法被脚本驱动。
bool EditorShell::AiActivatePanel(const std::string& panel, std::string* message){
		auto fail = [message](const std::string& text)
		{
			if (message) *message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		if (IsIndependentPanel(panel))
		{
			if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
			{
				m_ActiveWindowTag = panel;
				if (message) *message = "activated " + panel + " (top-bar tab)";
				return true;
			}
			if (FloatWindowHost* host = FindFloatHost(panel))
			{
				host->ActivatePanel(panel);
				host->Focus();
				if (message) *message = "activated " + panel + " (independent window)";
				return true;
			}
			OpenPanelAttached(panel);
			if (message) *message = "opened and activated " + panel;
			return true;
		}
		// 停靠面板:把它所在标签组的 Active 指到它(树里没有 = 先按 Window 菜单同一条路打开)。
		std::function<bool(Wui::DockNode&)> activateInTree = [&](Wui::DockNode& node) -> bool
		{
			if (node.IsTabs())
			{
				for (size_t i = 0; i < node.Panels.size(); ++i)
					if (node.Panels[i] == panel)
					{
						node.Active = i;
						return true;
					}
				return false;
			}
			for (Wui::DockNode& child : node.Children)
				if (activateInTree(child))
					return true;
			return false;
		};
		if (activateInTree(m_Layout.Root))
		{
			// P4-UX16:主窗口正被"已附加的独立窗口"占用时(顶栏标签模式,m_ActiveWindowTag 非空),
			// 停靠区根本不渲染 —— 此时只回 "activated (dock tab)" 就是假话:实测在内容浏览器里
			// 新建材质(顺带打开材质编辑器,它默认附加到主窗口)之后,ui.activate content_browser
			// 返回成功,但面板一个无障碍节点都不回来,后续 ui.invoke 全部落空。
			// 所以切停靠面板时顺手退回停靠视图(用户点顶栏标签的等效动作)。
			const bool leftAttachedView = !m_ActiveWindowTag.empty();
			m_ActiveWindowTag.clear();
			SaveLayout();
			if (message)
				*message = "activated " + panel + " (dock tab)"
					+ (leftAttachedView ? "; left the attached-window view" : "");
			return true;
		}
		if (m_Ctx)
			TogglePanel(*m_Ctx, panel);
		if (message) *message = "opened " + panel;
		return true;
	}


bool EditorShell::AiAttachPanel(const std::string& panel, std::string* message){
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		if (std::strcmp(PanelStateLabel(panel), "attached") == 0)
		{
			// 幂等:已经是目标状态(顶栏标签 + 隐藏的 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already attached: " + panel + " (top-bar tag active, OS window hidden)";
			return true;
		}
		const std::string before = m_Layout.Serialize();
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel); // 未打开过:先按独立窗口建出,再走同一条挂靠路径
		AttachIndependentWindowToSlot(panel);
		if (!FindFloatHost(panel) || std::strcmp(PanelStateLabel(panel), "attached") != 0)
			return fail("cannot attach panel '" + panel + "'");
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "attach", panel, before);
		if (message)
			*message = std::string("attached ") + panel + " (independent window hidden; top-bar tag = "
				+ PanelTitle(panel) + ")";
		return true;
	}


bool EditorShell::AiRequestFloatCapture(const std::string& panel, const std::string& path){
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden() || path.empty())
			return false;
		// 只登记整窗抓图请求:该窗口自己的帧循环会在"UI 提交之后、呈现之前"执行它。
		Renderer::RequestPresentCapture(host->GetPresentTarget(), path,
			static_cast<uint32_t>(host->ScreenRect().W), static_cast<uint32_t>(host->ScreenRect().H));
		return true;
	}


bool EditorShell::AiRequestPreviewCapture(const std::string& panel, const std::string& path){
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end() || path.empty())
			return false;
		if (auto* material = dynamic_cast<MaterialEditorPanel*>(found->second.get()))
		{
			material->RequestPreviewCapture(path);
			return true;
		}
		return false;
	}


bool EditorShell::AiResizeWindow(const std::string& panel, float width, float height){
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
			return false;
		const uint32_t targetWidth = static_cast<uint32_t>(std::max(240.0f, width));
		const uint32_t targetHeight = static_cast<uint32_t>(std::max(160.0f, height));
		host->SetClientSize(targetWidth, targetHeight);
		m_LastFloatRects[panel] = Wui::WuiRect { host->ScreenRect().X, host->ScreenRect().Y,
			static_cast<float>(targetWidth), static_cast<float>(targetHeight) };
		return true;
	}


std::string EditorShell::AiDescribeState() const{
		auto escape = [](const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		};
		std::ostringstream out;
		out << "{";
		out << "\"docked\":[";
		bool first = true;
		std::vector<Wui::PanelId> docked;
		m_Layout.AllPanels(&docked);
		for (const Wui::PanelId& id : docked)
		{
			if (!first)
				out << ",";
			first = false;
			out << "\"" << escape(id) << "\"";
		}
		out << "],\"floating\":[";
		first = true;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.Panel) << "\",\"rect\":[" << entry.Rect.X << "," << entry.Rect.Y
				<< "," << entry.Rect.W << "," << entry.Rect.H << "]}";
		}
		out << "],\"independentWindows\":[";
		first = true;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden())
				continue;
			if (!first)
				out << ",";
			first = false;
			const Wui::WuiRect rect = host->ScreenRect();
			out << "{\"panel\":\"" << escape(host->Panel()) << "\",\"rect\":[" << rect.X << "," << rect.Y
				<< "," << rect.W << "," << rect.H << "],\"tabCount\":" << host->Panels().size() << "}";
		}
		out << "],\"materials\":[";
		first = true;
		for (const auto& entry : m_PanelRegistry)
		{
			const auto* material = dynamic_cast<const MaterialEditorPanel*>(entry.second.get());
			if (!material)
				continue;
			const Ref<Material>& asset = material->GetMaterial();
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.first) << "\",\"path\":\"" << escape(material->GetMaterialPath()) << "\"";
			if (asset)
			{
				const MaterialDesc& desc = asset->GetDesc();
				out << ",\"albedo\":\"" << escape(desc.AlbedoTexture) << "\""
					<< ",\"normal\":\"" << escape(desc.NormalTexture) << "\""
					<< ",\"revision\":" << asset->GetRevision()
					<< ",\"baseColor\":[" << desc.BaseColor.r << "," << desc.BaseColor.g << ","
					<< desc.BaseColor.b << "," << desc.BaseColor.a << "]"
					<< ",\"blendMode\":" << static_cast<int>(desc.BlendMode);
			}
			out << "}";
		}
		// P3-1②:"附加/浮动态"是第一手断言目标(ui.open → ui.detach → ui.attach 的验收),
		// 这里按面板 id 列出形态(attached/floating/hidden),与 AttachTag 无障碍节点的
		// value 走同一个 PanelStateLabel —— 脚本不必去解析 floating/independentWindows 记录。
		std::vector<std::string> attachPanels = m_Panels;
		for (const auto& entry : m_PanelRegistry)
			if (std::find(attachPanels.begin(), attachPanels.end(), entry.first) == attachPanels.end())
				attachPanels.push_back(entry.first);
		for (const std::string& id : m_AttachedPanels)
			if (std::find(attachPanels.begin(), attachPanels.end(), id) == attachPanels.end())
				attachPanels.push_back(id);
		out << "],\"attach\":[";
		first = true;
		for (const std::string& id : attachPanels)
		{
			// 只有独立窗口形态的面板才有 attached/floating 两态;停靠面板的浮动是主窗口内
			// 临时浮动,不属于本清单(形态规则见 T03 / PanelStateLabel)。
			if (!IsDeclaredPanel(id) || !IsIndependentPanel(id))
				continue;
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(id) << "\",\"state\":\"" << PanelStateLabel(id) << "\"}";
		}
		out << "]}";
		return out.str();
	}


	// U26:窗口几何(客户区屏幕原点 + 尺寸,均为物理像素),键 = 无障碍节点的 window。
	// 主窗口 = GLFW 内容区原点(与注入窗口一致);独立窗口 = FloatWindowHost::ScreenRect
	// (同一份数据也是跨窗口拖放命中用的原点)。隐藏态(附加到主窗口)照常列出,
	// 但 visible=false —— 脚本由此能判断"面板在主窗口里"(那时节点 window 键是 main)。
std::string EditorShell::AiWindowRectsJson() const{
		auto escape = [](const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		};
		std::ostringstream out;
		out << "[";
		bool first = true;
		if (Application::HasInstance())
		{
			int x = 0, y = 0;
			Application::Get().GetWindow().GetPosition(&x, &y);
			out << "{\"window\":\"main\",\"x\":" << x << ",\"y\":" << y
				<< ",\"w\":" << Application::Get().GetWindow().GetWidth()
				<< ",\"h\":" << Application::Get().GetWindow().GetHeight()
				<< ",\"visible\":true,\"state\":\"main\",\"tabs\":1}";
			first = false;
		}
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (!first)
				out << ",";
			first = false;
			const Wui::WuiRect rect = host->ScreenRect();
			out << "{\"window\":\"float:" << escape(host->Panel())
				<< "\",\"x\":" << rect.X << ",\"y\":" << rect.Y
				<< ",\"w\":" << rect.W << ",\"h\":" << rect.H
				<< ",\"visible\":" << (host->IsHidden() ? "false" : "true")
				<< ",\"state\":\"" << PanelStateLabel(host->Panel())
				<< "\",\"tabs\":" << host->Panels().size() << "}";
		}
		out << "]";
		return out.str();
	}


std::string EditorShell::IndependentWindowPanel(size_t index) const{
		// 返回该窗口的代表面板(活动标签);调用方以它定位窗口(焦点/收回/挂靠)。
		if (index >= m_FloatHosts.size() || m_FloatHosts[index]->IsHidden())
			return std::string();
		return m_FloatHosts[index]->Panel();
	}


size_t EditorShell::IndependentWindowCount() const{
		size_t count = 0;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (!host->IsHidden())
				++count;
		return count;
	}


std::string EditorShell::IndependentWindowLabel(size_t index) const{
		if (index >= m_FloatHosts.size())
			return std::string();
		std::string label;
		for (size_t i = 0; i < m_FloatHosts[index]->Panels().size(); ++i)
		{
			if (i != 0)
				label += " | ";
			label += PanelTitle(m_FloatHosts[index]->Panels()[i]);
		}
		return label;
	}


FloatWindowHost* EditorShell::FindFloatHost(const std::string& panel){
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host->Contains(panel))
				return host.get();
		return nullptr;
	}


void EditorShell::EraseFloatHost(FloatWindowHost* host){
		if (!host)
			return;
		// 每窗口屏幕位置缓存以标签为键:宿主销毁时一并清理,避免重建后误判"停稳"。
		for (const std::string& panel : host->Panels())
			m_LastFloatScreenRects.erase(panel);
		m_FloatHosts.erase(std::remove_if(m_FloatHosts.begin(), m_FloatHosts.end(),
			[&](const std::unique_ptr<FloatWindowHost>& candidate) { return candidate.get() == host; }),
			m_FloatHosts.end());
	}


void EditorShell::FocusIndependentWindow(const std::string& panel){
		if (FloatWindowHost* host = FindFloatHost(panel))
			host->Focus();
	}


void EditorShell::OpenMaterialEditor(const std::string& path){
		// 每个材质一个面板 + 独立窗口:同一材质重复打开只是前置焦点(用户 2026-09-16)。
		const std::string panelId = std::string(kMaterialPanelPrefix) + MaterialLibrary::NormalizePath(path);
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<MaterialEditorPanel>(path);
			panel->OpenMaterial(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}


void EditorShell::EnsureMaterialPanelFromId(const std::string& panelId){
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kMaterialPanelPrefix));
		if (path == "(unsaved)")
			return;   // 未落盘的临时面板无法跨会话恢复
		auto panel = std::make_unique<MaterialEditorPanel>(path);
		panel->OpenMaterial(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}


	// ---- P1b D5:模型预览面板(动态实例,id = "model:<逻辑路径>")----

void EditorShell::OpenModelPreview(const std::string& logicalPath){
		// 每个模型一个面板 + 独立窗口:重复双击只是前置焦点(与材质编辑器同款;
		// 用户反馈"多次打开会多次叠加"—— 现在打开预览不改场景,放进场景在预览里显式点)。
		std::string path = logicalPath;
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
			return;
		const std::string panelId = std::string(kModelPanelPrefix) + path;
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<ModelPreviewPanel>(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}


	// ---- P4-U13c:prefab 资产窗口(动态实例,id = "prefab:<逻辑路径>")----

void EditorShell::OpenPrefabWindow(const std::string& logicalPath){
		OpenPrefabWindowChecked(logicalPath, nullptr);
	}


bool EditorShell::OpenPrefabWindowChecked(const std::string& logicalPath, std::string* message){
		// 与 `.wmodel` 预览同款:每个 prefab 一个面板 + 默认附加到主窗口;
		// 重复打开 = 立刻重读一次 + 前置焦点(资产的"刷新"入口)。
		std::string path = logicalPath;
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
		{
			if (message)
				*message = "empty prefab path";
			return false;
		}
		const std::string panelId = std::string(kPrefabPanelPrefix) + path;
		PrefabPanel* panel = nullptr;
		if (const auto found = m_PanelRegistry.find(panelId); found != m_PanelRegistry.end())
		{
			panel = dynamic_cast<PrefabPanel*>(found->second.get());
		}
		else
		{
			auto created = std::make_unique<PrefabPanel>(path);
			panel = created.get();
			m_PanelRegistry.emplace(panelId, std::move(created));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 560.0f } });
		}
		if (!panel)
		{
			if (message)
				*message = "panel id '" + panelId + "' is not a prefab window";
			return false;
		}
		// 打开/前置之前先把这份资产读一遍:失败也要开窗口 —— 状态行写可读原因,
		// 调用方(AI 通道)拿到 false + 同一条原因,不存在"静默成功"。
		const bool readable = panel->ReloadNow(m_Editor.GetActiveScene().get(), message);
		OpenPanelAttached(panelId);
		return readable;
	}


	// P4-U13e:脚本化写字段 —— 复用面板自己的写入口(不是旁路:同样走脏标记/状态行/资产警告)。
bool EditorShell::SetPrefabPanelField(const std::string& panelId, const std::string& component, const std::string& field, const std::string& value, const std::string& axis, std::string* message){
		PrefabPanel* prefab = PrefabPanelById(panelId);
		if (!prefab)
		{
			if (message)
				*message = "no prefab window for panel '" + panelId + "'";
			return false;
		}
		return prefab->SetEditableField(component, field, value, axis, message);
	}


void EditorShell::RequestImportDestination(const std::string& sourcePath){
		// D10-10(用户 2026-09-19):导入位置选择器改成**窗口级模态**(此前是内容浏览器面板
		// 内的一层覆盖:位置偏、挡不住后面的输入)。范围仍限定内容根内 —— 原生文件夹对话框
		// 能选到工作区外,那种位置场景/打包都引用不到。状态与目录树由 shell 持有。
		if (sourcePath.empty())
			return;
		m_ImportSourcePath = std::filesystem::path(sourcePath);
		m_ImportStatus.clear();
		// 内容根 = 运行期当前项目根(World::Paths):换项目(--project / WLD_PROJECT_DIR)后
		// 导入位置选择器跟着切,不再固定成编译期默认项目。
		m_ImportTreeRoot = World::Paths::AssetRoot();
		m_ImportDestDir = m_ImportTreeRoot;
		const std::string panel = "content_browser";
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);   // 内容浏览器被关掉时先让它回到停靠树(导入完能直接看到新文件)
		const auto found = m_PanelRegistry.find(panel);
		if (found != m_PanelRegistry.end())
			if (auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get()))
				m_ImportDestDir = browser->CurrentDirectory();   // 默认落点 = 当前文件夹
		// 打开时扫一次内容根 + 重置模态自己的树状态(根行默认展开)。
		ScanImportTree();
		m_ImportTreeOpen.clear();
		// 默认落点(内容浏览器当前文件夹)可能在深层:把它的祖先链一起展开,
		// 打开时就能看到"选中"的那一行(只展开,不改选中)。
		for (std::filesystem::path dir = m_ImportDestDir; dir != m_ImportTreeRoot && dir.has_relative_path();)
		{
			m_ImportTreeOpen.insert(dir);
			const std::filesystem::path parent = dir.parent_path();
			if (parent == dir)
				break;   // 防御:到达盘符根仍不等于内容根时停止
			dir = parent;
		}
		m_ImportTreeOpen.insert(m_ImportTreeRoot);
		m_ImportTreeScroll = 0.0f;
		m_ImportModalOpen = true;
		if (m_Ctx)
		{
			// 打开前清掉悬着的弹窗;清文本焦点,否则后面面板里已聚焦的输入框还会继续吃键盘输入。
			m_Ctx->CloseAllPopups();
			m_Ctx->SetFocus(0);
			m_Ctx->RecordOp("import", "dest-open", m_ImportSourcePath.filename().string(), "");
		}
		WLD_CORE_INFO("[import] 选择导入位置(窗口级模态): {0}", sourcePath);
	}


void EditorShell::Notify(const std::string& message){
		// P4-UX16:面板级短反馈统一进状态栏提示(4s 停留 / 悬停冻结 / 移开 2.6s 宽限后淡出)。
		// 走同一条 PushNotice = 同一条无障碍节点,脚本与读屏也能读到这句话。
		PushNotice(message);
	}


	// D10-10:扫内容根下的全部子目录(低频操作:只在打开导入模态时跑一次)。
	// 与内容浏览器左侧树同一套数据形态:按路径排序 + 根行在最前(Depth 0)。
void EditorShell::ScanImportTree(){
		m_ImportTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_ImportTreeRoot,
			std::filesystem::directory_options::skip_permission_denied, scanError);
		const std::filesystem::recursive_directory_iterator scanEnd;
		for (; scanIt != scanEnd; scanIt.increment(scanError))
		{
			if (scanError)
				break;   // 权限错误等:已扫到的部分照常可用(不抛异常、不中断整个选择器)
			const std::filesystem::directory_entry& entry = *scanIt;
			std::error_code entryError;
			if (!entry.is_directory(entryError))
				continue;
			ImportTreeRow row;
			row.Path = entry.path();
			row.Depth = scanIt.depth() + 1;
			std::error_code childError;
			for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(row.Path,
				std::filesystem::directory_options::skip_permission_denied, childError))
			{
				if (childError)
					break;
				std::error_code childDirError;
				if (child.is_directory(childDirError))
				{
					row.HasChildren = true;
					break;
				}
			}
			m_ImportTree.push_back(std::move(row));
		}
		std::sort(m_ImportTree.begin(), m_ImportTree.end(),
			[](const ImportTreeRow& a, const ImportTreeRow& b) { return a.Path < b.Path; });
		ImportTreeRow rootRow;
		rootRow.Path = m_ImportTreeRoot;
		rootRow.Depth = 0;
		rootRow.HasChildren = !m_ImportTree.empty();
		m_ImportTree.insert(m_ImportTree.begin(), std::move(rootRow));
	}


	// D10-11:窗口级"选择导入位置"模态 —— 居中/遮罩/标题栏/Esc/按钮条走 WuiModal 组件;
	// "挡住后面所有面板的命中"由 OnRender 的 BeginModalInputBlock/EndModalInputBlock 成对负责
	// (见那里的顺序说明)。状态行仍归 shell(它有稳定的无障碍 id import.dest.status)。
void EditorShell::RenderImportDestinationModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.importdest");
		if (m_ImportModalOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();

		const auto logicalText = [this](const std::filesystem::path& dir)
		{
			const std::string relative = dir.lexically_relative(m_ImportTreeRoot).generic_string();
			return (relative.empty() || relative == ".") ? std::string("(内容根)") : relative;
		};

		Wui::WuiRect panel;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = "选择导入位置";
		frameDesc.Size = { 560.0f, 440.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			return;

		const float pad = 16.0f;
		Wui::Label(ctx, { panel.X + pad, panel.Y + 40.0f },
			"源文件: " + m_ImportSourcePath.filename().string()
				+ " · 导入到: " + logicalText(m_ImportDestDir),
			m_Theme.TextMuted, 13.0f);

		const float statusH = 20.0f;
		// 按钮条固定贴 frame 底部(ModalFooter 画),状态行排在它上方。
		const float statusY = panel.Y + panel.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight - 6.0f - statusH;
		const Wui::WuiRect treeArea { panel.X + pad, panel.Y + 62.0f, panel.W - pad * 2.0f,
			std::max(40.0f, statusY - 8.0f - (panel.Y + 62.0f)) };
		Wui::PanelBackground(ctx, treeArea, { 0.09f, 0.095f, 0.10f, 1 });

		// 可见行:父行折叠 → 整棵子树不显示(m_ImportTree 是"父在子前"的预排序)。
		std::vector<const ImportTreeRow*> visibleRows;
		std::vector<Wui::TreeViewItem> treeItems;
		std::vector<Wui::WuiId> treeItemIds;
		std::vector<bool> openAtDepth;
		for (const ImportTreeRow& row : m_ImportTree)
		{
			if (row.Depth > 0)
			{
				if (row.Depth - 1 >= static_cast<int>(openAtDepth.size()))
					continue;   // 祖先行没显示 → 本行也不显示
				if (!openAtDepth[row.Depth - 1])
					continue;   // 直接父行折叠
			}
			if (static_cast<int>(openAtDepth.size()) > row.Depth)
				openAtDepth.resize(static_cast<size_t>(row.Depth));
			const bool expanded = m_ImportTreeOpen.find(row.Path) != m_ImportTreeOpen.end();
			openAtDepth.push_back(expanded);

			const std::filesystem::path rel = row.Path.lexically_relative(m_ImportTreeRoot);
			const std::string relText = (rel == ".") ? std::string() : rel.generic_string();
			Wui::TreeViewItem item;
			// 无障碍 id 约定(逐字,与 D10-9 一致):根行 = import.dest.tree.root,
			// 其它 = import.dest.tree.<相对路径>。
			item.Id = Wui::HashId(relText.empty() ? "import.dest.tree.root"
				: ("import.dest.tree." + relText).c_str());
			item.Label = row.Path.filename().string();   // 根行 = 内容根目录名
			item.Depth = row.Depth;
			item.HasChildren = row.HasChildren;
			item.Expanded = expanded;
			item.Selected = m_ImportDestDir == row.Path;
			visibleRows.push_back(&row);
			treeItemIds.push_back(item.Id);
			treeItems.push_back(std::move(item));
		}
		const Wui::TreeViewResult tree = Wui::TreeView(ctx, treeArea, treeItems, 20.0f, m_ImportTreeScroll, m_Theme);
		for (size_t i = 0; i < visibleRows.size(); ++i)
		{
			const ImportTreeRow& row = *visibleRows[i];
			if (tree.ClickedArrow == static_cast<int>(i))
			{
				if (treeItems[i].Expanded)
					m_ImportTreeOpen.erase(row.Path);
				else
					m_ImportTreeOpen.insert(row.Path);
			}
			else if (tree.Clicked == static_cast<int>(i))
			{
				m_ImportDestDir = row.Path;   // 单选:点行只改选中,不导航
			}
			// TreeView 自身不登记行节点(已知限制),按上面的 id 约定手动登记:
			// AI 可读可点;只登记落在树可视区内的行,避免点到看不见的行。
			if (i < tree.ItemRects.size() && treeItemIds[i] != 0)
			{
				const Wui::WuiRect& rowRect = tree.ItemRects[i];
				// 只登记"行中心确实落在树可视区内"的行:AI 注入的点击打在行中心,
				// 半滚出视口的行中心可能压到按钮行,点了会打错目标。
				const float rowCenterY = rowRect.Y + rowRect.H * 0.5f;
				if (rowRect.W > 0.0f && rowRect.H > 0.0f
					&& rowCenterY >= treeArea.Y && rowCenterY <= treeArea.Y + treeArea.H)
				{
					Wui::WuiAccessNode rowNode;
					rowNode.Id = treeItemIds[i];
					rowNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					rowNode.Panel = "shell";
					rowNode.Kind = "tree-item";
					rowNode.Label = treeItems[i].Label;
					rowNode.Value = logicalText(row.Path);
					rowNode.Rect = rowRect;
					rowNode.Interactive = true;
					Wui::WuiAccessibility::Get().Register(rowNode);
				}
			}
		}

		bool closeRequested = false;
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, panel, "导入到此文件夹", "取消",
			Wui::HashId("import.dest.ok"), Wui::HashId("import.dest.cancel"), true, m_Theme);
		if (footerResult == Wui::ModalResult::Confirm)
		{
			// 目的地 = 选中目录相对内容根的路径;**内容根本身传空串**(与内核/cook 约定一致)。
			const std::filesystem::path destRelative = m_ImportDestDir.lexically_relative(m_ImportTreeRoot);
			const std::string destRelativeText = destRelative.generic_string();
			const std::string destination =
				(destRelativeText.empty() || destRelativeText == ".") ? std::string() : destRelativeText;
			std::string message;
			std::string logicalModel;
			if (m_Editor.ImportModelFile(m_ImportSourcePath.string(), &message, &logicalModel, destination))
			{
				m_ImportStatus = message.empty() ? ("已导入到 " + logicalText(m_ImportDestDir)) : message;
				ctx.RecordOp("import", "dest-ok", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_INFO("[import] {0}", m_ImportStatus);
				// 内容浏览器刷新(它自己的公开入口)+ 模型预览。
				const auto browserFound = m_PanelRegistry.find("content_browser");
				if (browserFound != m_PanelRegistry.end())
					if (auto* browser = dynamic_cast<ContentBrowserPanel*>(browserFound->second.get()))
						browser->RefreshContents();
				if (!logicalModel.empty())
					OpenModelPreview(logicalModel);
				closeRequested = true;   // 成功后关闭;失败保持打开,用户可改选目录重试。
			}
			else
			{
				m_ImportStatus = message.empty() ? std::string("导入失败(宿主未给出原因)") : message;
				ctx.RecordOp("import", "dest-failed", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_WARN("[import] 导入 '{0}' 失败: {1}", m_ImportSourcePath.string(), m_ImportStatus);
			}
		}
		else if (footerResult == Wui::ModalResult::Cancel)
		{
			ctx.RecordOp("import", "dest-cancel", m_ImportSourcePath.filename().string(), "");
			closeRequested = true;
		}
		if (escapePressed)
			closeRequested = true;

		// 状态行:选中目录 + 上一次导入的可读结果(失败原因也写在这里)。
		const std::string statusText = "选中: " + logicalText(m_ImportDestDir)
			+ (m_ImportStatus.empty() ? std::string() : (" · " + m_ImportStatus));
		{
			Wui::WuiAccessNode statusNode;
			statusNode.Id = Wui::HashId("import.dest.status");
			statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			statusNode.Panel = "shell";
			statusNode.Kind = "status";
			statusNode.Label = "import destination status";
			statusNode.Value = statusText;
			statusNode.Rect = { panel.X + pad, statusY, panel.W - pad * 2.0f, statusH };
			statusNode.Interactive = false;
			Wui::WuiAccessibility::Get().Register(statusNode);
		}
		Wui::Label(ctx, { panel.X + pad, statusY }, statusText,
			m_ImportStatus.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);

		if (closeRequested)
		{
			m_ImportModalOpen = false;
			m_ImportStatus.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}

}
