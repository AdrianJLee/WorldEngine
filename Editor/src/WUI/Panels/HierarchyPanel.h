#pragma once

#include "EditorPanel.h"
#include "World/WUI/WuiWidget.h"
#include "World/Gameplay/Prefab.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace World
{
	// 场景层级面板:实体列表、选择与上下文菜单。菜单目标跨帧保留在模型里。
	class HierarchyPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "hierarchy"; }
		const char* Title() const override { return "Scene Hierarchy"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 开发/验证入口:按索引触发该行的点击回调(与 GUI 点击走同一个 OnClick),
		// 供 WLD_HIERARCHY_CLICK 自动化复现"Play 下点击层级行"这条真实路径。
		bool DebugInvokeRowClick(size_t index);

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
		// ---- P4-U13d:"Create Prefab from Selection…" 的居中模态 ----
		// 用户此前反馈(「你这 wprefab 啥呀,太难用了」):旧入口用实体 Tag 当文件名、固定写
		// prefabs/、同名静默覆盖、没有任何回显。现在名称/目录/落点/覆盖全部可见且可改:
		// 名称默认 = 实体 Tag(空 Tag 用 "Prefab"),后缀 .wprefab 自动补,目标已存在 = 红字提示 +
		// 主按钮变 Overwrite,非法名称禁用主按钮并给原因。
		bool m_CreateOpen = false;
		uint32_t m_CreateOpenedFrame = 0;
		Entity m_CreateRoot;
		std::string m_CreateName;
		std::vector<std::string> m_CreateFolders;   // 打开时扫一次的内容根目录清单(逻辑路径)
		int m_CreateFolderIndex = 0;
		std::string m_CreateFailure;                // 上一次创建失败的可读原因(显示在提示行)
		std::string m_CreateFailureFor;             // 该原因对应的落点(落点一改就自动清掉)
		void OpenCreatePrefabModal(Wui::WuiContext& ctx, PanelHost& host, Entity root);
		void CloseCreatePrefabModal(Wui::WuiContext& ctx, PanelHost& host);
		void DrawCreatePrefabModal(Wui::WuiContext& ctx, PanelHost& host);
		// 名称 → 基础名(去首尾空白、剥掉用户可能多打的 .wprefab 后缀)/ 目标逻辑路径 / 校验原因。
		std::string CreatePrefabBaseName() const;
		std::string CreatePrefabTarget() const;
		std::string CreatePrefabNameError() const;
		// P4-U13b:实例破坏性动作的确认模态(Apply 写回资产 / Unpack 断开链接)。
		// 实例记录本身由 Scene 持有(存档一起走),面板只保留"待确认的动作"这一帧间状态。
		enum class PrefabConfirmAction { None = 0, Apply, Unpack };
		PrefabConfirmAction m_PrefabConfirm = PrefabConfirmAction::None;
		Entity m_PrefabConfirmRoot;
		std::string m_PrefabConfirmSource;
		Wui::WuiId m_PrefabConfirmModal = 0;
		void OpenPrefabConfirm(Wui::WuiContext& ctx, PanelHost& host, PrefabConfirmAction action,
			Entity root, const std::string& source);
		void ClosePrefabConfirm(Wui::WuiContext& ctx, PanelHost& host);
		void DrawPrefabConfirm(Wui::WuiContext& ctx, PanelHost& host);
		glm::vec2 m_MenuPos {};
		glm::vec2 m_BlankMenuPos {};
	};
}
