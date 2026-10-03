#include "EditorShell_Internal.h"

namespace World
{

using namespace EditorShellDetail;


	// ---- PROJ-1/T1:File ▸ New Project…(任意位置新建标准项目)----
	//
	// 需求(用户 2026-09-28):在任意位置生成一个**标准、干净**的项目骨架(不含示例)。
	// 本模态只做三件事:收集 项目名 + 位置;实时回显落点与行内错误;把"浏览…/创建"
	// 标记成下一帧开头的任务(原生对话框与落盘都不在渲染中途做)。
	// 生成内核 = Editor::ProjectScaffolder(清单走引擎 writer、Main.wd 走引擎序列化、
	// 结构来自所选模板 templates/project-<id>/**);成功后模态换成两个动作:资源管理器打开 / 打开项目。
	// PROJ-2/T1 追加:可勾选"最小可运行场景"(默认勾选),成功态第三个动作 = 启动项目
	// (独立 Runtime 进程,不重启编辑器)。

std::string EditorShell::NewProjectTrimmedName() const{
		return TrimProjectNameText(m_NewProjectName);
	}


std::string EditorShell::NewProjectNameError() const{
		return Editor::ProjectScaffolder::ValidateProjectName(m_NewProjectName);
	}


std::string EditorShell::NewProjectLocationError() const{
		return Editor::ProjectScaffolder::ValidateLocation(
			std::filesystem::u8path(m_NewProjectLocation), m_NewProjectName);
	}


std::filesystem::path EditorShell::NewProjectTargetRoot() const{
		if (m_NewProjectLocation.empty())
			return {};
		const std::string name = NewProjectTrimmedName();
		if (name.empty())
			return {};
		// 输入框与原生对话框都是 UTF-8 文本 → 按 UTF-8 解释成路径(非 ASCII 位置不会乱码)。
		return (std::filesystem::u8path(m_NewProjectLocation) / std::filesystem::u8path(name)).lexically_normal();
	}


