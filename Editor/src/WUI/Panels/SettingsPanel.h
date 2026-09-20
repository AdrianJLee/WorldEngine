#pragma once

#include "EditorPanel.h"
#include "World/Core/Asset/ProjectManifest.h"

#include <string>

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
		Asset::RenderingSettings m_Edit;
		// P4-1:物理设置(与渲染设置同一个面板)。
		Asset::PhysicsSettingsData m_Physics;
		bool m_PhysicsChanged = false;
		std::string m_Status = "渲染设置来自 project.we.yaml 的 `rendering` 区块";
		bool m_StatusIsError = false;
		// 自动保存防抖:拖拽控件时每帧都会 changed,不能每帧写盘。
		bool m_PendingSave = false;
		double m_LastChangeSeconds = 0.0;
	};
}
