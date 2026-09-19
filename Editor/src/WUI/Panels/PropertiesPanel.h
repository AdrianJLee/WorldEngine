#pragma once

#include "EditorPanel.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// 属性检查器:按反射 schema 渲染选中实体的组件字段。编辑状态由 Persist 持有。
	class PropertiesPanel final : public EditorPanel
	{
	public:
		explicit PropertiesPanel(PanelHost& host) : m_Host(host) {}
		const char* Id() const override { return "properties"; }
		const char* Title() const override { return "Properties"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// 一个组件分区的手工滚动布局状态(标题/展开态/内容高度)。
		struct SectionEntry
		{
			std::string Title;
			bool Open = false;
			float ContentHeight = 0;
		};

		// 面板级滚动状态:按实体句柄区分,切换选择后回到顶部。
		struct ScrollState
		{
			uint32_t Handle = ~0u;
			float Offset = 0;
		};

		float DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect, void* instance,
			const std::string& typeName, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect);
		float DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect);
		float DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, TransformComponent& transform,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect);
		float DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect);

		PanelHost& m_Host;
		// Play/Simulate 期间为 true:字段只显示不落值(只读查看)。
		bool m_ReadOnly = false;
		// 分区滚动布局(手工绘制/裁剪;滚动偏移按实体句柄持久化在 WuiContext::Persist)。
		std::vector<SectionEntry> m_Sections;
		std::vector<std::string> m_LastSchemaNames;
		// 滚动条拖拽状态(跨帧)。
		bool m_ScrollThumbDragging = false;
		float m_ScrollThumbGrabOffset = 0;
		// P2 W5b:Lua 脚本 Reload 按钮的最近一次结果(按实体句柄区分,切换选择后不再显示)。
		std::string m_LuaReloadMessage;
		uint32_t m_LuaReloadHandle = ~0u;
		bool m_LuaReloadOk = true;
	};
}
