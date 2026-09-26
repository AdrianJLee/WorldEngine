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
			const std::string& typeName, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
			std::vector<std::string>* changedFields = nullptr);
		float DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect);
		float DrawTransformInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, TransformComponent& transform,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
			std::vector<std::string>* changedFields = nullptr);
		float DrawCameraInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, void* instance,
			const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
			std::vector<std::string>* changedFields = nullptr);
		// 2026-09-26 脚本组件重写:统一脚本检视器(两个脚本组件共用一条路径)。
		// 引用行(Luau 资产下拉 + 打开脚本编辑器 / C++ 已注册脚本下拉)→ 状态行 → 属性表 → 动作行;
		// 编辑态直接读写 `Properties`(不实例化脚本),Play/Simulate 只读。
		float DrawScriptComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity,
			void* instance, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
			std::vector<std::string>* changedFields = nullptr);

		// ---- P4-U13b:prefab 实例条(选中实体属于某实例时画在组件列表最上方)----
		// 实例归属/来源/覆盖计数来自宿主(Scene 持有实例记录);来源资产找不到时实例条
		// 给出可读提示并进入只读:回滚/应用需要来源文件,断开链接仍可用(它是唯一出路)。
		struct InstanceBarInfo
		{
			bool InInstance = false;
			std::string Source;
			size_t Overrides = 0;
			bool SourceMissing = false;
			bool CanRevert = false;
			bool CanApply = false;
			Entity Root;
		};
		InstanceBarInfo ResolveInstanceBar(PanelHost& host, Entity entity);
		// 实例条布局:高度/按钮矩形先算出来,组件列表才知道要让出多少行(窄面板按钮换行)。
		struct InstanceBarLayout
		{
			float Height = 0.0f;
			Wui::WuiRect Buttons[3] {};
			Wui::WuiRect Hint {};
			bool HasHint = false;
		};
		InstanceBarLayout LayoutInstanceBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const InstanceBarInfo& info) const;
		void DrawInstanceBar(Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiRect& rect,
			const InstanceBarInfo& info, const InstanceBarLayout& layout);
		// Apply / Unpack 是破坏性动作:先弹确认模态(与"移除组件"同一套面板级模态)。
		enum class PrefabAction { None = 0, Apply, Unpack };
		PrefabAction m_PrefabActionPending = PrefabAction::None;
		Entity m_PrefabActionRoot;
		std::string m_PrefabActionSource;
		void OpenPrefabActionConfirm(Wui::WuiContext& ctx, PrefabAction action, Entity root,
			const std::string& source);
		void ClosePrefabActionConfirm(Wui::WuiContext& ctx);
		void DrawPrefabActionConfirm(Wui::WuiContext& ctx);
		// 本帧被编辑的字段 → 实例覆盖登记(由"改动发生处"登记,不靠全量 diff 反推)。
		void RegisterPrefabOverrides(Entity entity, const std::vector<std::string>& fields);

		// ---- U6:Add Component 选择器(方案 §8.2;候选/分类/说明全部来自 schema)----
		// 打开:清空上一次状态并聚焦搜索框;绘制:搜索框 + 分组列表(最近使用 → 分类 → 未分类)。
		void OpenAddComponentPicker(Wui::WuiContext& ctx);
		void DrawAddComponentPicker(Wui::WuiContext& ctx, Entity entity,
			Scene* scene, Schema::SchemaRegistry& schemas);
		// 关闭居中模态(清模态态 + 解除面板级输入封锁)。
		void CloseAddComponentPicker(Wui::WuiContext& ctx);
		// ---- P4-U9:移除组件(核心组件禁止移除;破坏性操作先确认)----
		// 点击分区标题右侧的 ✕ → 打开确认模态;确认后走与场景结构改动同一条路径。
		void DrawRemoveComponentConfirm(Wui::WuiContext& ctx, Entity entity);
		void OpenRemoveComponentConfirm(Wui::WuiContext& ctx, uint32_t componentId, const std::string& displayName);
		void CloseRemoveComponentConfirm(Wui::WuiContext& ctx);
		uint32_t m_RemovePendingId = 0;      // 待确认移除的组件 id(0 = 没有)
		std::string m_RemovePendingName;     // 组件显示名(模态标题/操作记录用)
		// MRU 更新(置顶,最多 5 条)并落盘;<local>/wui-properties.json 写失败只告警,不影响编辑。
		void TouchRecent(const std::string& shortName);
		void LoadState();
		void SaveState() const;

		PanelHost& m_Host;
		// Play/Simulate 期间为 true:字段只显示不落值(只读查看)。
		bool m_ReadOnly = false;
		// U6b:"添加组件"是**居中模态**(用户 2026-09-21:「为什么不弹出个居中窗口呢」)——
		// 打开期间由宿主封锁整窗输入,面板自身在选中行后回车/点 Add 落地。
		bool m_AddOpen = false;
		// 左侧分类栏的当前过滤(空 = 全部);候选集里出现过的 CategoryPath。
		// 语义:m_AddCategoryAll = 侧栏选中"全部";否则 m_AddCategoryFilter 为空表示"未分类",
		// 非空表示该分类路径。
		bool m_AddCategoryAll = true;
		std::string m_AddCategoryFilter;
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
		std::string m_StatePath;                     // <local>/wui-properties.json
		std::vector<std::string> m_RecentComponents; // 最近使用(短类型名,MRU 顺序)
		std::string m_AddSearch;                     // 搜索框缓冲(TextField 直接写入)
		std::string m_AddSearchLast;                 // 上一次的搜索词:变化时把高亮归零
		int m_AddHighlight = -1;                     // 键盘高亮(候选项序号;-1 = 无 → Enter 取第一个)
		bool m_AddListFocus = false;                 // Tab 是否停在列表侧
		float m_AddScroll = 0.0f;                    // 列表内部滚动偏移
		// 右侧滚动条的滑块拖动状态(与主滚动区同一套直接定位换算)。
		bool m_AddThumbDragging = false;
		float m_AddThumbGrabOffset = 0.0f;
		std::string m_RevealSection;                 // 添加后要展开并滚到可见的分区(DisplayName)
		int m_RevealFrames = 0;                      // 剩余强制滚动帧数(分区高度是上一帧实测值)
		uint64_t m_AddOpenedFrame = 0;               // 打开的那一帧(同帧的按键不当作选择器输入)
	};
}