	// PROJ-7/T2:模板库刷新(模板 = 磁盘事实 templates/project-*/template.json)。打开模态时强制
	// 刷一次;模态开着时按 2s 节流 —— 目录扫描不进每帧路径。选中项按 id 保持;被删/首次打开时
	// 优先回退到 empty,再退第一个模板。
void EditorShell::RefreshNewProjectTemplates(bool force){
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (!force && m_NewProjectTemplatesScannedAt > 0.0 && now - m_NewProjectTemplatesScannedAt < 2.0)
			return;
		m_NewProjectTemplatesScannedAt = now;
		m_NewProjectTemplates = Editor::ProjectScaffolder::ListTemplates();
		const auto has = [this](const std::string& id)
		{
			return std::any_of(m_NewProjectTemplates.begin(), m_NewProjectTemplates.end(),
				[&id](const Editor::ProjectScaffolder::TemplateInfo& info) { return info.Id == id; });
		};
		if (!has(m_NewProjectTemplateId))
		{
			m_NewProjectTemplateId.clear();
			if (has("empty"))
				m_NewProjectTemplateId = "empty";
			else if (!m_NewProjectTemplates.empty())
				m_NewProjectTemplateId = m_NewProjectTemplates.front().Id;
			// 换了模板:上一次的落盘失败原因不再对应当前选择(与"名称/落点一改就作废"同口径)。
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
		}
	}


const Editor::ProjectScaffolder::TemplateInfo* EditorShell::SelectedNewProjectTemplate() const{
		for (const Editor::ProjectScaffolder::TemplateInfo& info : m_NewProjectTemplates)
			if (info.Id == m_NewProjectTemplateId)
				return &info;
		return nullptr;
	}


void EditorShell::OpenNewProjectModal(Wui::WuiContext& ctx, const std::string& preselectTemplateId){
		// PROJ-17(用户 2026-09-29:"位置那里,删除按一下就把路径全删了"):文本框的编辑状态
		// (光标/选区)按控件 id 长期持久化 —— 上一次会话里若整段被选中(拖选或 Ctrl+A),
		// 下次打开对话框时选区仍在,第一次 Delete/Backspace 就会把**整段路径**一次删掉。
		// 每次打开对话框都清掉这两个输入框的持久状态:新状态 = 光标在末尾、无选区。
		ctx.ErasePersist(Wui::HashId("project.new.name"));
		ctx.ErasePersist(Wui::HashId("project.new.location"));
		m_NewProjectOpen = true;
		m_NewProjectOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewProjectCreated = false;
		m_NewProjectCreatedStarterScene = true;
		m_NewProjectCreatedTemplateName.clear();
		m_NewProjectCreatedStartScene.clear();
		m_NewProjectEntryPoints.clear();
		m_NewProjectRoot.clear();
		m_NewProjectCreatedName.clear();
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		m_NewProjectBrowsePending = false;
		m_NewProjectCreatePending = false;
		// PROJ-7/T3b:调用方可以预选模板(启动器"新建示例项目…" ⇒ example)。必须写在强制刷新
		// **之前** —— RefreshNewProjectTemplates 只在"选中 id 不在模板库里"时才回退,所以预选值
		// 存在就保留;模板缺失时自动降级到 empty/第一个模板(不额外报错)。
		if (!preselectTemplateId.empty())
			m_NewProjectTemplateId = preselectTemplateId;
		RefreshNewProjectTemplates(true);   // 模态打开这一帧就把模板列出来(磁盘事实)
		if (m_NewProjectName.empty())
			m_NewProjectName = "MyProject";
		if (m_NewProjectLocation.empty())
			m_NewProjectLocation = Editor::ProjectScaffolder::DefaultProjectLocation().u8string();
		ctx.SetModal(Wui::HashId("modal.newproject"));
		ctx.SetFocus(Wui::HashId("project.new.name"));
		ctx.RecordOp("project", "new-ask", NewProjectTrimmedName(), m_NewProjectLocation);
	}


void EditorShell::RunNewProjectBrowse(Wui::WuiContext& ctx){
		// 原生"选择文件夹"对话框:只在帧边界打开(取消返回空串 → 保持原值)。
		const std::string folder = FileDialogs::SelectFolder("选择项目位置");
		if (folder.empty())
			return;
		m_NewProjectLocation = folder;
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		ctx.RecordOp("project", "browse", NewProjectTrimmedName(), folder);
	}


void EditorShell::RunNewProjectCreate(Wui::WuiContext& ctx){
		RefreshNewProjectTemplates(false);
		const Editor::ProjectScaffolder::TemplateInfo* selected = SelectedNewProjectTemplate();
		// 所选模板自带启动场景(声明 defaultScene)时不生成最小可运行场景:向导里那个勾选框
		// 此时是禁用态(见 DrawNewProjectModal),这里再夹一次,防止"绕过按钮的第二次调用"。
		const bool templateOwnsStartScene = selected != nullptr && !selected->DefaultScene.empty();
		const std::string nameError = NewProjectNameError();
		const std::string locationError = NewProjectLocationError();
		const std::string templateError = Editor::ProjectScaffolder::ValidateTemplate(m_NewProjectTemplateId);
		const std::string blocked = !nameError.empty() ? nameError
			: (!locationError.empty() ? locationError : templateError);
		if (!blocked.empty())
		{
			// 模态里已经画过行内错误;这条分支只是拒绝"绕过按钮的第二次调用"。
			m_NewProjectFailure = blocked;
			m_NewProjectFailureFor = NewProjectTargetRoot().u8string();
			return;
		}

		const std::string name = NewProjectTrimmedName();
		const Editor::ProjectScaffolder::Result result = Editor::ProjectScaffolder::Create(
			std::filesystem::u8path(m_NewProjectLocation), name, Application::Get().GetContext(),
			templateOwnsStartScene ? false : m_NewProjectStarterScene, m_NewProjectTemplateId);
		if (!result.Ok)
		{
			m_NewProjectFailure = result.Error;
			m_NewProjectFailureFor = NewProjectTargetRoot().u8string();
			WLD_CORE_WARN("[new-project] create failed: {0}", result.Error);
			return;
		}

		m_NewProjectCreated = true;
		m_NewProjectCreatedStarterScene = !templateOwnsStartScene && m_NewProjectStarterScene;
		m_NewProjectCreatedTemplateName = selected != nullptr ? selected->Name : m_NewProjectTemplateId;
		m_NewProjectCreatedStartScene = templateOwnsStartScene ? selected->DefaultScene : std::string();
		m_NewProjectEntryPoints = result.EntryPoints;
		m_NewProjectRoot = result.ProjectRoot;
		m_NewProjectCreatedName = name;
		m_NewProjectFailure.clear();
		m_NewProjectFailureFor.clear();
		// 操作日志与状态栏提示:与其它"新建资产"入口同一口径(project / new)。
		// 细节里带上模板 id:验证者/脚本从 ops.tail 就能确认"用了哪个模板"。
		ctx.RecordOp("project", "new", name,
			result.ProjectRoot.u8string() + " [template " + m_NewProjectTemplateId + "]");
		PushNotice(Wui::TrFormat("notice.newproject.created",
			"Created project {path} (template: {template})",
			{ { "path", result.ProjectRoot.u8string() }, { "template", m_NewProjectCreatedTemplateName } }));
		WLD_CORE_INFO("[new-project] created '{0}' ({1} files)", result.ProjectRoot.u8string(),
			result.Files.size());
	}


void EditorShell::DrawNewProjectModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.newproject");
		if (m_NewProjectOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewProjectOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newproject.title", "New Project");
		// PROJ-7/T2:模板选择 + 说明/提示各占一行 ⇒ 比 PROJ-2 的 320 高 100px。
		frameDesc.Size = { 620.0f, 420.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewProjectOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 16.0f;

			// ---- 成功态:已生成 + 三个动作按钮(资源管理器打开 / 打开项目 / 启动项目)----
		if (m_NewProjectCreated)
		{
			float cursorY = frame.Y + 48.0f;
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.newproject.created",
				"Created the project (template: {template}):",
				{ { "template", m_NewProjectCreatedTemplateName } }), m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			Wui::Label(ctx, { labelX, cursorY }, m_NewProjectRoot.u8string(), m_Theme.Text, 13.0f);
			cursorY += 24.0f;
			// 成功态也把"实际用的模板 / 清单 start_scene"登记进无障碍树:验证者按 id 读,
			// 不用从提示文案里猜(ops.tail 里的 project/new 也带模板 id)。
			{
				Wui::WuiAccessNode templateNode;
				templateNode.Id = Wui::HashId("project.new.created.template");
				templateNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				templateNode.Panel = "shell";
				templateNode.Kind = "text";
				templateNode.Label = Wui::Tr("modal.newproject.template", "Template");
				templateNode.Value = m_NewProjectCreatedTemplateName;
				templateNode.Rect = { labelX, frame.Y + 46.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				templateNode.Enabled = true;
				templateNode.Interactive = false;
				templateNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(templateNode);

				Wui::WuiAccessNode sceneNode;
				sceneNode.Id = Wui::HashId("project.new.created.scene");
				sceneNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				sceneNode.Panel = "shell";
				sceneNode.Kind = "text";
				sceneNode.Label = Wui::Tr("modal.newproject.created.scene_label", "Start scene");
				sceneNode.Value = m_NewProjectCreatedStartScene.empty()
					? std::string("scenes/Main.wd") : m_NewProjectCreatedStartScene;
				sceneNode.Rect = { labelX, frame.Y + 66.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				sceneNode.Enabled = true;
				sceneNode.Interactive = false;
				sceneNode.Visible = true;
				Wui::WuiAccessibility::Get().Register(sceneNode);
			}
			if (!m_NewProjectCreatedStartScene.empty())
			{
				// PROJ-7/T2:模板自带启动场景(example ⇒ scenes/3DTest.wd)—— 没有生成 Main.wd。
				Wui::Label(ctx, { labelX, cursorY },
					Wui::TrFormat("modal.newproject.created.template_scene",
						"Start scene: {scene} (ships with the template).",
						{ { "scene", m_NewProjectCreatedStartScene } }),
					m_Theme.TextMuted, 12.0f);
			}
			else if (m_NewProjectCreatedStarterScene)
			{
				Wui::Label(ctx, { labelX, cursorY },
					Wui::Tr("modal.newproject.created.starter",
						"assets/scenes/Main.wd has a minimal scene (camera + directional light) — "
						"Launch Project shows a picture right away."),
					m_Theme.TextMuted, 12.0f);
			}
			else
			{
				Wui::Label(ctx, { labelX, cursorY },
					Wui::Tr("modal.newproject.created.empty_scene",
						"The scene is empty — launching the project shows a blank picture. "
						"Tick the starter-scene option next time, or add content in the editor."),
					m_Theme.Danger, 12.0f);
			}
			cursorY += 22.0f;
			// PROJ-3/T1:启动器模式下没有"留在这里继续工作"这一选项 —— 默认动作是打开新项目。
			Wui::Label(ctx, { labelX, cursorY },
				m_LauncherMode
					? Wui::Tr("modal.newproject.created.hint.launcher",
						"In launcher mode: Open Project (the default action) restarts the editor into "
						"the new project; Launch Project runs it in a separate Runtime process.")
					: Wui::Tr("modal.newproject.created.hint",
						"Launch Project runs it in a separate Runtime process; Open Project restarts the "
						"editor into it; or keep working here."),
				m_Theme.TextMuted, 12.0f);
			cursorY += 22.0f;
			// PROJ-3/T1(P2b):项目根里的启动入口(exe 优先,缺失则 .cmd 兜底)。
			if (!m_NewProjectEntryPoints.empty())
			{
				std::string entryList;
				for (const std::string& entry : m_NewProjectEntryPoints)
				{
					if (!entryList.empty())
						entryList += ", ";
					entryList += entry;
				}
				Wui::Label(ctx, { labelX, cursorY },
					Wui::TrFormat("modal.newproject.created.entrypoints",
						"Project root: {files}; build with build.cmd, other entry files live in .we/ "
						"(engine root record + .cmd fallback), and Visual Studio lists one launch item "
						"(ProjectRun.exe — opens this project in the editor, from .vs/launch.vs.json).",
						{ { "files", entryList } }),
					m_Theme.TextMuted, 12.0f);
			}

			// PROJ-3/T1:启动器模式下"打开项目"是默认(最左)动作 —— 启动器进程要做的是把
			// 用户送进新项目;普通编辑器形态保持 PROJ-2 的按钮顺序。
			// None = 本帧没有任何按钮被按下(ModalButtons 返回 -1)—— 不能当成 Close,
			// 否则成功态会在画出来的同一帧把自己关掉。
			enum class CreateAction { None, Explorer, OpenProject, LaunchProject, Close };
			CreateAction actionOrder[4];
			Wui::ModalButtonDesc buttons[4];
			if (m_LauncherMode)
			{
				actionOrder[0] = CreateAction::OpenProject;
				buttons[0] = { Wui::Tr("modal.newproject.open_project", "Open Project"),
					Wui::HashId("project.new.open"), true };
				actionOrder[1] = CreateAction::LaunchProject;
				buttons[1] = { Wui::Tr("modal.newproject.launch", "Launch Project"),
					Wui::HashId("project.new.launch"), true };
				actionOrder[2] = CreateAction::Explorer;
				buttons[2] = { Wui::Tr("modal.newproject.open_explorer", "Open in Explorer"),
					Wui::HashId("project.new.explorer"), true };
				actionOrder[3] = CreateAction::Close;
				buttons[3] = { Wui::Tr("modal.newproject.close", "Close"),
					Wui::HashId("project.new.close"), true };
			}
			else
			{
				actionOrder[0] = CreateAction::Explorer;
				buttons[0] = { Wui::Tr("modal.newproject.open_explorer", "Open in Explorer"),
					Wui::HashId("project.new.explorer"), true };
				actionOrder[1] = CreateAction::OpenProject;
				buttons[1] = { Wui::Tr("modal.newproject.open_project", "Open Project"),
					Wui::HashId("project.new.open"), true };
				// PROJ-2/T1:第三个动作按钮 —— 独立 Runtime 进程直接跑刚建的项目(不重启编辑器)。
				actionOrder[2] = CreateAction::LaunchProject;
				buttons[2] = { Wui::Tr("modal.newproject.launch", "Launch Project"),
					Wui::HashId("project.new.launch"), true };
				actionOrder[3] = CreateAction::Close;
				buttons[3] = { Wui::Tr("modal.newproject.close", "Close"),
					Wui::HashId("project.new.close"), true };
			}
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 4, m_Theme);
			const CreateAction action = clicked >= 0 && clicked < 4 ? actionOrder[clicked] : CreateAction::None;
			if (action == CreateAction::Explorer)
			{
				// ShellExecuteW 打开项目目录(与"打开外部脚本"同一条系统关联路径)。
				const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
					m_NewProjectRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
				{
					PushNotice(Wui::TrFormat("notice.newproject.explorer_failed",
						"Could not open the project folder: {path}",
						{ { "path", m_NewProjectRoot.u8string() } }));
					WLD_CORE_WARN("[new-project] ShellExecuteW failed ({0}) for '{1}'",
						static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)),
						m_NewProjectRoot.u8string());
				}
				else
				{
					ctx.RecordOp("project", "open-explorer", m_NewProjectCreatedName,
						m_NewProjectRoot.u8string());
				}
			}
			else if (action == CreateAction::OpenProject)
			{
				ctx.RecordOp("project", "open", m_NewProjectCreatedName, m_NewProjectRoot.u8string());
				// 重启编辑器到新项目(有未保存改动时先走既有的未保存确认模态)。
				m_Editor.RelaunchWithProject(m_NewProjectRoot);
				m_NewProjectOpen = false;
				ctx.ClearModal();
			}
			else if (action == CreateAction::LaunchProject)
			{
				// 一键启动:独立 Runtime 进程(定位/命令行/日志/操作记录都在 EditorLayer)。
				m_Editor.LaunchProjectRuntime(m_NewProjectRoot);
			}
			else if (action == CreateAction::Close || escapePressed)
			{
				m_NewProjectOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		const Wui::WuiId nameId = Wui::HashId("project.new.name");
		const Wui::WuiId locationId = Wui::HashId("project.new.location");
		const Wui::WuiId browseId = Wui::HashId("project.new.browse");
		const Wui::WuiId okId = Wui::HashId("project.new.ok");
		const Wui::WuiId cancelId = Wui::HashId("project.new.cancel");
		const bool justOpened = ctx.Frame() == m_NewProjectOpenedFrame;
		// PROJ-7/T2:模板库按 2s 节流刷新(打开模态那一帧已经在 OpenNewProjectModal 里强制刷过)。
		RefreshNewProjectTemplates(false);
		const Editor::ProjectScaffolder::TemplateInfo* selectedTemplate = SelectedNewProjectTemplate();
		// 模板自带启动场景(声明 defaultScene)⇒ 那个"最小可运行场景"勾选框不可用(灰显 + 理由)。
		const bool templateOwnsStartScene = selectedTemplate != nullptr && !selectedTemplate->DefaultScene.empty();

		// 写盘失败原因只对"同一个落点"有效:名称/位置一改就作废(与新建脚本向导同口径)。
		if (!m_NewProjectFailure.empty() && m_NewProjectFailureFor != NewProjectTargetRoot().u8string())
		{
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
		}
		const std::string nameError = NewProjectNameError();
		const std::string locationError = NewProjectLocationError();
		const std::string templateError = Editor::ProjectScaffolder::ValidateTemplate(m_NewProjectTemplateId);

		// ---- 名称(= 目录名;非法/保留名就地报错)----
		float cursorY = frame.Y + 46.0f;
		const std::string nameLabel = Wui::Tr("modal.newproject.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("modal.newproject.name.placeholder", "Project name (folder name)");
		const bool nameFocused = ctx.Focus() == nameId;
		const bool nameSubmitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewProjectName, m_Theme,
			nameError, &nameA11y);
		cursorY += 46.0f;

		// ---- 位置 + Browse…(原生文件夹对话框;帧边界执行)----
		const float browseW = 84.0f;
		const float locationW = fieldW - browseW - 8.0f;
		const std::string locationLabel = Wui::Tr("modal.newproject.location", "Location");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, locationLabel, m_Theme.TextMuted, 13.0f);
		const Wui::WuiRect locationRect { fieldX, cursorY, locationW, 24.0f };
		Wui::TextFieldA11y locationA11y;
		locationA11y.Label = locationLabel;
		locationA11y.Placeholder = Wui::Tr("modal.newproject.location.placeholder",
			"Folder that will contain the project");
		const bool locationFocused = ctx.Focus() == locationId;
		const bool locationSubmitted = Wui::TextFieldEx(ctx, locationId, locationRect, m_NewProjectLocation,
			m_Theme, locationError, &locationA11y);
		const Wui::WuiRect browseRect { fieldX + locationW + 8.0f, cursorY, browseW, 24.0f };
		const bool browseClicked = Wui::Button(ctx, browseId, browseRect,
			Wui::Tr("modal.newproject.browse", "Browse…"), m_Theme);
		if (browseClicked)
			m_NewProjectBrowsePending = true;   // 下一帧开头弹原生对话框
		cursorY += 46.0f;

		// ---- 实时落点(<位置>/<名称>;绝对路径进节点 Tooltip)----
		const std::filesystem::path target = NewProjectTargetRoot();
		const std::string targetText = target.empty() ? std::string("—") : target.u8string();
		const std::string targetLabel = Wui::Tr("modal.newproject.target", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, targetLabel, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, targetText,
			target.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("project.new.target");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = targetLabel;
			node.Value = targetText;
			node.Tooltip = target.u8string();
			node.Rect = { fieldX, cursorY - 3.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;

		// ---- PROJ-7/T2:模板选择(分段按钮;每项一个稳定 a11y id project.new.template.<id>)----
		// 模板库 = templates/project-*/template.json(引擎自带);name 画在按钮上,description
		// 是悬停提示 + 选中行说明。坏模板(缺 template.json / 缺必需条目)也列出来 —— 选中它
		// 在下面给可读的行内错误,而不是从列表里静默消失。
		{
			const std::string templateLabel = Wui::Tr("modal.newproject.template", "Template");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, m_Theme.TextMuted, 13.0f);
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "group";
				node.Label = templateLabel;
				node.Value = m_NewProjectTemplateId;   // 稳定 id(脚本按它断言,不按显示名)
				node.Tooltip = selectedTemplate != nullptr ? selectedTemplate->Description : std::string();
				node.Rect = { fieldX, cursorY - 3.0f, fieldW, 26.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			float segmentX = fieldX;
			constexpr float segmentW = 168.0f;
			constexpr float segmentGap = 8.0f;
			for (const Editor::ProjectScaffolder::TemplateInfo& info : m_NewProjectTemplates)
			{
				// 放不下就换行(模板数量由磁盘决定,不假设只有两个)。
				if (segmentX > fieldX && segmentX + segmentW > fieldX + fieldW + 0.5f)
				{
					segmentX = fieldX;
					cursorY += 30.0f;
				}
				const std::string segmentLabel = info.Valid ? info.Name
					: Wui::TrFormat("modal.newproject.template.unavailable", "{name} (unavailable)",
						{ { "name", info.Name } });
				const Wui::WuiId segmentId = Wui::HashId(("project.new.template." + info.Id).c_str());
				if (Wui::ButtonEx(ctx, segmentId, { segmentX, cursorY, segmentW, 24.0f }, segmentLabel,
					m_Theme, true, info.Id == m_NewProjectTemplateId,
					info.Valid ? info.Description : info.Error))
				{
					m_NewProjectTemplateId = info.Id;
					m_NewProjectFailure.clear();
					m_NewProjectFailureFor.clear();
				}
				segmentX += segmentW + segmentGap;
			}
			cursorY += 30.0f;
			if (selectedTemplate != nullptr && !selectedTemplate->Description.empty())
			{
				Wui::Label(ctx, { labelX, cursorY }, selectedTemplate->Description, m_Theme.TextMuted, 12.0f);
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template.description");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "text";
				node.Label = templateLabel;
				node.Value = selectedTemplate->Description;
				node.Tooltip = selectedTemplate->Id;
				node.Rect = { labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
			}
			cursorY += 20.0f;
			// 带示例内容的模板(assets/scripts/examples/)给一行"示例内容随模板走"的提示。
			if (selectedTemplate != nullptr && selectedTemplate->HasSamples)
			{
				const std::string hint = Wui::Tr("modal.newproject.template.samples",
					"Brings in the sample scenes, materials and scripts (sample content ships with the template).");
				Wui::Label(ctx, { labelX, cursorY }, hint, m_Theme.Warning, 12.0f);
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("project.new.template.hint");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "text";
				node.Label = templateLabel;
				node.Value = hint;
				node.Tooltip = selectedTemplate->Id;
				node.Rect = { labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
				node.Enabled = true;
				node.Interactive = false;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				cursorY += 20.0f;
			}
		}

		// ---- PROJ-2/T1 + PROJ-7/T2:最小可运行场景(相机 + 方向光;默认勾选)----
		// 勾选 = assets/scenes/Main.wd 由引擎序列化写入一台 Camera3D + 一盏方向光(不含示例);
		// 不勾 = 旧口径的空场景,成功态会给"启动后是空画面"的提示。
		// 所选模板自带启动场景(example ⇒ scenes/3DTest.wd)时该勾选框**禁用**(灰显 + 理由):
		// 清单的 start_scene 指向模板自带场景,不再生成 Main.wd —— 勾选没有意义。
		{
			const Wui::WuiId starterId = Wui::HashId("project.new.starter_scene");
			const Wui::WuiRect starterRect { labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 22.0f };
			const std::string starterLabel = Wui::Tr("modal.newproject.starter",
				"Include a minimal runnable scene (camera + directional light)");
			if (templateOwnsStartScene)
			{
				const std::string lockedReason = Wui::TrFormat("modal.newproject.starter.locked",
					"The template ships its own start scene ({scene}); no minimal scene is generated.",
					{ { "scene", selectedTemplate->DefaultScene } });
				// 禁用态 = 与 Wui::Checkbox 同形状,但灰显且不响应点击(Engine 的 Checkbox 没有
				// 禁用参数,Engine/** 不在本任务白名单内 ⇒ 这里按 ButtonEx 的禁用口径自绘)。
				const Wui::WuiRect box { starterRect.X, starterRect.Y + (starterRect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, box, m_Theme.PanelBg, 3.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, box, m_Theme.Border, 3.0f, 1.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text,
					{ starterRect.X + 24.0f, starterRect.Y + (starterRect.H - 15.0f) * 0.5f, 0, 0 },
					m_Theme.TextDisabled, 0, 1.0f, starterLabel, 15.0f, false });
				Wui::WuiAccessNode node;
				node.Id = starterId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = "shell";
				node.Kind = "checkbox";
				node.Label = starterLabel;
				node.Value = lockedReason;   // 灰件不能没有理由(理由同时进悬停提示)
				node.Tooltip = lockedReason;
				node.Rect = starterRect;
				node.Enabled = false;
				node.Interactive = true;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				Wui::Tooltip(ctx, starterRect, lockedReason);
			}
			else
			{
				Wui::Checkbox(ctx, starterId, starterRect, starterLabel, m_NewProjectStarterScene, m_Theme);
				Wui::Tooltip(ctx, starterRect,
					Wui::Tr("modal.newproject.starter.tooltip",
						"Writes assets/scenes/Main.wd with a Camera3D at [0, 1, 5] and a directional "
						"light, so Launch Project renders a picture. No sample assets, materials or "
						"scripts are copied."));
			}
		}
		cursorY += 30.0f;

		// ---- 模板/落盘的通用错误行(名称与位置各自画在自己的输入框下)----
		const std::string generalError = !templateError.empty() ? templateError : m_NewProjectFailure;
		if (!generalError.empty())
		{
			Wui::Label(ctx, { labelX, cursorY + 2.0f }, generalError, m_Theme.Danger, 12.0f);
			// 行内错误也进无障碍树(标签本身不登记节点):自动化/验证者用 project.new.error
			// 读"为什么建不了",不必截图猜文字。
			Wui::WuiAccessNode errorNode;
			errorNode.Id = Wui::HashId("project.new.error");
			errorNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			errorNode.Panel = "shell";
			errorNode.Kind = "text";
			errorNode.Label = Wui::Tr("modal.newproject.error.label", "Cannot create");
			errorNode.Value = generalError;
			errorNode.Tooltip = generalError;
			errorNode.Rect = { labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 18.0f };
			errorNode.Enabled = true;
			errorNode.Interactive = false;
			errorNode.Visible = true;
			Wui::WuiAccessibility::Get().Register(errorNode);
		}
		else
			Wui::Label(ctx, { labelX, cursorY + 2.0f },
				Wui::Tr("modal.newproject.hint",
					"Content comes from the selected template (templates/project-<id>/**, copied file by "
					"file); the manifest is written by the engine writer; engine C++ stays in Engine/**, "
					"project C++ goes to src/**."),
				m_Theme.TextMuted, 12.0f);

		// ---- 底部按钮(名称/位置/模板任一不通过时创建按钮禁用并带原因)----
		const bool canCreate = nameError.empty() && locationError.empty() && templateError.empty();
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
			Wui::Tr("modal.newproject.ok", "Create"),
			Wui::Tr("modal.newproject.cancel", "Cancel"),
			okId, cancelId, canCreate, m_Theme);

		bool closeRequested = false;
		if ((footerResult == Wui::ModalResult::Confirm
				|| ((nameSubmitted && nameFocused) || (locationSubmitted && locationFocused)))
			&& !justOpened && canCreate)
		{
			// 落盘推迟到下一帧开头(与 Browse… 同一条帧边界纪律)。
			m_NewProjectCreatePending = true;
		}
		else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
		{
			ctx.RecordOp("project", "new-cancel", NewProjectTrimmedName(), m_NewProjectLocation);
			closeRequested = true;
		}

		if (closeRequested)
		{
			m_NewProjectOpen = false;
			m_NewProjectFailure.clear();
			m_NewProjectFailureFor.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}


	// ---- PLUG-AUTH-1:File ▸ New Plugin…(窗口级模态;成功态换成动作按钮)----
	//
	// 入口:File ▸ New Plugin…(menu.file.new_plugin)、插件管理器面板的「新建插件…」
	// (plugins.action.new)、启动器页的「新建插件…」(project.launcher.new_plugin)——
	// 三条路都进同一个 m_NewPlugin* 状态机,内核 = PluginScaffolder。
	//
	// 目标:引擎插件 = <repo>/plugins(启动器形态固定这一项,Project 分段禁用并给理由);
	// 项目插件 = <项目根>/plugins。模板 = templates/plugin-*(6 类);requires != none 的模板
	// 只生成"可编译骨架 + TODO",向导里显式提示需要哪个注册面。
	//
	// 落盘在**帧边界**执行(m_NewPluginCreatePending 由 OnRender 开头消费,与 New Project 同款)。

std::string EditorShell::NewPluginTrimmedName() const{
		return TrimProjectNameText(m_NewPluginName);
	}


std::string EditorShell::NewPluginTrimmedId() const{
		return TrimProjectNameText(m_NewPluginId);
	}


std::filesystem::path EditorShell::NewPluginPluginsRoot() const{
		if (m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project)
		{
			const std::filesystem::path projectRoot = World::Paths::ProjectDir();
			if (projectRoot.empty())
				return {};
			return projectRoot / "plugins";
		}
		return std::filesystem::path(WLD_REPO_ROOT) / "plugins";
	}


std::filesystem::path EditorShell::NewPluginTargetRoot() const{
		const std::filesystem::path root = NewPluginPluginsRoot();
		const std::string name = NewPluginTrimmedName();
		if (root.empty() || name.empty())
			return {};
		return (root / std::filesystem::u8path(name)).lexically_normal();
	}


std::string EditorShell::NewPluginNameError() const{
		return Editor::PluginScaffolder::ValidatePluginName(m_NewPluginName);
	}


std::string EditorShell::NewPluginIdError() const{
		return Editor::PluginScaffolder::ValidatePluginId(m_NewPluginId);
	}


std::string EditorShell::NewPluginTemplateError() const{
		return Editor::PluginScaffolder::ValidateTemplate(m_NewPluginTemplateId, m_NewPluginTarget);
	}


std::string EditorShell::NewPluginTargetError() const{
		if (m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project
			&& (m_LauncherMode || World::Paths::ProjectDir().empty()))
		{
			return Wui::Tr("modal.newplugin.error.no_project",
				"Open a project first — project plugins live in <project>/plugins.");
		}
		const std::filesystem::path root = NewPluginPluginsRoot();
		if (root.empty())
			return Wui::Tr("modal.newplugin.error.root_missing",
				"Cannot resolve the plugins directory for this target");
		// 名称/ID 的错误画在各自输入框下,这里不重复报(与 New Project 的模板错误同款)。
		if (!NewPluginNameError().empty() || !NewPluginIdError().empty())
			return {};
		return Editor::PluginScaffolder::ValidateTarget(root, m_NewPluginName, m_NewPluginId);
	}


void EditorShell::EnsureNewPluginTemplateSelection(){
		// 只调整"当前选中的模板",**不碰** m_NewPluginTemplates —— 调用它的路径可能发生在
		// 一帧的中途(目标分段按钮的点击),而同一帧后面还持有指向该向量的指针
		// (PLUG-AUTH-1 实测:帧中途重建向量会让早先抓的 selectedTemplate 悬垂,
		//  读 Description 直接 std::length_error "string too long")。
		const auto allowed = [this](const Editor::PluginScaffolder::PluginTemplateInfo& info)
		{
			return m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Engine
				? info.EngineOk : info.ProjectOk;
		};
		const auto contains = [this](const std::string& id)
		{
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
				if (info.Id == id)
					return true;   // 坏模板也列出来(选中给行内错误),不静默消失
			return false;
		};
		if (contains(m_NewPluginTemplateId))
			return;
		m_NewPluginTemplateId.clear();
		for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
		{
			if (info.Id == "empty" && allowed(info))
			{
				m_NewPluginTemplateId = info.Id;
				break;
			}
		}
		if (m_NewPluginTemplateId.empty())
		{
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			{
				if (allowed(info))
				{
					m_NewPluginTemplateId = info.Id;
					break;
				}
			}
		}
		if (m_NewPluginTemplateId.empty() && !m_NewPluginTemplates.empty())
			m_NewPluginTemplateId = m_NewPluginTemplates.front().Id;
		if (m_NewPluginTemplateId.empty())
			m_NewPluginTemplateId = "empty";
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
	}


void EditorShell::RefreshNewPluginTemplates(bool force){
		const double now = ShellNowSeconds();
		if (!force && m_NewPluginTemplatesScannedAt > 0.0
			&& now - m_NewPluginTemplatesScannedAt < 2.0)
			return;
		m_NewPluginTemplatesScannedAt = now;
		m_NewPluginTemplates = Editor::PluginScaffolder::ListTemplates();
		EnsureNewPluginTemplateSelection();
	}


const Editor::PluginScaffolder::PluginTemplateInfo* EditorShell::SelectedNewPluginTemplate() const{
		for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			if (info.Id == m_NewPluginTemplateId)
				return &info;
		return nullptr;
	}


void EditorShell::RunNewPluginCreate(Wui::WuiContext& ctx){
		RefreshNewPluginTemplates(false);
		const Editor::PluginScaffolder::PluginTemplateInfo* selected = SelectedNewPluginTemplate();
		const std::string nameError = NewPluginNameError();
		const std::string idError = NewPluginIdError();
		const std::string templateError = NewPluginTemplateError();
		const std::string targetError = NewPluginTargetError();
		// Create 按钮在任一校验不过时是禁用态;这里再夹一次,防止"绕过按钮的第二次调用"。
		if (selected == nullptr || !nameError.empty() || !idError.empty()
			|| !templateError.empty() || !targetError.empty())
		{
			const std::string blocked = !templateError.empty() ? templateError
				: (!targetError.empty() ? targetError
					: (!nameError.empty() ? nameError : idError));
			m_NewPluginFailure = blocked.empty()
				? Wui::Tr("modal.newplugin.error.no_template",
					"No plugin template is available under <checkout>/templates/plugin-*/.")
				: blocked;
			m_NewPluginFailureFor = NewPluginTargetRoot().u8string();
			return;
		}

		Editor::PluginScaffolder::PluginScaffoldRequest request;
		request.Target = m_NewPluginTarget;
		request.TemplateId = m_NewPluginTemplateId;
		request.Name = NewPluginTrimmedName();
		request.PluginId = NewPluginTrimmedId();

		const Editor::PluginScaffolder::PluginScaffoldResult result =
			Editor::PluginScaffolder::Scaffold(NewPluginPluginsRoot(), request);
		if (!result.Ok)
		{
			m_NewPluginFailure = result.Error;
			m_NewPluginFailureFor = NewPluginTargetRoot().u8string();
			WLD_CORE_WARN("[new-plugin] scaffold failed: {0}", result.Error);
			return;
		}

		m_NewPluginCreated = true;
		m_NewPluginRoot = result.PluginRoot;
		m_NewPluginFiles = result.Files;
		m_NewPluginCreatedId = request.PluginId;
		m_NewPluginCreatedTarget = Editor::PluginScaffolder::TargetName(request.Target);
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
		ctx.RecordOp("plugin", "new", request.PluginId,
			result.PluginRoot.u8string() + " [template " + m_NewPluginTemplateId + " target "
				+ m_NewPluginCreatedTarget + " files " + std::to_string(result.Files.size()) + "]");
		PushNotice(Wui::TrFormat("notice.newplugin.created", "Created the plugin at {path}",
			{ { "path", result.PluginRoot.u8string() } }));
		WLD_CORE_INFO("[new-plugin] created '{0}' (template '{1}', target {2}) at '{3}': {4} files",
			request.PluginId, m_NewPluginTemplateId, m_NewPluginCreatedTarget,
			result.PluginRoot.u8string(), result.Files.size());
	}


void EditorShell::OpenNewPluginModal(Wui::WuiContext& ctx){
		m_NewPluginOpen = true;
		m_NewPluginOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewPluginCreated = false;
		m_NewPluginRoot.clear();
		m_NewPluginFiles.clear();
		m_NewPluginCreatedId.clear();
		m_NewPluginCreatedTarget.clear();
		m_NewPluginFailure.clear();
		m_NewPluginFailureFor.clear();
		m_NewPluginCreatePending = false;

		// 目标默认:能建项目插件时 = 项目插件(编辑器/面板入口的常见场景);
		// 启动器形态(没有项目)= 引擎插件 —— Project 分段禁用并给理由(D1=允许引擎插件)。
		const bool projectAvailable = !m_LauncherMode && !World::Paths::ProjectDir().empty();
		m_NewPluginTarget = projectAvailable
			? Editor::PluginScaffolder::PluginTarget::Project
			: Editor::PluginScaffolder::PluginTarget::Engine;
		RefreshNewPluginTemplates(true);   // 打开这一帧就把模板列出来(磁盘事实)
		if (m_NewPluginName.empty())
			m_NewPluginName = "MyPlugin";
		if (m_NewPluginId.empty() || !m_NewPluginIdTouched)
			m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
		ctx.SetModal(Wui::HashId("modal.newplugin"));
		ctx.RecordOp("plugin", "new-ask", NewPluginTrimmedName(),
			Editor::PluginScaffolder::TargetName(m_NewPluginTarget));
	}


void EditorShell::DrawNewPluginModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.newplugin");
		if (m_NewPluginOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!m_NewPluginOpen)
			return;

		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("modal.newplugin.title", "New Plugin");
		frameDesc.Size = { 680.0f, 470.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
		{
			// 模态被别的路径接管/收口:同步清掉宿主状态,避免状态与真实模态脱节。
			m_NewPluginOpen = false;
			return;
		}

		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 16.0f;

		// ---- 成功态:落盘目录 + 文件清单 + 「在内容浏览器中定位 / 打开插件目录 / 关闭」----
		if (m_NewPluginCreated)
		{
			const Editor::PluginScaffolder::PluginTemplateInfo* templateInfo = SelectedNewPluginTemplate();
			const std::string templateName = templateInfo != nullptr ? templateInfo->Name
				: m_NewPluginTemplateId;
			float cursorY = frame.Y + 48.0f;
			Wui::Label(ctx, { labelX, cursorY }, Wui::TrFormat("modal.newplugin.created",
				"Created the plugin (template: {template}, target: {target}):",
				{ { "template", templateName }, { "target", m_NewPluginCreatedTarget } }),
				m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			Wui::Label(ctx, { labelX, cursorY }, m_NewPluginRoot.u8string(), m_Theme.Text, 13.0f);
			cursorY += 24.0f;
			{
				// plugin.new.done:验证者按 id 读"落盘目录"与 id/target/文件数,不用从提示文案里猜。
				std::string files;
				const size_t shown = std::min<size_t>(m_NewPluginFiles.size(), 8);
				for (size_t index = 0; index < shown; ++index)
				{
					if (index > 0)
						files += ", ";
					files += m_NewPluginFiles[index];
				}
				if (m_NewPluginFiles.size() > shown)
					files += ", …";
				RegisterShellAccessNode(Wui::HashId("plugin.new.done"), "text",
					Wui::Tr("modal.newplugin.created.label", "Created plugin"),
					"path=" + m_NewPluginRoot.u8string() + " id=" + m_NewPluginCreatedId
						+ " target=" + m_NewPluginCreatedTarget + " files="
						+ std::to_string(m_NewPluginFiles.size()),
					{ labelX, frame.Y + 46.0f, frame.W - (labelX - frame.X) - 16.0f, 40.0f },
					false, true, files);
			}
			if (!m_NewPluginFiles.empty())
			{
				std::string files;
				const size_t shown = std::min<size_t>(m_NewPluginFiles.size(), 8);
				for (size_t index = 0; index < shown; ++index)
				{
					if (index > 0)
						files += ", ";
					files += m_NewPluginFiles[index];
				}
				if (m_NewPluginFiles.size() > shown)
					files += ", …";
				Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.newplugin.created.files", "Files: ")
						+ files, m_Theme.TextMuted, 12.0f);
				cursorY += 20.0f;
			}
			Wui::Label(ctx, { labelX, cursorY }, Wui::Tr("modal.newplugin.created.hint",
				"Engine plugins: re-run the engine CMake configure and build ALL_BUILD, then restart "
				"the editor. Project plugins: build the project (build.cmd) — its plugin DLL lands in "
				"<project>/build/x64-<config>/bin/<config>/plugins/<config>/."),
				m_Theme.TextMuted, 12.0f);
			cursorY += 20.0f;
			// 引擎插件不在内容浏览器的第三根里(那儿只显示 <项目根>/plugins)⇒ locate 禁用,
			// 并把理由画成可读文字 + 只读 a11y 节点(禁用按钮本身没有 tooltip 槽位)。
			const bool canLocate = m_NewPluginCreatedTarget == std::string("project")
				&& !World::Paths::ProjectDir().empty();
			if (!canLocate)
			{
				const std::string reason = Wui::Tr("modal.newplugin.locate.reason",
					"Locate is for project plugins: engine plugins live in <engine>/plugins, and the "
					"content browser only shows <project>/plugins.");
				Wui::Label(ctx, { labelX, cursorY }, reason, m_Theme.TextMuted, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.locate.reason"), "text",
					Wui::Tr("modal.newplugin.locate", "Locate in Content Browser"), reason,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, reason);
			}

			const Wui::ModalButtonDesc buttons[3] = {
				{ Wui::Tr("modal.newplugin.locate", "Locate in Content Browser"),
					Wui::HashId("plugin.new.locate"), canLocate },
				{ Wui::Tr("modal.newplugin.open", "Open Plugin Folder"),
					Wui::HashId("plugin.new.open"), true },
				{ Wui::Tr("modal.newplugin.close", "Close"),
					Wui::HashId("plugin.new.close"), true },
			};
			const int clicked = Wui::ModalButtons(ctx, frame, buttons, 3, m_Theme);
			if (clicked == 0 && canLocate)
			{
				std::string message;
				if (RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope::ProjectPlugins,
						m_NewPluginRoot, &message))
					ctx.RecordOp("plugin", "new-locate", m_NewPluginCreatedId,
						m_NewPluginRoot.generic_string());
				else
					PushNotice(message);
			}
			else if (clicked == 1)
			{
				const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open",
					m_NewPluginRoot.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
				{
					PushNotice(Wui::TrFormat("notice.newplugin.explorer_failed",
						"Could not open the plugin folder: {path}",
						{ { "path", m_NewPluginRoot.u8string() } }));
					WLD_CORE_WARN("[new-plugin] ShellExecuteW failed ({0}) for '{1}'",
						static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)),
						m_NewPluginRoot.u8string());
				}
				else
				{
					ctx.RecordOp("plugin", "new-open-explorer", m_NewPluginCreatedId,
						m_NewPluginRoot.u8string());
				}
			}
			else if (clicked == 2 || escapePressed)
			{
				m_NewPluginOpen = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
			return;
		}

		const Wui::WuiId nameId = Wui::HashId("plugin.new.name");
		const Wui::WuiId idId = Wui::HashId("plugin.new.id");
		const Wui::WuiId okId = Wui::HashId("plugin.new.create");
		const Wui::WuiId cancelId = Wui::HashId("plugin.new.cancel");
		const bool justOpened = ctx.Frame() == m_NewPluginOpenedFrame;
		RefreshNewPluginTemplates(false);
		const bool projectAvailable = !m_LauncherMode && !World::Paths::ProjectDir().empty();

		// 写盘失败原因只对"同一个落点"有效:名称/ID/目标一改就作废(与 New Project 同口径)。
		if (!m_NewPluginFailure.empty()
			&& m_NewPluginFailureFor != NewPluginTargetRoot().u8string())
		{
			m_NewPluginFailure.clear();
			m_NewPluginFailureFor.clear();
		}
		const std::string nameError = NewPluginNameError();
		const std::string idError = NewPluginIdError();
		const std::string templateError = NewPluginTemplateError();
		const std::string targetError = NewPluginTargetError();

		float cursorY = frame.Y + 46.0f;

		// ---- 目标(引擎 / 项目;启动器形态 Project 禁用并给理由)----
		{
			const std::string targetLabel = Wui::Tr("modal.newplugin.target", "Target");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, targetLabel, m_Theme.TextMuted, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.target"), "group", targetLabel,
				Editor::PluginScaffolder::TargetName(m_NewPluginTarget),
				{ fieldX, cursorY - 3.0f, fieldW, 26.0f }, false, true, std::string());
			const float segmentW = 168.0f;
			const float segmentGap = 8.0f;
			const bool engineActive =
				m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Engine;
			const std::filesystem::path engineRoot =
				std::filesystem::path(WLD_REPO_ROOT) / "plugins";
			if (Wui::ButtonEx(ctx, Wui::HashId("plugin.new.scope.engine"),
					{ fieldX, cursorY, segmentW, 24.0f },
					Wui::Tr("modal.newplugin.scope.engine", "Engine Plugin"), m_Theme, true,
					engineActive, engineRoot.u8string()))
			{
				if (!engineActive)
				{
					m_NewPluginTarget = Editor::PluginScaffolder::PluginTarget::Engine;
					m_NewPluginFailure.clear();
					m_NewPluginFailureFor.clear();
					// 帧中途只重选模板,不重建模板向量(见 EnsureNewPluginTemplateSelection 注释)。
					EnsureNewPluginTemplateSelection();
					if (!m_NewPluginIdTouched)
						m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
				}
			}
			const std::filesystem::path projectPluginsRoot =
				World::Paths::ProjectDir().empty() ? std::filesystem::path()
					: World::Paths::ProjectDir() / "plugins";
			const std::string projectReason = projectAvailable ? projectPluginsRoot.u8string()
				: Wui::Tr("modal.newplugin.scope.project.locked",
					"Open or create a project first — project plugins live in <project>/plugins.");
			const bool projectActive =
				m_NewPluginTarget == Editor::PluginScaffolder::PluginTarget::Project;
			if (Wui::ButtonEx(ctx, Wui::HashId("plugin.new.scope.project"),
					{ fieldX + segmentW + segmentGap, cursorY, segmentW, 24.0f },
					Wui::Tr("modal.newplugin.scope.project", "Project Plugin"), m_Theme,
					projectAvailable, projectActive, projectReason)
				&& projectAvailable && !projectActive)
			{
				m_NewPluginTarget = Editor::PluginScaffolder::PluginTarget::Project;
				m_NewPluginFailure.clear();
				m_NewPluginFailureFor.clear();
				EnsureNewPluginTemplateSelection();
				if (!m_NewPluginIdTouched)
					m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
			}
			cursorY += 34.0f;
		}

		// 目标分段按钮的点击可能刚刚改过 m_NewPluginTarget(并且只重选了模板 id,不动向量):
		// 这里**重新取一次**选中模板指针 —— 指针必须晚于任何可能改动 m_NewPluginTemplates 的调用。
		const Editor::PluginScaffolder::PluginTemplateInfo* selectedTemplate = SelectedNewPluginTemplate();

		// ---- 模板(分段按钮;每项一个稳定 a11y id plugin.new.template.<id>)----
		{
			const std::string templateLabel = Wui::Tr("modal.newplugin.template", "Template");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, m_Theme.TextMuted, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.template"), "group", templateLabel,
				m_NewPluginTemplateId,
				{ fieldX, cursorY - 3.0f, fieldW, 26.0f }, false, true,
				selectedTemplate != nullptr ? selectedTemplate->Description : std::string());
			float segmentX = fieldX;
			const float segmentW = 168.0f;
			const float segmentGap = 8.0f;
			float rowY = cursorY;
			for (const Editor::PluginScaffolder::PluginTemplateInfo& info : m_NewPluginTemplates)
			{
				if (segmentX > fieldX && segmentX + segmentW > fieldX + fieldW + 0.5f)
				{
					segmentX = fieldX;
					rowY += 30.0f;
				}
				const std::string surface = PluginRequiresSurface(info.Requires);
				std::string tooltip = info.Valid ? info.Description : info.Error;
				if (info.Valid && !surface.empty() && info.Requires != "none")
				{
					tooltip += "\n" + Wui::TrFormat("modal.newplugin.template.requires",
						"Needs the {surface} registration surface (the generated skeleton compiles today).",
						{ { "surface", surface } });
				}
				const std::string segmentLabel = info.Valid ? info.Name
					: Wui::TrFormat("modal.newplugin.template.unavailable", "{name} (unavailable)",
						{ { "name", info.Name } });
				if (Wui::ButtonEx(ctx,
						Wui::HashId(("plugin.new.template." + info.Id).c_str()),
						{ segmentX, rowY, segmentW, 24.0f }, segmentLabel, m_Theme, true,
						info.Id == m_NewPluginTemplateId, tooltip))
				{
					m_NewPluginTemplateId = info.Id;
					m_NewPluginFailure.clear();
					m_NewPluginFailureFor.clear();
				}
				segmentX += segmentW + segmentGap;
			}
			cursorY = rowY + 30.0f;
			if (selectedTemplate != nullptr && !selectedTemplate->Description.empty())
			{
				Wui::Label(ctx, { labelX, cursorY }, selectedTemplate->Description,
					m_Theme.TextMuted, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.template.description"), "text",
					templateLabel, selectedTemplate->Description,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, selectedTemplate->Id);
			}
			cursorY += 20.0f;
			if (selectedTemplate != nullptr && selectedTemplate->Valid
				&& selectedTemplate->Requires != "none")
			{
				const std::string requiresText = Wui::TrFormat("modal.newplugin.template.requires",
					"Needs the {surface} registration surface (the generated skeleton compiles today).",
					{ { "surface", PluginRequiresSurface(selectedTemplate->Requires) } });
				Wui::Label(ctx, { labelX, cursorY }, requiresText, m_Theme.Warning, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.template.requires"), "text",
					templateLabel, requiresText,
					{ labelX, cursorY - 2.0f, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, selectedTemplate->Requires);
			cursorY += 20.0f;
			}
		}

		// ---- 名称(= 目录名 + 默认显示名;非法就地报错)----
		{
			const std::string nameLabel = Wui::Tr("modal.newplugin.name", "Name");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, m_Theme.TextMuted, 13.0f);
			const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
			Wui::TextFieldA11y nameA11y;
			nameA11y.Label = nameLabel;
			nameA11y.Placeholder = Wui::Tr("modal.newplugin.name.placeholder",
				"Plugin name (folder name; letters, digits, '-' and '_')");
			const bool nameFocused = ctx.Focus() == nameId;
			const std::string nameBefore = m_NewPluginName;
			const bool nameSubmitted = Wui::TextFieldEx(ctx, nameId, nameRect, m_NewPluginName,
				m_Theme, nameError, &nameA11y);
			// 名称变化时同步默认 ID(用户手动改过 ID 之后不再改写)。
			if (m_NewPluginName != nameBefore && !m_NewPluginIdTouched)
				m_NewPluginId = DefaultPluginIdForName(NewPluginTrimmedName());
			cursorY += 46.0f;
			if (!nameError.empty())
				Wui::Label(ctx, { fieldX, cursorY - 20.0f }, nameError, m_Theme.Danger, 12.0f);

			// ---- 插件 ID(反域名;唯一性在 Create 前由 ValidateTarget 查)----
			const std::string idLabel = Wui::Tr("modal.newplugin.id", "Plugin ID");
			Wui::Label(ctx, { labelX, cursorY + 5.0f }, idLabel, m_Theme.TextMuted, 13.0f);
			const Wui::WuiRect idRect { fieldX, cursorY, fieldW, 24.0f };
			Wui::TextFieldA11y idA11y;
			idA11y.Label = idLabel;
			idA11y.Placeholder = Wui::Tr("modal.newplugin.id.placeholder",
				"Reverse-domain ID (e.g. com.studio.my-plugin)");
			const bool idFocused = ctx.Focus() == idId;
			const std::string idBefore = m_NewPluginId;
			const bool idSubmitted = Wui::TextFieldEx(ctx, idId, idRect, m_NewPluginId,
				m_Theme, idError, &idA11y);
			if (m_NewPluginId != idBefore)
				m_NewPluginIdTouched = true;
			cursorY += 46.0f;
			if (!idError.empty())
				Wui::Label(ctx, { fieldX, cursorY - 20.0f }, idError, m_Theme.Danger, 12.0f);

			// ---- 实时落点(只读;绝对路径进节点 Tooltip)----
			const std::filesystem::path target = NewPluginTargetRoot();
			const std::string targetText = target.empty() ? std::string("—") : target.u8string();
			const std::string locationLabel = Wui::Tr("modal.newplugin.location", "Will create");
			Wui::Label(ctx, { labelX, cursorY + 3.0f }, locationLabel, m_Theme.TextMuted, 12.0f);
			Wui::Label(ctx, { fieldX, cursorY + 1.0f }, targetText,
				target.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);
			RegisterShellAccessNode(Wui::HashId("plugin.new.location"), "text", locationLabel,
				targetText, { fieldX, cursorY - 3.0f, fieldW, 20.0f }, false, true,
				target.u8string());
			cursorY += 24.0f;

			// ---- 模板/落点的通用错误行 + 提示 ----
			const std::string generalError = !templateError.empty() ? templateError
				: (!targetError.empty() ? targetError : m_NewPluginFailure);
			if (!generalError.empty())
			{
				Wui::Label(ctx, { labelX, cursorY + 2.0f }, generalError, m_Theme.Danger, 12.0f);
				RegisterShellAccessNode(Wui::HashId("plugin.new.error"), "text",
					Wui::Tr("modal.newplugin.error.label", "Cannot create"), generalError,
					{ labelX, cursorY, frame.W - (labelX - frame.X) - 16.0f, 18.0f },
					false, true, generalError);
			}
			else
			{
				Wui::Label(ctx, { labelX, cursorY + 2.0f }, Wui::Tr("modal.newplugin.hint",
					"The generated plugin.we.yaml is validated by the plugin loader before anything "
					"lands; an existing directory is never overwritten. The plugin DLL is produced by "
					"the engine or project build, not by this wizard."), m_Theme.TextMuted, 12.0f);
			}

			// ---- 底部按钮(名称/ID/模板/落点任一不过时 Create 禁用并带原因)----
			const bool canCreate = selectedTemplate != nullptr && nameError.empty()
				&& idError.empty() && templateError.empty() && targetError.empty();
			const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, frame,
				Wui::Tr("modal.newplugin.ok", "Create"),
				Wui::Tr("modal.newplugin.cancel", "Cancel"),
				okId, cancelId, canCreate, m_Theme);

			bool closeRequested = false;
			if ((footerResult == Wui::ModalResult::Confirm
					|| ((nameSubmitted && nameFocused) || (idSubmitted && idFocused)))
				&& !justOpened && canCreate)
			{
				// 落盘推迟到下一帧开头(与 New Project 同一条帧边界纪律)。
				m_NewPluginCreatePending = true;
			}
			else if (footerResult == Wui::ModalResult::Cancel || escapePressed)
			{
				ctx.RecordOp("plugin", "new-cancel", NewPluginTrimmedName(),
					Editor::PluginScaffolder::TargetName(m_NewPluginTarget));
				closeRequested = true;
			}
			if (closeRequested)
			{
				m_NewPluginOpen = false;
				m_NewPluginFailure.clear();
				m_NewPluginFailureFor.clear();
				ctx.ClearModal();
			}
		}
		Wui::EndModalFrame(ctx);
	}


	// ---- PROJ-2/T1:项目启动器 / File ▸ Open Project… / 运行 ▸ 启动项目(Runtime) ----
	//
	// 启动决策(显式项目 / 自动打开最近一次 / 显示启动器)在 EditorLayer;本文件只做:
	//   * 启动器模态的渲染与四个动作(新建项目向导 / 新建示例项目向导(预选 example 模板) /
	//     打开项目… / 关闭·退出);
	//   * 最近列表的缓存刷新与"移除失效项";
	//   * File ▸ Open Project… 的入口(与启动器共用同一条"选目录 → 校验 → 重启"路径);
	//   * 运行 ▸ 启动项目(Runtime)= EditorLayer::LaunchProjectRuntime(当前项目根)。
	// 原生目录对话框与重启都在**帧边界**发生:这里只置 m_OpenProjectBrowsePending。

