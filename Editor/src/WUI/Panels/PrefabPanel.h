#pragma once

#include "EditorPanel.h"
#include "World/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace World
{
	// P4-U13c/P4-U13e:prefab 资产窗口 —— 每个 `.wprefab` 一个面板(id = "prefab:<逻辑路径>")。
	//
	// 用户口径(2026-09-21:「为什么不给 wprefab 一个独立窗口呢」):
	//   - **窗口 = 看/管理这个资产**:实体树 + 只读组件摘要 + 引用资产 + 场景实例;
	//   - **编辑仍然进文档会话**(窗口里的 `Edit Prefab`):那里才有真 3D 视口与 gizmo。
	// 与 `.wmodel` 预览窗口同款:默认附加到主窗口,可拖出成独立 OS 窗口。
	//
	// P4-U13e(用户 2026-09-21:「单独打开 Prefab 应该可以预览和编辑」):窗口本身就要能
	// **预览 + 就地编辑**——
	//   - 左侧 3D 预览:`SceneRenderer` 把 staging 场景渲染到离屏目标(与视口/相机预览同一条
	//     提交路径),轨道相机(拖拽旋转 / 滚轮缩放 / 双击或 F 取景),设备换代时重建资源;
	//   - 右侧就地编辑:v1 只覆盖 Transform / MeshRenderer / Camera / 三种灯光组件,
	//     其余组件仍按只读摘要列出(小节标题写明"只读");
	//   - 保存闭环:任一字段改动 → 脏标记 + 头部 Save/Revert;Save 把 staging 写回该
	//     `.wprefab`(与 `Gameplay::SaveFromScene` 同一内核),Revert 重新读盘丢弃改动;
	//     有未保存改动时关闭窗口 / 进文档会话先问一次(宿主 EditorShell 的确认模态)。
	//
	// 读盘纪律:**不碰当前文档**。面板自己持有一个 staging `Ref<Scene>`,用
	// `SceneSerializer` 反序列化(与 `Gameplay::InstantiateFromFile` 同一条读法),
	// 带 2s TTL —— 每帧最多重扫一次,不做逐帧读盘;**有未保存改动时不重扫**
	// (否则用户的编辑会在 2s 后被磁盘内容静默覆盖)。
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

		// ---- P4-U13e:就地编辑 / 保存闭环 ----
		// 有未保存改动(标题带 `*`、头部出现 Save/Revert、无障碍树出现 prefab.dirty)。
		bool HasUnsavedChanges() const { return m_Dirty; }
		// 丢弃面板内的未保存改动(宿主确认"丢弃"后调用):下一帧从磁盘重读。
		void DiscardUnsavedChanges();
		// 把 staging 写回这个 `.wprefab`(Gameplay::SaveFromScene);成功后清脏并写 "Saved <时间>"。
		bool SaveToDisk(std::string* message = nullptr);
		// 重新读盘并丢弃未保存改动(Revert 按钮);状态行写 "Reverted ..."。
		bool RevertFromDisk(Scene* activeScene, std::string* message = nullptr);
		// AI 通道脚本化写字段(与面板控件同一条写入口):component/field/axis 用小节内的短名,
		// 例如 ("MeshRenderer","MeshPath","") / ("Transform","Location","x")。
		// 只接受 v1 的可编辑字段;失败 = false + 可读原因。
		bool SetEditableField(const std::string& component, const std::string& field,
			const std::string& value, const std::string& axis, std::string* message = nullptr);

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
		// P4-U13e:留给只读小节的摘要(可编辑组件之外的部分)。
		std::vector<std::string> BuildReadOnlySummary(entt::entity handle) const;
		// staging 场景的实例根(没有父节点的那个实体;prefab 文件按约定只有一个根)。
		entt::entity StagingRoot() const;
		// 状态行原文按优先级重算:资产路径警告 > 未保存 > Saved 时间 > "N entities · M ..."。
		void RefreshStatusText();
		// 未保存标记(任一字段真的改了之后调用)。
		void MarkDirty();
		// 用户改过网格/材质路径后,把"路径在内容根里找不到"写成可读警告(不阻断保存)。
		void RefreshAssetWarning();
		// prefab 引用的材质/网格逻辑路径(去重、稳定排序)。
		std::vector<std::string> CollectReferencedAssets() const;
		// 当前场景里这个 prefab 的实例(逐帧刷新:内存数据,增删要立刻反映到 locate 的可用性)。
		void RefreshInstances(Scene* scene);
		// 选中一个实例根(层级面板与视口都读 m_SelectedEntity,不需要额外接线)。
		void SelectInstance(PanelHost& host, Scene* scene, entt::entity root);

		// ---- P4-U13e:3D 预览(与 ModelPreviewPanel 同一套离屏写法) ----
		// 惰性创建/设备换代重建:SceneRenderer 的附件/命令缓冲属于当前设备,设备释放前必须
		// 把句柄放掉(见 ModelPreviewPanel 的同款说明)。
		void EnsurePreviewResources();
		void ReleasePreviewResources();
		// P4-U13f:预览离屏目标 = 预览区**物理像素**尺寸(设计单位 × UiScale),长边按
		// [128, 2048] 等比 clamp;每帧调用,矩形变化(窗口缩放/分离/挂靠/分栏)就重建目标。
		// 旧实现固定 320×320 再放大到整块预览区 —— 这就是用户看到的"非常模糊"。
		void UpdatePreviewTargetSize(const Wui::WuiRect& view);
		// 渲染预览到离屏目标并登记成 WUI 纹理;返回 0 = 不可用(调用方画可读原因)。
		uint64_t RenderPreview();
		// 取景:把轨道相机对到 staging 场景的包围盒上(首次自动 / 双击 / `F` / 头部按钮)。
		void FramePreview();
		// 预览取景用的包围盒(按实体世界变换的近似 AABB;不加载网格资源)。
		void PreviewFocusBounds(glm::vec3* center, float* radius) const;
		// 预览区:标题 + WuiImage + 轨道交互 + 无障碍节点(prefab.preview / .hint / .camera)。
		void DrawPreview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		// 预览不可用时的可读原因(空 = 可用)。
		std::string PreviewUnavailableReason() const;

		// ---- P4-U13e:就地编辑(右侧第一段) ----
		// 可编辑字段小节(Transform / MeshRenderer / Camera / 灯光)+ 只读摘要小节。
		void DrawEditableComponents(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		// 一行 vec3(标签 + X/Y/Z 三个 drag-float);返回占用的高度。changed = 本帧真的改过。
		float DrawVec3Row(Wui::WuiContext& ctx, float x, float y, float width, const char* idPrefix,
			const std::string& label, const std::string& tooltip, glm::vec3& value, float speed,
			const Wui::WuiTheme& theme, bool& changed);
		// 一行标量(标签 + 单个 drag-float)。
		float DrawScalarRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
			const std::string& label, const std::string& tooltip, float& value, float speed,
			float min, float max, const Wui::WuiTheme& theme, bool& changed);
		// 一行颜色(标签 + 取色器)。
		float DrawColorRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
			const std::string& label, const std::string& tooltip, glm::vec4& value,
			const Wui::WuiTheme& theme, bool& changed);
		// 一行资产路径(标签 + 可搜索资产下拉,选项来自 EditorAssetCatalog;"(none)" = 清空)。
		float DrawAssetRow(Wui::WuiContext& ctx, float x, float y, float width, const char* idText,
			const std::string& label, const std::string& tooltip, const std::string& assetType,
			std::string& value, const Wui::WuiTheme& theme, bool& changed);
		// 右侧第一段的总高度估算(滚动区内容高度要在绘制前给出)。
		float EstimateEditorHeight() const;

		// ---- 只读视图 ----
		void DrawEntityTree(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
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
		float m_DetailsScroll = 0.0f;
		double m_NextDiskScan = 0.0;

		// ---- P4-U13e:3D 预览状态 ----
		Ref<SceneRenderer> m_PreviewRenderer;
		void* m_PreviewGpuDevice = nullptr;
		uint64_t m_PreviewTextureId = 0;
		const void* m_PreviewTextureHandle = nullptr;
		uint32_t m_UiTextureGeneration = 0;
		// P4-U13f:离屏目标的真实尺寸(物理像素)/ 交给 SceneRenderer::OnResize 的请求尺寸
		// (两者在 rendering.render_scale ≠ 1 时不同:请求值 = 目标值 ÷ 倍率)。
		uint32_t m_PreviewTargetW = 320;
		uint32_t m_PreviewTargetH = 320;
		uint32_t m_PreviewRequestedW = 320;
		uint32_t m_PreviewRequestedH = 320;
		float m_PreviewUiScale = 1.0f;
		float m_PreviewRenderScale = 1.0f;
		bool m_PreviewSizeDirty = true;
		bool m_PreviewFramed = false;
		bool m_Orbiting = false;
		float m_OrbitYaw = 0.6f;
		float m_OrbitPitch = 0.25f;
		float m_CameraDistance = 4.0f;
		float m_MinDistance = 0.5f;
		float m_MaxDistance = 200.0f;
		glm::vec3 m_Focus { 0.0f };
		glm::vec2 m_LastMouse { 0.0f };

		// ---- P4-U13e:就地编辑状态 ----
		bool m_Dirty = false;
		std::string m_LastSavedText;
		std::string m_AssetWarning;
	};
}
