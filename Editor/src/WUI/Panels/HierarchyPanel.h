#pragma once

#include "EditorPanel.h"
#include "World/WUI/WuiWidget.h"
#include "World/Gameplay/Prefab.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace World
{
	// 场景层级面板:实体列表、选择与上下文菜单。菜单目标跨帧保留在模型里。
	class HierarchyPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "hierarchy"; }
		const char* Title() const override { return "Scene Hierarchy"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiScrollArea> m_Scroll;
		std::vector<std::shared_ptr<Wui::WuiListRow>> m_Rows;
		std::vector<Entity> m_RowEntities;
		// 折叠状态按实体句柄保存(跨帧保持;实体销毁后残留项无害)。
		std::unordered_set<uint32_t> m_Collapsed;
		std::string m_LastOrderKey;
		Entity m_Context;
		// 拖拽设父的待提交落点(拖拽结束后统一提交)。
		Entity m_PendingDropHandle;
		uint32_t m_PendingDropSource = 0;
		// 0=插到目标之前 1=成为目标子节点 2=插到目标之后
		uint32_t m_PendingDropZone = 1;
		// W4-2b:从内容浏览器拖来的 .wprefab(相对内容根路径),松开时实例化。
		std::string m_PendingPrefabFile;
		// W4-3b:本场景里由拖拽实例化出来的 prefab 实例(实例根句柄 -> 记录)。
		// 记录里含来源路径与覆盖集合,供 Revert/Apply/Unpack 使用。
		std::unordered_map<uint32_t, Gameplay::PrefabInstanceRecord> m_PrefabInstances;
		glm::vec2 m_MenuPos {};
		glm::vec2 m_BlankMenuPos {};
	};
}
