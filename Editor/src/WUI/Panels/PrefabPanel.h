#pragma once

#include "EditorPanel.h"

#include <string>
#include <vector>

namespace World
{
	// P4-U13c:prefab 资产窗口 —— 每个 `.wprefab` 一个面板(id = "prefab:<逻辑路径>")。
	//
	// 用户口径(2026-09-21:「为什么不给 wprefab 一个独立窗口呢」):
	//   - **窗口 = 看/管理这个资产**:实体树 + 只读组件摘要 + 引用资产 + 场景实例;
	//   - **编辑仍然进文档会话**(窗口里的 `Edit Prefab`):那里才有真 3D 视口与 gizmo。
	// 与 `.wmodel` 预览窗口同款:默认附加到主窗口,可拖出成独立 OS 窗口。
	//
	// 读盘纪律:**不碰当前文档**。面板自己持有一个 staging `Ref<Scene>`,用
	// `SceneSerializer` 反序列化(与 `Gameplay::InstantiateFromFile` 同一条读法),
	// 带 2s TTL —— 每帧最多重扫一次,不做逐帧读盘。
	class PrefabPanel final : public EditorPanel
	{
	public:
		explicit PrefabPanel(std::string logicalPath);
		~PrefabPanel() override;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const std::string& LogicalPath() const { return m_LogicalPath; }
		// 状态行原文(加载成功 = "N entities · M referenced assets";失败 = 可读原因)。
		const std::string& StatusText() const { return m_Status; }
		// 这份资产当前能否解析成 prefab 文档(失败时窗口仍然在,状态行写原因)。
		bool HasDocument() const { return m_DocumentValid; }

		// 立即(同步)读盘一次:打开窗口时用,也是"点一下刷新"的入口。
		// 返回 false = 读不了(找不到 / 不是可解析的 prefab 文档),原因写进 message 与状态行。
		bool ReloadNow(Scene* activeScene, std::string* message = nullptr);

	private:
		struct EntityRow
		{
			entt::entity Handle = entt::null;
			std::string Name;
			size_t ComponentCount = 0;
			int Depth = 0;
		};
		struct InstanceRow
		{
			entt::entity Handle = entt::null;
			std::string Name;
		};

		// 2s TTL 的读盘节流(force = 打开窗口 / 重复双击时立刻重读)。
		void RefreshFromDisk(Scene* activeScene, bool force);
		// 重建 staging 场景并反序列化;失败时 m_DocumentValid=false、m_Status 写可读原因。
		void LoadStaging(Scene* activeScene);
		// 从 staging 场景重建只读实体树(层级顺序 + 每个实体的组件数)。
		void RebuildRows();
		// 选中实体的只读组件摘要(组件名 + 关键字段);没有组件 = 一条"无组件"。
		std::vector<std::string> BuildComponentSummary(size_t rowIndex) const;
		// prefab 引用的材质/网格逻辑路径(去重、稳定排序)。
		std::vector<std::string> CollectReferencedAssets() const;
		// 当前场景里这个 prefab 的实例(逐帧刷新:内存数据,增删要立刻反映到 locate 的可用性)。
		void RefreshInstances(Scene* scene);
		// 选中一个实例根(层级面板与视口都读 m_SelectedEntity,不需要额外接线)。
		void SelectInstance(PanelHost& host, Scene* scene, entt::entity root);

		// ---- 三段只读视图 ----
		void DrawEntityTree(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawDetails(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawAssets(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawInstances(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host, Scene* scene,
			const Wui::WuiTheme& theme);

		std::string m_PanelId = "prefab:";
		std::string m_PanelTitle = "Prefab";
		std::string m_LogicalPath;
		std::string m_Status;
		bool m_StatusIsError = false;
		bool m_DocumentValid = false;

		Ref<Scene> m_Staging;
		std::vector<EntityRow> m_Rows;
		std::vector<std::string> m_Assets;
		std::vector<InstanceRow> m_Instances;
		int m_SelectedRow = 0;
		int m_SelectedAsset = -1;
		entt::entity m_SelectedInstance = entt::null;

		float m_TreeScroll = 0.0f;
		float m_AssetsScroll = 0.0f;
		float m_InstancesScroll = 0.0f;
		double m_NextDiskScan = 0.0;
	};
}
