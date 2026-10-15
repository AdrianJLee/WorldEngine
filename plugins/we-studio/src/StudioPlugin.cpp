#include "World/Plugins/WePluginApi.h"
#include "StudioPolicy.h"
#include "BacklogManager.h"
#include "StudioValidator.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"

#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;
	using namespace World::Studio;

	constexpr const char* kPluginId = "engine.we-studio";
	const char* const kProvides[] = { "workflow.policy", "workflow.backlog", "editor.ui" };

	WeHostApi& Host()
	{
		static WeHostApi s_Host;
		return s_Host;
	}

	int g_StudioActiveTab = 0;
	char g_NewTaskBuffer[256] = "";
	char g_NewBugBuffer[256] = "";
	std::string g_ValidationReport = "点击下方按钮运行全量门禁自检。";
	int g_ValidationTone = 0;

	int DrawStudioPanel(void* /*userData*/, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator)
			return 1;

		// 1. 顶部 Tab 导航
		static const char* kTabs[] = { "📋 任务看板", "🐛 缺陷跟踪", "🛡️ 门禁自检" };
		if (ui->TabBar)
		{
			ui->TabBar(uiContext, "studio_tabs", kTabs, 3, &g_StudioActiveTab);
		}
		else
		{
			ui->Label(uiContext, "=== WorldEngine Studio Workflow ===");
		}
		ui->Separator(uiContext);

		// ==================== Tab 0: 任务看板 ====================
		if (g_StudioActiveTab == 0)
		{
			auto& bm = BacklogManager::Get();
			auto tasks = bm.ListTasks("");

			std::string countStr = "当前待办/执行中任务数量: " + std::to_string(tasks.size());
			if (ui->LabelColored)
				ui->LabelColored(uiContext, countStr.c_str(), 4 /*Accent*/);
			else
				ui->Label(uiContext, countStr.c_str());

			// 显示前 5 条任务
			size_t maxShow = std::min<size_t>(tasks.size(), 5);
			for (size_t i = 0; i < maxShow; ++i)
			{
				const auto& t = tasks[i];
				std::string tLine = "[" + t.status + "] " + t.id + ": " + t.title + " (" + t.role + ")";
				int tone = (t.status == "done" ? 1 /*Success*/ : (t.status == "in_progress" ? 4 /*Accent*/ : 0));
				if (ui->LabelColored)
					ui->LabelColored(uiContext, tLine.c_str(), tone);
				else
					ui->Label(uiContext, tLine.c_str());

				if (t.status == "open")
				{
					std::string btnId = "btn_start_" + t.id;
					if (ui->Button(uiContext, btnId.c_str(), "▶ 开始进行 (In Progress)"))
					{
						bm.UpdateTask("", t.id, "in_progress", "Started via Editor UI");
					}
				}
				else if (t.status == "in_progress")
				{
					std::string btnId = "btn_done_" + t.id;
					if (ui->Button(uiContext, btnId.c_str(), "✔ 完成交付 (Done)"))
					{
						bm.UpdateTask("", t.id, "done", "Completed via Editor UI");
					}
				}
			}

			ui->Separator(uiContext);
			ui->Label(uiContext, "--- 创建新研发任务 ---");
			if (ui->TextInput)
			{
				ui->TextInput(uiContext, "input_task_title", g_NewTaskBuffer, sizeof(g_NewTaskBuffer));
			}

			if (ui->Button(uiContext, "btn_create_task", "➕ 创建任务 (Create Task)"))
			{
				std::string title(g_NewTaskBuffer);
				if (!title.empty())
				{
					StudioTask newTask;
					newTask.title = title;
					newTask.role = "gameplay_programmer";
					newTask.stage = "0";
					newTask.status = "open";
					bm.CreateTask("", newTask);
					g_NewTaskBuffer[0] = 0;
				}
			}
		}
		// ==================== Tab 1: 缺陷跟踪 ====================
		else if (g_StudioActiveTab == 1)
		{
			auto& bm = BacklogManager::Get();
			auto bugs = bm.ListBugs("");

			std::string bugCount = "未闭环 Bug 总数: " + std::to_string(bugs.size());
			if (ui->LabelColored)
				ui->LabelColored(uiContext, bugCount.c_str(), bugs.empty() ? 1 /*Success*/ : 3 /*Danger*/);
			else
				ui->Label(uiContext, bugCount.c_str());

			size_t maxShow = std::min<size_t>(bugs.size(), 5);
			for (size_t i = 0; i < maxShow; ++i)
			{
				const auto& b = bugs[i];
				std::string bLine = "[" + b.severity + "] " + b.id + ": " + b.title + " -> " + b.status;
				int tone = (b.severity == "blocker" ? 3 /*Danger*/ : 2 /*Warning*/);
				if (ui->LabelColored)
					ui->LabelColored(uiContext, bLine.c_str(), tone);
				else
					ui->Label(uiContext, bLine.c_str());

				if (b.status != "closed")
				{
					std::string btnId = "btn_close_bug_" + b.id;
					if (ui->Button(uiContext, btnId.c_str(), "✔ 关闭/修复该缺陷"))
					{
						bm.UpdateBug("", b.id, "closed", "Resolved via Editor UI");
					}
				}
			}

			ui->Separator(uiContext);
			ui->Label(uiContext, "--- 快速登记缺陷 ---");
			if (ui->TextInput)
			{
				ui->TextInput(uiContext, "input_bug_title", g_NewBugBuffer, sizeof(g_NewBugBuffer));
			}

			if (ui->Button(uiContext, "btn_report_blocker", "🚨 登记 Blocker 阻断缺陷"))
			{
				std::string title(g_NewBugBuffer);
				if (!title.empty())
				{
					StudioBug bug;
					bug.title = title;
					bug.severity = "blocker";
					bug.status = "open";
					bug.role = "qa";
					bm.CreateBug("", bug);
					g_NewBugBuffer[0] = 0;
				}
			}
			if (ui->Button(uiContext, "btn_report_major", "⚠️ 登记 Major 普通缺陷"))
			{
				std::string title(g_NewBugBuffer);
				if (!title.empty())
				{
					StudioBug bug;
					bug.title = title;
					bug.severity = "major";
					bug.status = "open";
					bug.role = "qa";
					bm.CreateBug("", bug);
					g_NewBugBuffer[0] = 0;
				}
			}
		}
		// ==================== Tab 2: 门禁自检 ====================
		else if (g_StudioActiveTab == 2)
		{
			auto stage = World::Agent::WeAgentCore::Get().GetCurrentStage();
			std::string stageInfo = "流水线阶段: " + std::string(World::Agent::WorkflowStageToString(stage))
				+ " (Stage " + std::to_string(static_cast<int>(stage)) + ") | 预设: default (40 Roles)";
			if (ui->LabelColored)
				ui->LabelColored(uiContext, stageInfo.c_str(), 4 /*Accent*/);
			else
				ui->Label(uiContext, stageInfo.c_str());

			ui->Separator(uiContext);
			ui->Label(uiContext, "【质量门禁检查矩阵】");
			ui->Label(uiContext, "1. 阶段工件完备性 (GDD / Tech Spec / Assets)");
			ui->Label(uiContext, "2. 阻断缺陷清零 (Zero Blocker Bugs)");
			ui->Label(uiContext, "3. 写入守卫审计 (WriteGuard Sandbox)");
			ui->Label(uiContext, "4. 引擎源码指纹保护 (Engine Tree Integrity)");
			ui->Separator(uiContext);

			if (ui->Button(uiContext, "btn_run_validate", "🛡️ 立即运行全量门禁校验 (studio.validate)"))
			{
				std::string res, err;
				auto& reg = World::Ai::CommandRegistry::Get();
				if (reg.Dispatch("studio.validate", {}, res, err))
				{
					g_ValidationReport = "✅ 门禁通过！项目工件完备，0 个 Blocker 缺陷，无越权改动。允许推进到下一研发阶段。";
					g_ValidationTone = 1; // Success
				}
				else
				{
					g_ValidationReport = "❌ 门禁阻断: " + err;
					g_ValidationTone = 3; // Danger
				}
			}

			if (ui->LabelColored)
				ui->LabelColored(uiContext, g_ValidationReport.c_str(), g_ValidationTone);
			else
				ui->Label(uiContext, g_ValidationReport.c_str());
		}

		return 0;
	}

	bool Register(World::WorldContext& /*context*/, const WeHostApi& host)
	{
		Host() = host;

		// 1. 注册工作流策略到引擎编排内核
		auto policy = std::make_shared<StudioPolicy>("default");
		World::Agent::WeAgentCore::Get().SetPolicy(policy);

		// 2. 注册命令到 CommandRegistry
		auto& reg = World::Ai::CommandRegistry::Get();
		BacklogManager::RegisterCommands(reg);
		StudioValidator::RegisterCommands(reg);

		// 3. 注册编辑器面板
		if (host.AbiVersion == WE_PLUGIN_ABI_VERSION
			&& host.StructSize >= offsetof(WeHostApi, UnregisterEditorPanel) + sizeof(host.UnregisterEditorPanel)
			&& host.RegisterEditorPanel)
		{
			WeEditorPanelDesc panel;
			panel.Id = "studio_workflow";
			panel.Title = "Studio Workflow (工作流看板)";
			panel.Draw = &DrawStudioPanel;
			host.RegisterEditorPanel(host.UserData, &panel);
		}

		return true;
	}

	void Unregister(World::WorldContext& /*context*/)
	{
		const WeHostApi& host = Host();
		if (host.UserData && host.UnregisterEditorPanel)
		{
			host.UnregisterEditorPanel(host.UserData, "studio_workflow");
		}

		// 复位为最小后备策略
		World::Agent::WeAgentCore::Get().ResetToMinimalPolicy();
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = "WorldEngine Studio";
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 3;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = MakePlugin();
	return &plugin;
}
