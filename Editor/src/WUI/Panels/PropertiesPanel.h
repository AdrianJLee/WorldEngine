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
		explicit PropertiesPanel(PanelHost& host);
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

		// ---- U6:Add Component 选择器(方案 §8.2;候选/分类/说明全部来自 schema)----
		// 打开:清空上一次状态并聚焦搜索框;绘制:搜索框 + 分组列表(最近使用 → 分类 → 未分类)。
		void OpenAddComponentPicker(Wui::WuiContext& ctx);
		void DrawAddComponentPicker(Wui::WuiContext& ctx, const Wui::WuiRect& addButton, Entity entity,
			Scene* scene, Schema::SchemaRegistry& schemas);
		// MRU 更新(置顶,最多 5 条)并落盘;<Editor>/wui-properties.json 写失败只告警,不影响编辑。
		void TouchRecent(const std::string& shortName);
		void LoadState();
		void SaveState() const;

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
		// ---- U6:选择器状态(只属于本面板对象,不进任何全局表)----
		std::string m_StatePath;                     // <Editor>/wui-properties.json
		std::vector<std::string> m_RecentComponents; // 最近使用(短类型名,MRU 顺序)
		std::string m_AddSearch;                     // 搜索框缓冲(TextField 直接写入)
		std::string m_AddSearchLast;                 // 上一次的搜索词:变化时把高亮归零
		int m_AddHighlight = -1;                     // 键盘高亮(候选项序号;-1 = 无 → Enter 取第一个)
		bool m_AddListFocus = false;                 // Tab 是否停在列表侧
		float m_AddScroll = 0.0f;                    // 列表内部滚动偏移
		std::string m_RevealSection;                 // 添加后要展开并滚到可见的分区(DisplayName)
		int m_RevealFrames = 0;                      // 剩余强制滚动帧数(分区高度是上一帧实测值)
		uint64_t m_AddOpenedFrame = 0;               // 打开的那一帧(同帧的按键不当作选择器输入)
	};
}
