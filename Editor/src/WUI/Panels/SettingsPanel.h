#pragma once

#include "EditorPanel.h"
#include "../SettingsUi.h"
#include "World/Core/Asset/ProjectManifest.h"

#include <string>
#include <vector>

namespace World
{
	// P1b D8a2:项目渲染设置面板 —— 把"过去只藏在代码常量/环境变量里"的开关
	// 开放给引擎用户(做项目的人),而不是每次都要改引擎代码或设环境变量。
	//
	// 交互口径(P4-UX1 用户反馈"改完还要点保存太繁琐"):
	//  - 控件改动 → **立即生效**(RenderSettings::Set)+ **自动落盘**(防抖 400ms);
	//  - 没有"保存"按钮;面板只显示"已自动保存/保存失败"状态,"恢复默认"仍然保留;
	//  - 阴影贴图尺寸是**资源创建期**参数 → 面板明确提示"下次启动生效"。
	class SettingsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "settings"; }
		const char* Title() const override { return "Project Settings"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// 控件值从 RenderSettings::Get() 初始化一次,之后由控件驱动
		// (每帧回读会把用户正在拖动的值覆盖掉)。
		bool m_Initialized = false;
		// P4-UX12:整个面板由设置注册表渲染(描述符在 RegisterProjectSettings 里注册一次)。
		bool m_Registered = false;
		// P4-U4(2026-09-21):另外两个作用域也进同一个面板(页签切换)。
		//   tab 0 = Project(渲染/物理/启动/发行包),1 = Import Defaults,2 = World(场景级)。
		int m_Tab = 0;
		bool m_ImportRegistered = false;
		bool m_WorldRegistered = false;
		Editor::SettingsPageState m_ImportPage;
		Editor::SettingsPageState m_WorldPage;
		// 导入默认值的暂存副本(与渲染/物理同一套"改完防抖落盘"节奏)。
		Asset::ModelImportSettings m_Import;
		bool m_ImportDirty = false;
		PanelHost* m_Host = nullptr;
		Editor::SettingsPageState m_Page;
		Asset::RenderingSettings m_Edit;
		// P4-1:物理设置(与渲染设置同一个面板)。
		Asset::PhysicsSettingsData m_Physics;
		bool m_PhysicsChanged = false;
		// P4-UX11:启动与内容(renderer / start_scene / content_root)—— 来自 project.we.yaml,
		// 以前只能手改文件;这三项各自生效时机不同(见面板 tooltip 与重启入口)。
		struct StartupSettings
		{
			std::string Renderer = "opengl";
			std::string StartScene;
			std::string ContentRoot;
		};
		StartupSettings m_Startup;
		std::vector<std::string> m_SceneOptions;
		// 发行包列表(project.we.yaml 的 packages:):列表编辑不走注册表(它是"一行一个值"的模型)。
		std::vector<std::string> m_Packages;
		std::string m_StartupContentRootBuffer;
		std::string m_Status = "渲染设置来自 project.we.yaml 的 `rendering` 区块";
		bool m_StatusIsError = false;
		// 自动保存防抖:拖拽控件时每帧都会 changed,不能每帧写盘。
		bool m_PendingSave = false;
		double m_LastChangeSeconds = 0.0;
		void RegisterProjectSettings();
		// P4-U4:project.we.yaml 的 `imports:`(资产导入默认值)。
		void RegisterImportSettings();
		// P4-U4:.wd 场景头的 `World:` 块(场景级设置;绑当前文档场景)。
		void RegisterWorldSettings();
	};
}
