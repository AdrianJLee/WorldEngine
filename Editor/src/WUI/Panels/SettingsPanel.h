#pragma once

#include "EditorPanel.h"
#include "World/Core/Asset/ProjectManifest.h"

#include <string>

namespace World
{
	// P1b D8a2:项目渲染设置面板 —— 把"过去只藏在代码常量/环境变量里"的开关
	// 开放给引擎用户(做项目的人),而不是每次都要改引擎代码或设环境变量。
	//
	// 交互口径:
	//  - 控件改动 → **立即生效**(RenderSettings::Set,不写盘),方便边看边调;
	//  - "保存到 project.we.yaml" → 由宿主写盘(下次启动与打包产物同样生效);
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
		std::string m_Status = "渲染设置来自 project.we.yaml 的 `rendering` 区块";
		bool m_StatusIsError = false;
	};
}
