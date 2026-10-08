#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Gameplay/Framework/InputMap.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World
{
	// 输入映射面板。
	//
	// R4 起它不再只是"名单 + 重绑":可以**从零建出**一份可用映射 ——
	//   * 动作:新增(输入名字)/ 删除 / 重绑(替换主绑定)/ 追加绑定(键盘捕获、鼠标、手柄);
	//   * 轴:新增 / 删除 / 正负动作与手柄轴、死区可编辑;
	//   * 上下文:新增 / 删除 / 优先级、消费、default(装载即自动压栈)可编辑;
	//   * 触发器:在上下文映射上新增 / 删除 / 选类型 + 调时长与间隔。
	// 此前它只有"重绑 + 保存",而新建空项目是 `actions: []` —— 面板上是一片空白且无从下手
	// (文件头自己写着"有意不做的:动作增删",那个"有意"在真实工作流里站不住)。
	//
	// 仍然有意不做:动作改名(改名要连带修轴的正负引用与上下文映射,收益低风险高 ——
	// 删除后新建更清楚)、多玩家分栏、手柄轴的曲线编辑。
	class InputMapPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "input"; }
		const char* Title() const override { return "Input Map"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureLoaded();
		bool Save();
		float ContentHeight() const;
		void MarkDirtyAndSave(const std::string& what);

		Gameplay::InputMap m_Map;
		std::filesystem::path m_Path;
		bool m_Loaded = false;
		std::string m_Status;

		// 正在**替换**主绑定的动作(点名字触发的重绑,既有行为)。
		std::string m_RebindingAction;
		// 正在为哪个动作**追加**一条键绑定(R4:与上面的"替换"分开 —— 追加不该清掉已有绑定)。
		std::string m_AppendKeyAction;

		// 新增项的输入缓冲。
		std::string m_NewActionName;
		std::string m_NewAxisName;
		std::string m_NewContextName;

		float m_ScrollY = 0.0f;
	};
}
