#pragma once

#include "EditorPanel.h"
#include "World/Scene/Components.h"
#include "World/Script/ScriptProperties.h"
#include "World/WUI/WuiWidget.h"
#include "World/WUI/WuiWidgets.h"

#include <cstdint>
#include <map>
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

		// ---- VEC-C2:数组/映射行上下文(只有脚本属性行的容器会带)----
		//
		// `Container` = 容器自身的 `ScriptProperty`(元素行要能直接增删 `Children`);
		// `IdText` = 容器行的无障碍 id(`properties.<组件>.<属性>`),`.add` / `.remove.<下标|键>`
		// 由它派生;`Writable` = 编辑态(Play/只读态只画禁用占位,不画增删)。
		// **只对脚本合成路径生效**:`ScriptTableCollectionOf` 只在合成 arena 里查得到集合形态,
		// 普通 schema 的 Object 字段(Play 里 C++ 实例的嵌套结构)拿不到上下文,增删路径不误走。
		struct ScriptCollectionRows
		{
			ScriptProperty* Container = nullptr;
			ScriptPropertyCollection Kind = ScriptPropertyCollection::None;
			std::string IdText;
			bool Writable = false;
		};
		float DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect, void* instance,
			const std::string& typeName, const Schema::TypeSchema& schema, const Wui::WuiRect& visibleRect,
			std::vector<std::string>* changedFields = nullptr, bool scriptPropertyRow = false,
			ScriptCollectionRows* collectionRows = nullptr, int depth = 0);
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

		// ---- VEC-F2 / VEC-H6:两级复原(单项 / 整个集合)----
		//
		// 用户口径(VEC-F2):「对于数组或 Map 类型的属性,复原按钮会把整个都复原而非单个值」→ 拆成两级:
		//   * 单项 `↺`(叶子 / 数组元素 / 映射值行)= 只把**这一行**清成"未设",显示回落脚本默认值,
		//     同一集合里其它元素的行/值/形状都不动;
		//   * 集合头 `↺`(数组 / 映射 / 结构化表的折叠头行)= 复原**整个集合**到脚本声明的默认形状
		//     与默认值(丢弃所有增删改)。
		//
		// 用户口径(VEC-H6,2026-09-27):① 两级 `↺` 都只在"当前值/形状 != 脚本声明默认"时**出现**
		// (一致时不画 —— 不是禁用态);② 集合头复原**单击即落地**,删除上一轮的二次确认模态。
		// 复原按**名字路径**做(顶层属性名 → 逐层子行名):声明同步会整体重建属性表,`ScriptProperty*`
		// 会失效;映射键原样是一段,不按 '.' 拆串。
		// 当前正在绘制的脚本组件(集合头 `↺` 复原时带上实体/组件/语言)。
		Entity m_ScriptInspectingEntity;
		uint32_t m_ScriptInspectingComponentId = 0;
		bool m_ScriptInspectingLuau = false;
		// true = 当前属性表来自组件的 `ScriptProperty` 合成路径(false = Play 里 C++ 实例的真实结构体)。
		bool m_ScriptInspectingScriptRows = false;
		std::string m_ScriptInspectingComponentName;   // schema.DisplayName(prefab 覆盖登记用)
		// 当前绘制位置的**容器路径**(行路径 = 它 + 字段名);只有脚本属性行的递归维护它。
		std::vector<std::string> m_ScriptRowPath;
		// 本帧当前脚本组件的注解/schema 声明(只有 Luau 有):集合头的"形状是否偏离默认"判定要按
		// 声明的默认形状比较;每帧在 DrawScriptComponentInspector 开头刷新,离开该函数即失效(不再引用)。
		std::vector<ScriptProperties::Declaration> m_ScriptDeclarations;
		bool m_ScriptDeclarationsValid = false;
		// VEC-H6:集合头 `↺` 的落地**推迟到本属性画完** —— 点头部那一帧后面还要按旧合成 schema 递归画
		// 子行;立刻重建容器会让"schema(旧形状) vs Children(新形状)"错位一帧(与元素增删同一套延后口径)。
		ScriptProperty* m_PendingCollectionReset = nullptr;
		bool m_PendingCollectionResetLuau = false;
		std::vector<std::string> m_PendingCollectionResetPath;
		// 集合头 `↺` 单击即落地(无二次确认):Luau 按**单条声明**重建这一条(默认形状 + 默认值);
		// C++ 结构化表递归回默认值。container 就是本帧正在画的那个容器(合成路径下指针稳定)。
		void ApplyScriptCollectionReset(Wui::WuiContext& ctx, ScriptProperty& container, bool luau,
			const std::vector<std::string>& path);
		// VEC-F2:单项 `↺` 的落地 —— Luau 按名字路径把该行对齐到脚本**当前声明**的默认值
		// (Value 清成"未设" + Default 刷新):面板 `+`/`-` 只重排行名,旧 Default 会挂在改名后的行上,
		// 只回落到旧 Default 会显示"不是当前声明的默认值"。返回 false = 声明拿不到(C++ / 无 VM),
		// 调用方退回"只清 Value"的旧路径。
		bool ApplyScriptRowDeclaredReset(const std::string& rowName);

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
		// SCRIPT-V7:编辑器侧的"注解同步记忆"已删除 —— 属性新鲜度由引擎的声明缓存
		// (路径 + 内容指纹 + VM 可用性)判定,面板每帧调 ScriptEngine::SyncScriptDeclarations。
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
