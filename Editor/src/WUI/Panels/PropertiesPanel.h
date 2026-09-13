#pragma once

#include "EditorPanel.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidget.h"

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
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::vector<SectionEntry> m_Sections;
		std::vector<std::string> m_LastSchemaNames;
	};
}