void EditorShell::RefreshRecentProjectsIfStale(bool force){
		// 最近列表只在下拉/启动器可见时按 1 秒节流刷新(逐帧查盘 + 解析清单会白烧 IO)。
		const double now = ShellNowSeconds();
		if (!force && m_RecentProjectsLoadedAt > 0.0 && now - m_RecentProjectsLoadedAt < 1.0)
			return;
		m_RecentProjectsLoadedAt = now;
		m_RecentProjects = Editor::ProjectLauncher::LoadRecent();
	}


const std::vector<Editor::RecentProjectEntry>& EditorShell::RecentProjects(){
		RefreshRecentProjectsIfStale();
		return m_RecentProjects;
	}


void EditorShell::RequestOpenProjectBrowse(Wui::WuiContext& ctx){
		if (m_OpenProjectBrowsePending)
			return;
		m_OpenProjectBrowsePending = true;   // 下一帧开头弹原生"选择项目目录"
		ctx.RecordOp("project", "open-ask", "", "");
	}


void EditorShell::RunOpenProjectBrowse(Wui::WuiContext& ctx){
		// 原生"选择文件夹"对话框:只在帧边界打开(取消返回空串 → 什么都不做)。
		const std::string folder = FileDialogs::SelectFolder("Open Project");
		if (folder.empty())
			return;
		const std::filesystem::path root = std::filesystem::u8path(folder);
		std::string reason;
		if (!Editor::ProjectLauncher::IsValidProjectRoot(root, &reason))
		{
			// 选中的目录不是项目(缺清单/清单解析失败):通知里给可读原因,不重启。
			ctx.RecordOp("project", "open-failed", folder, reason);
			WLD_CORE_WARN("[project] open failed: '{0}': {1}", folder, reason);
			PushNotice(Wui::TrFormat("notice.project.open_failed", "{reason}: {path}",
				{ { "reason", reason }, { "path", folder } }));
			return;
		}
		ctx.RecordOp("project", "open", Editor::ProjectLauncher::DisplayName(root), root.u8string());
		m_Editor.RelaunchWithProject(root);
	}


