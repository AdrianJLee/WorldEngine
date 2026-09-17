#pragma once

#include "EditorPanel.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidget.h"

#include <cstdint>
#include <memory>

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
		struct SectionEntry
		{
			std::string Name;
			std::shared_ptr<Wui::WuiSection> Section;
		};

		float DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect, void* instance,
			const std::string& typeName, const Schema::TypeSchema& schema);
		float DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema);

		PanelHost& m_Host;
		// Play/Simulate 期间为 true:字段只显示不落值(只读查看)。
		bool m_ReadOnly = false;
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::vector<SectionEntry> m_Sections;
		std::vector<std::string> m_LastSchemaNames;
		// P2 W5b:Lua 脚本 Reload 按钮的最近一次结果(按实体句柄区分,切换选择后不再显示)。
		std::string m_LuaReloadMessage;
		uint32_t m_LuaReloadHandle = ~0u;
		bool m_LuaReloadOk = true;
	};
}