void EditorShell::LaunchCurrentProjectRuntime(){
		// 目标项目 = 当前项目根(定位 Runtime / 命令行 / 日志 / 操作记录全在 EditorLayer)。
		m_Editor.LaunchProjectRuntime(World::Paths::ProjectDir());
	}


void EditorShell::DrawProjectLauncherModal(Wui::WuiContext& ctx){
		const Wui::WuiId modalId = Wui::HashId("modal.project_launcher");
		// 其它模态在前时让位(启动期最可能的来源是错误框);它们收口后启动器会回到前台。
		const bool blockedByOtherModal = m_NewProjectOpen || m_NewPluginOpen || m_NewCppScriptOpen
			|| m_NewLuaSystemOpen
			|| m_ImportModalOpen ||
			m_PrefabPendingAction != PrefabPendingAction::None || m_Editor.ShowUnsavedModal() ||
			m_Editor.ShowErrorModal() || m_Editor.ShowCookingProgress();
		const bool visible = m_Editor.ShowProjectLauncher() && !blockedByOtherModal;
		// PROJ-7/T3b:刚回到前台的那一帧吞掉点击/Esc(理由见头文件 m_LauncherModalVisible 注释):
		// 向导的"取消"与启动器按钮在同一帧收口时,上一次输入的落点会命中启动器按钮。
		const bool justBecameVisible = visible && !m_LauncherModalVisible;
		m_LauncherModalVisible = visible;
		if (visible)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();
		if (!visible)
		{
			// PROJ-5R/T1:模态收口/让位时"删除…"流程不悬空 —— 下次打开回到最近列表态。
			m_LauncherDeleteOpen = false;
			m_LauncherDeletePath.clear();
			m_LauncherDeleteName.clear();
			m_LauncherDeleteError.clear();
			return;
		}

		const LauncherDeleteVariant deleteVariant = ClassifyLauncherDeleteTarget(m_LauncherDeletePath);
		Wui::WuiRect frame;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		// PROJ-5R/T1:确认模态的标题按目标当前状态切换(模态 id 不变 —— 确认/失效都是同一个
		// 模态框;确认态下列表行不再绘制/登记,所以模态期间点不到其它项目行)。
		frameDesc.Title = !m_LauncherDeleteOpen
			? Wui::Tr("modal.launcher.title", "Choose a Project")
			: deleteVariant == LauncherDeleteVariant::Delete
				? Wui::Tr("modal.project_delete.title", "Permanently delete this project?")
				: deleteVariant == LauncherDeleteVariant::Missing
					? Wui::Tr("modal.project_delete.missing.title", "This folder no longer exists")
					: Wui::Tr("modal.project_delete.reject.title", "Cannot delete this folder");
		frameDesc.Size = { 660.0f, 430.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, m_Theme))
			return;
		if (m_LauncherDeleteOpen)
		{
			DrawLauncherDeleteFlow(ctx, frame, escapePressed);
			Wui::EndModalFrame(ctx);
			return;
		}

		const float pad = 16.0f;
		// 行高 24 + 2 间距;列表本体放在标题行与提示行之间,由滚动区裁剪
		// (上限 30 条,超出时滚轮/键盘滚动;行数少时不出现滚动)。
		const float rowHeight = 24.0f;
		const float rowWidth = frame.W - pad * 2.0f;
		float cursorY = frame.Y + 46.0f;
		Wui::Label(ctx, { frame.X + pad, cursorY }, Wui::Tr("modal.launcher.recent", "Recent projects"),
			m_Theme.Text, 13.0f);

		// PROJ-4/T1(P2):搜索框 + 清空按钮。占位提示是单独画的 Label,读屏/脚本读不到 →
		// 显式喂给 TextFieldA11y(与设置页同一口径);清空在空输入时禁用并给理由。
		const float clearWidth = 64.0f;
		const float searchWidth = 220.0f;
		const Wui::WuiRect searchRect { frame.X + frame.W - pad - clearWidth - 8.0f - searchWidth,
			cursorY - 3.0f, searchWidth, 24.0f };
		const Wui::WuiRect clearRect { frame.X + frame.W - pad - clearWidth, cursorY - 3.0f, clearWidth, 24.0f };
		Wui::TextFieldA11y searchA11y;
		searchA11y.Label = Wui::Tr("modal.launcher.search.a11y", "Search recent projects");
		searchA11y.Placeholder = Wui::Tr("modal.launcher.search.hint", "Search projects…");
		Wui::TextField(ctx, Wui::HashId("project.launcher.search"), searchRect, m_LauncherSearch,
			m_Theme, nullptr, &searchA11y);
		if (m_LauncherSearch.empty())
		{
			Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f },
				Wui::Tr("modal.launcher.search.hint", "Search projects…"), m_Theme.TextDisabled, 12.0f);
		}
		if (Wui::ButtonEx(ctx, Wui::HashId("project.launcher.search.clear"), clearRect,
			Wui::Tr("modal.launcher.search.clear", "Clear"), m_Theme, !m_LauncherSearch.empty(), false,
			m_LauncherSearch.empty()
				? Wui::Tr("modal.launcher.search.clear_disabled",
					"Nothing to clear — the search box is empty")
				: std::string()))
		{
			m_LauncherSearch.clear();
			m_LauncherScrollY = 0.0f;
		}
		cursorY += 30.0f;

		RefreshRecentProjectsIfStale();
		std::string openPendingPath;
		std::string openPendingName;
		// PROJ-5/T1:点"删除…"⇒下一帧起进入两步确认(本帧先画列表,不改流程状态)。
		std::string deletePendingPath;
		std::string deletePendingName;
		// 过滤只决定"画哪些行";行 id、动作、打开目标一律用**原列表下标** ——
		// 过滤状态下点第一行打开的仍是它自己对应的项目,不会错位到筛选后的第一条。
		const std::string loweredNeedle = AsciiLowerCopy(m_LauncherSearch);
		std::vector<size_t> visibleRows;
		const size_t recentCount = std::min<size_t>(m_RecentProjects.size(), 30);
		for (size_t i = 0; i < recentCount; ++i)
		{
			const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
			if (!ContainsCaseInsensitive(entry.Name, loweredNeedle) &&
				!ContainsCaseInsensitive(entry.Path, loweredNeedle))
				continue;
			visibleRows.push_back(i);
		}
		if (m_RecentProjects.empty())
		{
			Wui::Label(ctx, { frame.X + pad, cursorY + 4.0f },
				Wui::Tr("modal.launcher.empty",
					"No recent projects yet — create a new one, or open a folder that contains project.we.yaml."),
				m_Theme.TextMuted, 13.0f);
		}
		else if (visibleRows.empty())
		{
			// P2:无匹配 ⇒ 空态;文案登记成独立 a11y 节点,脚本按 modal.launcher.no_match 读它。
			const std::string noMatch = Wui::Tr("modal.launcher.no_match",
				"No matching projects — try Open Project… to pick a folder.");
			Wui::Label(ctx, { frame.X + pad, cursorY + 4.0f }, noMatch, m_Theme.TextMuted, 13.0f);
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("modal.launcher.no_match");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = noMatch;
			node.Value = noMatch;
			node.Rect = { frame.X + pad, cursorY + 4.0f, rowWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		else
		{
			const Wui::WuiRect listClip { frame.X + pad, cursorY, rowWidth,
				std::max(40.0f, frame.Y + frame.H - 82.0f - cursorY) };
			const float contentHeight = static_cast<float>(visibleRows.size()) * (rowHeight + 2.0f);
			Wui::BeginScrollArea(ctx, listClip, contentHeight, m_LauncherScrollY, m_Theme,
				Wui::HashId("project.launcher.recent.scroll"));
			float rowY = listClip.Y - m_LauncherScrollY;
			for (const size_t i : visibleRows)
			{
				const Editor::RecentProjectEntry& entry = m_RecentProjects[i];
				const std::string path = entry.Path;
				const Wui::WuiRect row { listClip.X, rowY, listClip.W, rowHeight };
				rowY += rowHeight + 2.0f;
				if (!ctx.ClipAllows(row))
					continue;   // 滚出视口的行不绘制也不登记(与属性面板滚动区同一口径)
				// PROJ-5R/T1(v2):行尾只剩"删除…"(危险色)——"移除"入口按用户二次指令去掉;
				// 只删列表项的动作改由"目录已不存在"确认模态里的"从列表移除"承接。
				const float deleteWidth = 96.0f;
				const Wui::WuiRect deleteRect { row.X + row.W - deleteWidth, row.Y, deleteWidth, rowHeight };
				const std::string name = entry.Name.empty() ? path : entry.Name;
				if (entry.Valid)
				{
					// 有效行:整行"打开"按钮,宽度让到"删除…"左侧。
					const std::string label = Wui::EllipsizeMiddleToWidth(ctx, name + "    " + entry.Path,
						deleteRect.X - row.X - 16.0f, 15.0f);
					const std::string tooltip = entry.LastOpened.empty()
						? entry.Path : entry.Path + "\n" + entry.LastOpened;
					if (Wui::ButtonEx(ctx,
						Wui::HashId(("project.launcher.recent." + std::to_string(i)).c_str()),
						{ row.X, row.Y, deleteRect.X - row.X - 6.0f, rowHeight }, label, m_Theme,
						true, false, tooltip))
					{
						openPendingPath = path;
						openPendingName = name;
					}
				}
				else
				{
					// 失效行:名称 + 路径 + 失效标记(红)在左(不可打开);右侧仍可"删除…"。
					const std::string text = name + "  —  " + entry.Path + "  ("
						+ Wui::Tr("modal.launcher.invalid", "unavailable") + ")";
					Wui::Label(ctx, { row.X + 2.0f, row.Y + 6.0f },
						Wui::EllipsizeMiddleToWidth(ctx, text, deleteRect.X - row.X - 10.0f, 12.0f),
						m_Theme.Danger, 12.0f);
				}
				// PROJ-5R/T1:"删除…"= 一步确认模态(完整路径 + 永久删除警告 + 确认/取消)。
				// 失效行同样可点开 —— 目录已不存在时模态换成"从列表移除"(只动最近列表);
				// 全部安全守卫仍在 API 里(危险目标会被逐条拒绝并给出可读理由)。
				if (Wui::ButtonEx(ctx,
					Wui::HashId(("project.launcher.recent.delete." + std::to_string(i)).c_str()),
					deleteRect, Wui::Tr("modal.launcher.delete", "Delete…"), DangerButtonTheme(m_Theme),
					true, false,
					Wui::Tr("modal.launcher.delete_hint",
						"Permanently delete this project folder (it will NOT go to the Recycle Bin)")))
				{
					deletePendingPath = path;
					deletePendingName = name;
				}
			}
			Wui::EndScrollArea(ctx);
		}

		// 打开/移除都在遍历之后执行(不能在持有 m_RecentProjects 引用时改表)。
		if (!openPendingPath.empty())
		{
			ctx.RecordOp("project", "open", openPendingName, openPendingPath);
			// PROJ-3/T1:启动器模式下没有"当前项目"可停留 —— 保留启动器页,
			// 子进程起来后本进程就退出(RelaunchWithProject 内部负责收尾)。
			if (!m_LauncherMode)
			{
				m_Editor.DismissProjectLauncher();
				ctx.ClearModal();
			}
			m_Editor.RelaunchWithProject(std::filesystem::u8path(openPendingPath));
			Wui::EndModalFrame(ctx);
			return;
		}
		if (!deletePendingPath.empty())
		{
			// PROJ-5R/T1:打开一步确认模态。这里**不**做任何删除 —— 真正的 remove_all 只在
			// "永久删除"按钮被点时发生,且守卫全在 ProjectLauncher 里。
			m_LauncherDeleteOpen = true;
			m_LauncherDeletePath = deletePendingPath;
			m_LauncherDeleteName = deletePendingName;
			m_LauncherDeleteError.clear();
			ctx.RecordOp("project", "delete-ask", deletePendingName, deletePendingPath);
		}

		// 动作按钮上方的说明(点项目 = 重启编辑器;"新建项目…"向导里可选空模板或示例模板)。
		Wui::Label(ctx, { frame.X + pad, frame.Y + frame.H - 76.0f },
			Wui::Tr("modal.launcher.hint",
				"Picking a project restarts the editor into it. New Project lets you choose a template "
				"(blank, or the example with scenes, materials and scripts); New Plugin creates an "
				"engine plugin package from one of the built-in plugin templates."),
			m_Theme.TextMuted, 12.0f);

		// PROJ-9:动作顺序 = 新建项目… / 打开项目… / 退出(编辑器形态=关闭)。
		// 单独的"新建示例项目…"已移除 —— 新建项目向导里本来就能选示例模板(templates/project-example/**),
		// 少一个入口也少一份维护面(用户 2026-09-29 口径)。
		// PLUG-AUTH-1:启动器形态没有菜单栏,所以「新建插件…」在这里也放一个入口(D1=允许
		// 无项目时新建引擎插件;Project 分段会在向导里禁用并给理由)。
		const Wui::ModalButtonDesc buttons[4] = {
			{ Wui::Tr("modal.launcher.new", "New Project…"), Wui::HashId("project.launcher.new"), true },
			{ Wui::Tr("modal.launcher.new_plugin", "New Plugin…"),
				Wui::HashId("project.launcher.new_plugin"), true },
			{ Wui::Tr("modal.launcher.open", "Open Project…"), Wui::HashId("project.launcher.open"), true },
			// PROJ-3/T1:启动器模式下第 4 个动作是"退出"(关掉整个启动器进程);
			// 普通编辑器形态下仍是 PROJ-2 的"关闭"(停在当前项目)。a11y id 两个形态共用。
			{ m_LauncherMode ? Wui::Tr("modal.launcher.quit", "Quit")
				: Wui::Tr("modal.launcher.close", "Close"),
				Wui::HashId("project.launcher.close"), true },
		};
		// 按钮照常绘制/登记(不能因为吞输入那一帧就少画),只丢掉这一帧的动作。
		const int clickedRaw = Wui::ModalButtons(ctx, frame, buttons, 4, m_Theme);
		const int clicked = justBecameVisible ? -1 : clickedRaw;
		const bool launcherEscape = justBecameVisible ? false : escapePressed;
		Wui::EndModalFrame(ctx);
		if (clicked == 0)
		{
			OpenNewProjectModal(ctx);   // 向导接管模态(启动器保持"待显示",向导收口后回来)
		}
		else if (clicked == 1)
		{
			OpenNewPluginModal(ctx);   // 启动器形态:目标固定引擎插件(向导里 Project 分段禁用)
		}
		else if (clicked == 2)
		{
			RequestOpenProjectBrowse(ctx);
		}
		else if (clicked == 3 || launcherEscape)
		{
			if (m_LauncherMode)
			{
				// "关窗 ⇒ 直接退出(不停留在空项目上)":日志 + 关闭进程都收在 EditorLayer。
				m_Editor.RequestLauncherExit();
			}
			else
			{
				m_Editor.DismissProjectLauncher();
				ctx.ClearModal();
				PushNotice(Wui::Tr("notice.project.launcher_closed",
					"Launcher closed — staying in the current project. Use File ▶ Open Project… to switch later."));
			}
		}
	}


	// PROJ-5R/T1(v2,用户二次指令):启动器"永久删除项目"的确认模态 —— 不再要求逐字输入目录名,
	// 只有"确认/取消"一步。同一个模态按**目标当前状态**(ClassifyLauncherDeleteTarget)分三种:
	//   ① 目录在 + 含项目清单 ⇒ 完整路径 + "永久删除、不进回收站、不可恢复"警告 +
	//      `永久删除`(危险色)+ `取消`(Esc = 取消);
	//   ② 目录已不存在 ⇒ 换成"该目录已不存在",主按钮变 `从列表移除` —— 只调 RemoveRecent
	//      (只动 local/projects.json,绝不碰磁盘);
	//   ③ 目录在但不是项目(缺 project.we.yaml)⇒ 拒绝打开/删除 + 可读理由,主按钮同样是
	//      `从列表移除`(只调 RemoveRecent),磁盘一动不动。
	// 用户可见的删除只发生在形态①的 `永久删除` 被点的那一帧,并且全部安全守卫都在
	// ProjectLauncher::DeleteProjectPermanently(先全查、再做唯一的写操作 remove_all);
	// 失败/被拒时列表项**不动**,原因就地显示 + 状态栏通知。
void EditorShell::DrawLauncherDeleteFlow(Wui::WuiContext& ctx, const Wui::WuiRect& frame, bool escapePressed){
		const float pad = 16.0f;
		const float contentWidth = frame.W - pad * 2.0f;
		const std::string path = m_LauncherDeletePath;
		const LauncherDeleteVariant variant = ClassifyLauncherDeleteTarget(path);
		const auto resetFlow = [this]
		{
			m_LauncherDeleteOpen = false;
			m_LauncherDeletePath.clear();
			m_LauncherDeleteName.clear();
			m_LauncherDeleteError.clear();
		};

		float cursorY = frame.Y + 54.0f;
		// ---- 目标是哪个目录:显示完整路径(读屏/脚本读的节点 Value 是未缩略的原串)----
		const std::string pathLabel = Wui::Tr("modal.project_delete.path", "Folder to delete");
		Wui::Label(ctx, { frame.X + pad, cursorY }, pathLabel, m_Theme.TextMuted, 12.0f);
		cursorY += 18.0f;
		const std::string shownPath = ctx.MeasureTextWidth(path, 13.0f) <= contentWidth
			? path : Wui::EllipsizeMiddleToWidth(ctx, path, contentWidth, 13.0f);
		Wui::Label(ctx, { frame.X + pad, cursorY }, shownPath, m_Theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("modal.project_delete.path");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = pathLabel;
			node.Value = path;             // 完整路径:绘制文本超宽才缩略,节点里永远是原串
			node.Tooltip = path;
			node.Rect = { frame.X + pad, cursorY - 2.0f, contentWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 30.0f;

		// ---- 形态说明:三种形态互斥,各自的 a11y 节点 id 不同(脚本按 id 断言)----
		{
			Wui::WuiId bodyNodeId = Wui::HashId("modal.project_delete.reject");
			std::string bodyLabel = Wui::Tr("modal.project_delete.reject.title", "Cannot delete this folder");
			std::string bodyText = Wui::Tr("modal.project_delete.error.no_manifest",
				"Not a WorldEngine project: project.we.yaml is missing");
			std::string bodyHint;                 // 形态③第二行:可以只把记录从最近列表移除
			Wui::WuiColor bodyColor = m_Theme.Danger;
			if (variant == LauncherDeleteVariant::Delete)
			{
				bodyNodeId = Wui::HashId("modal.project_delete.warning");
				bodyLabel = Wui::Tr("modal.project_delete.warning.label", "Warning");
				bodyText = Wui::Tr("modal.project_delete.warning",
					"Everything inside this folder is deleted permanently — no Recycle Bin, no recovery.");
			}
			else if (variant == LauncherDeleteVariant::Missing)
			{
				bodyNodeId = Wui::HashId("modal.project_delete.missing");
				bodyLabel = Wui::Tr("modal.project_delete.missing.title", "This folder no longer exists");
				bodyText = Wui::Tr("modal.project_delete.missing",
					"This folder no longer exists — it cannot be deleted. You can remove this entry "
					"from the recent list.");
				bodyColor = m_Theme.Text;
			}
			else if (variant == LauncherDeleteVariant::Reject)
			{
				// 目录在、但没有 project.we.yaml:打不开也删不掉;出口是"只把这条记录从最近
				// 列表移除" —— 与形态②同一条 RemoveRecent 路径,磁盘一动不动。
				bodyHint = Wui::Tr("modal.project_delete.reject.remove_hint",
					"It cannot be opened or deleted. You can still remove this entry from the "
					"recent list — nothing on disk is touched.");
			}
			Wui::Label(ctx, { frame.X + pad, cursorY }, bodyText, bodyColor, 12.5f);
			Wui::WuiAccessNode node;
			node.Id = bodyNodeId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = "shell";
			node.Kind = "text";
			node.Label = bodyLabel;
			// 形态③:两行都给到同一个节点,读屏/脚本一次拿到完整说明。
			node.Value = bodyHint.empty() ? bodyText : bodyText + " " + bodyHint;
			node.Rect = { frame.X + pad, cursorY - 2.0f, contentWidth, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			cursorY += 20.0f;
			if (!bodyHint.empty())
			{
				Wui::Label(ctx, { frame.X + pad, cursorY }, bodyHint, m_Theme.TextMuted, 12.0f);
				cursorY += 20.0f;
			}
		}
		cursorY += 14.0f;

		// 失败/被拒的原因就地显示(上一帧 API 的返回文本);状态栏通知里也有一份。
		if (!m_LauncherDeleteError.empty())
		{
			Wui::Label(ctx, { frame.X + pad, cursorY }, m_LauncherDeleteError, m_Theme.Danger, 12.0f);
			cursorY += 20.0f;
		}

		// 底部按钮与 ModalFooter 同一内边距/高度;这里手排是因为"永久删除"要用危险色主题
		// (ModalButtons 只吃一个主题),按钮宽度沿用 ModalFooter 的测量口径。
		const float buttonHeight = Wui::ModalFooterHeight;
		const float buttonY = frame.Y + frame.H - Wui::ModalFooterPadding - buttonHeight;
		const std::string cancelLabel = Wui::Tr("modal.project_delete.cancel", "Cancel");
		const std::string cancelTooltip = Wui::Tr("modal.project_delete.cancel.tooltip",
			"Cancel and go back to the project list (Esc)");
		const float cancelWidth = std::min(160.0f,
			std::max(90.0f, ctx.MeasureTextWidth(cancelLabel, 15.0f) + 30.0f));

		// 主按钮:形态① = `永久删除`(危险色);形态②/③ = `从列表移除`(安全动作,只动最近列表)。
		std::string primaryLabel;
		if (variant == LauncherDeleteVariant::Delete)
			primaryLabel = Wui::Tr("modal.project_delete.confirm", "Delete permanently");
		else
			primaryLabel = Wui::Tr("modal.project_delete.remove_from_list", "Remove from list");
		const float primaryWidth = primaryLabel.empty() ? 0.0f : std::min(200.0f,
			std::max(110.0f, ctx.MeasureTextWidth(primaryLabel, 15.0f) + 30.0f));
		const float rowX = frame.X + frame.W - pad
			- (primaryWidth > 0.0f ? primaryWidth + 8.0f : 0.0f) - cancelWidth;
		const Wui::WuiRect cancelRect { rowX + (primaryWidth > 0.0f ? primaryWidth + 8.0f : 0.0f),
			buttonY, cancelWidth, buttonHeight };

		bool primaryClicked = false;
		if (variant == LauncherDeleteVariant::Delete)
		{
			primaryClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.confirm"),
				{ rowX, buttonY, primaryWidth, buttonHeight }, primaryLabel, DangerButtonTheme(m_Theme),
				true, true, std::string());
		}
		else
		{
			// 失效/非项目条目补偿(形态②目录已不存在;形态③目录在但不是项目):
			// 只动最近列表 —— tooltip 明确"磁盘上什么都不动"。
			primaryClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.confirm"),
				{ rowX, buttonY, primaryWidth, buttonHeight }, primaryLabel, m_Theme, true, false,
				Wui::Tr("modal.project_delete.remove_from_list.tooltip",
					"Remove this entry from the recent list only — nothing on disk is touched"));
		}
		const bool cancelClicked = Wui::ButtonEx(ctx, Wui::HashId("project.project_delete.cancel"),
			cancelRect, cancelLabel, m_Theme, true, false, cancelTooltip);

		if (variant == LauncherDeleteVariant::Delete && primaryClicked)
		{
			// 唯一写操作:全部守卫 + remove_all 都在 API 里;返回空串 = 成功。
			const std::string reason = Editor::ProjectLauncher::DeleteProjectPermanently(
				std::filesystem::u8path(path));
			if (reason.empty())
			{
				ctx.RecordOp("project", "delete", m_LauncherDeleteName, path);
				RefreshRecentProjectsIfStale(/*force=*/true);   // 成功后条目已从最近列表移除
				PushNotice(Wui::TrFormat("notice.project_delete.done", "Permanently deleted: {path}",
					{ { "path", path } }));
				resetFlow();
			}
			else
			{
				// 失败/被拒 ⇒ 列表项不动(避免"看着删了其实还在");模态留在确认态可重试或取消。
				ctx.RecordOp("project", "delete-failed", m_LauncherDeleteName, reason);
				WLD_CORE_WARN("[project] permanent delete refused or failed: '{0}': {1}", path, reason);
				m_LauncherDeleteError = reason;
				PushNotice(Wui::TrFormat("notice.project_delete.failed", "Permanent delete failed: {reason}",
					{ { "reason", reason } }));
			}
		}
		else if (variant != LauncherDeleteVariant::Delete && primaryClicked)
		{
			// 失效/非项目条目补偿:只调 RemoveRecent(local/projects.json),磁盘一动不动。
			std::string error;
			if (Editor::ProjectLauncher::RemoveRecent(std::filesystem::u8path(path), &error))
			{
				ctx.RecordOp("project", "recent-remove", m_LauncherDeleteName, path);
				RefreshRecentProjectsIfStale(/*force=*/true);
				PushNotice(Wui::TrFormat("notice.project.recent_removed",
					"Removed from the recent list: {name} (the project files on disk were not touched)",
					{ { "name", m_LauncherDeleteName } }));
				resetFlow();
			}
			else
			{
				ctx.RecordOp("project", "recent-remove-failed", m_LauncherDeleteName, error);
				PushNotice(Wui::TrFormat("notice.project.recent_remove_failed",
					"Could not update the recent projects list: {reason}", { { "reason", error } }));
				m_LauncherDeleteError = error;
			}
		}
		else if (cancelClicked || escapePressed)
		{
			ctx.RecordOp("project", "delete-cancel", m_LauncherDeleteName, path);
			resetFlow();
		}
	}


void EditorShell::EnsureModelPanelFromId(const std::string& panelId){
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kModelPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ModelPreviewPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

}
