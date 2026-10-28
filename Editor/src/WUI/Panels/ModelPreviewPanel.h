#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Asset/ModelImportSettings.h"
#include "World/Asset/WModelIO.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Renderer.h"
#include "World/RHI/Rhi.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_set>
#include <vector>

namespace World
{
	// P1b D5: 模型预览面板 —— 工业级 3D 资产检视、材质诊断、骨骼树与动画走带全要素工作台。
	// 每个 .wmodel 一个独立窗口 (id = "model:<逻辑路径>")。
	// 契约:
	//   - 资产检视与离屏预览完全只读，不修改当前关卡与场景；
	//   - 视口提供 3D 轨道漫游、透视参考网格 (Grid)、AABB 包围盒线框 (Bounds) 与坐标轴向指示器；
	//   - 选项卡覆盖 5 大模块 (Overview / Materials / Skeleton / Import Settings / Clips)；
	//   - 动画提供专业走带控制器 (Transport Bar) 与交互式时间轴擦洗条 (Scrub Timeline Track)。
	class ModelPreviewPanel final : public EditorPanel
	{
	public:
		explicit ModelPreviewPanel(std::string logicalPath);
		~ModelPreviewPanel() override;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		const std::string& LogicalPath() const { return m_LogicalPath; }
		// 重新从磁盘读模型(导入覆盖后手动刷新用)。
		void Reload();
		// P1b D5b-2 / P4-U11: 用面板当前的设置重新导入并刷新 —— 设置随 .wmodel 存盘。
		bool Reimport(std::string* message = nullptr);
		const std::string& StatusText() const { return m_Status; }
		void RequestPreviewCapture(const std::string& path) { m_PendingPreviewCapture = path; }

	private:
		void EnsureGpuResources();
		void ReleaseGpuResources();
		void UpdatePreviewTargetSize(const Wui::WuiRect& previewRect);
		// 渲染预览到离屏目标; 返回可交给 WuiImage 的纹理 id (0 = 不可用)。
		uint64_t RenderPreview();
		// 无障碍诊断: WLD_PREVIEW_TEX_CAPTURE=<目录> 把预览纹理连续写成 PPM。
		void CapturePreviewTextureSequence();
		// 节点树 × 各 mesh 局部包围盒 → 世界空间包围盒 (相机取景用)。
		MeshBounds ComputeWorldBounds() const;
		// 用 .wmodel 逻辑路径推源路径(同目录同名 .gltf/.glb; 引擎导入约定)。
		void ResolveSource();
		// 刷新"需要重导"判断(源内容指纹 + 设置哈希 + 导入器版本)。
		void RefreshSyncState();

		// ---- 顶部与视口叠加层 ----
		void DrawTopToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& bar, PanelHost& host,
			const Wui::WuiTheme& theme);
		void DrawViewportOverlay(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
			const Wui::WuiTheme& theme);
		void DrawGroundGrid(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
			const Wui::WuiTheme& theme);
		void DrawBoundsWireframe(Wui::WuiContext& ctx, const Wui::WuiRect& previewRect,
			const Wui::WuiTheme& theme);

		// ---- 动画走带与擦洗时间轴 ----
		float DrawTransportBar(Wui::WuiContext& ctx, float x, float y, float width,
			const Wui::WuiTheme& theme);

		// ---- 5 大模块选项卡 ----
		void DrawModuleTabs(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
			const Wui::WuiTheme& theme);
		float DrawTabOverview(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
			const Wui::WuiTheme& theme);
		float DrawTabMaterials(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
			const Wui::WuiTheme& theme);
		float DrawTabSkeleton(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
			const Wui::WuiTheme& theme);
		float DrawTabImportSettings(Wui::WuiContext& ctx, const Wui::WuiRect& rect,
			const Wui::WuiTheme& theme);
		float DrawTabClips(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host,
			const Wui::WuiTheme& theme);

		// ---- 材质与骨骼辅助操作 ----
		void ApplySlotChange(size_t index, const std::string& newPath);
		void CreateAndAssignMaterial(size_t index);
		void DrawNodeInspector(Wui::WuiContext& ctx, float x, float& y, float width,
			const Wui::WuiTheme& theme);
		void RecordKeyframeForNode(uint32_t nodeIndex, float time);
		void RecordKeyframeForAllNodes(float time);
		void SaveCurrentClipToFile();
		void ExportActiveClipToWClip(PanelHost& host);
		void CreateNewClipAsset(PanelHost& host);

		// 兼容旧接口(测试与外部引用点)
		float DrawImportSettings(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
		{
			return DrawTabImportSettings(ctx, rect, theme);
		}
		float DrawStatsAndDependencies(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
		{
			return DrawTabOverview(ctx, rect, host, host.Theme());
		}
		float DrawAnimationControls(Wui::WuiContext& ctx, float x, float y, float width,
			const Wui::WuiTheme& theme)
		{
			return DrawTransportBar(ctx, x, y, width, theme);
		}

		// 相机取景到世界包围盒(双击预览 / F / 角落按钮都走它)。
		void FramePreview();
		// 当前预览播放的动画条(m_AnimClipIndex 越界时回退第 0 条; 空 = 没有动画)。
		const Asset::WModelAnimation* ActiveClip() const;
		// 资产里是否存在绑定到有效 skin 的 mesh。
		bool HasSkinnedMesh() const;
		// 当前 clip 时长(秒)。
		float AnimationClipDuration() const;

		std::string m_PendingPreviewCapture;
		std::string m_PanelId = "model:";
		std::string m_PanelTitle = "Model";
		std::string m_LogicalPath;
		std::string m_Status;
		bool m_StatusIsError = false;
		Ref<Mesh> m_Mesh;
		// 按 .wmodel 材质槽下标缓存材质(加载失败保持 null → 走常量色)。
		std::vector<Ref<Material>> m_SlotMaterials;
		// 磁盘上的 .wmodel 数据(meta/节点/子网格/材质槽; 与 m_Mesh 同源)。
		Asset::WModelData m_Data;
		bool m_DataValid = false;
		// 源与设置状态。
		std::string m_SourceLogical;
		bool m_SourceExists = false;
		bool m_NeedsReimport = false;
		std::string m_SyncDetail;
		Asset::ModelImportSettings m_Settings;
		bool m_SettingsLoaded = false;
		bool m_SettingsFromAsset = false;
		bool m_SettingsDirty = false;
		double m_NextSyncCheck = 0.0;

		// ---- 视口与相机状态 ----
		uint32_t m_PreviewWidth = 0;
		uint32_t m_PreviewHeight = 0;
		void* m_PreviewTextureHandle = nullptr;
		uint64_t m_PreviewTextureId = 0;
		uint32_t m_UiTextureGeneration = 0;
		int m_PreviewCaptureFrame = 0;
		int m_PreviewCaptureWritten = 0;
		void* m_GpuDevice = nullptr;
		bool m_Orbiting = false;
		float m_OrbitYaw = 0.6f;
		float m_OrbitPitch = 0.25f;
		float m_CameraDistance = 4.0f;
		float m_MinDistance = 0.5f;
		float m_MaxDistance = 200.0f;
		glm::vec3 m_Focus { 0.0f };
		glm::vec2 m_LastMouse { 0.0f };

		// 视口投影与辅助显示状态
		bool m_ShowGrid = true;
		bool m_ShowWireframe = false;
		bool m_ShowMesh = true;
		std::string m_PreviewMeshPath;
		bool m_ShowGizmo = true;
		bool m_ShowBounds = false;
		glm::mat4 m_LastCameraView { 1.0f };
		glm::mat4 m_LastCameraProj { 1.0f };

		// ---- 动画播放与时间线状态 ----
		bool m_AnimPlaying = false;
		bool m_AnimLoop = true;
		float m_AnimSpeed = 1.0f;
		float m_AnimTime = 0.0f;
		double m_LastAnimClock = 0.0;
		bool m_AnimClockValid = false;
		int m_AnimClipIndex = 0;
		bool m_Scrubbing = false;
		bool m_TimelineShowFrames = false;

		// ---- 模块选项卡状态 ----
		// 0: Overview, 1: Materials, 2: Skeleton, 3: Import Settings, 4: Clips
		int m_ActiveTab = 0;
		float m_TabScroll[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float m_TabContentHeight[5] = { 400.0f, 400.0f, 400.0f, 400.0f, 400.0f };
		std::string m_SkeletonFilter;
		std::unordered_set<std::string> m_SkeletonCollapsed;
		std::unordered_set<int> m_SkeletonCollapsedNodes;

		// ---- 材质编辑状态 ----
		std::vector<std::string> m_MaterialSlotBuffers;

		// ---- 骨骼检视与编辑状态 ----
		int m_SelectedNode = -1;
		std::string m_SelectedNodeNameBuffer;
		int32_t m_SelectedNodeParent = -1;
		glm::vec3 m_SelectedNodeTranslation { 0.0f };
		glm::vec3 m_SelectedNodeRotationEuler { 0.0f };
		glm::vec3 m_SelectedNodeScale { 1.0f };

		// ---- 视口富显示模式与折叠工具栏 ----
		bool m_ShowBones = true;
		bool m_ShowBoneNames = false;
		bool m_ShowJointAxes = false;
		bool m_OccludeBackBounds = true;
		bool m_ToolbarCollapsed = false;
		bool m_DisplayModesOpen = false;
		bool m_CameraMenuOpen = false;

		// ---- 动画片段编辑状态 ----
		bool m_NewClipOpen = false;
		std::string m_NewClipNameBuffer = "NewClip";
		float m_NewClipDurationBuffer = 1.0f;
		bool m_SplitClipOpen = false;
		float m_SplitStart = 0.0f;
		float m_SplitEnd = 1.0f;
		std::string m_ClipRenameBuffer;
		int m_ClipRenameIndex = -1;

		// 蒙皮提交被拒的一次性 warn 去重
		bool m_TransparentSkipWarned = false;
		bool m_SkinnedSubmitWarned = false;

		// ---- GPU 资源(预览专用, 惰性创建) ----
		Rhi::Handle<Rhi::RenderPass> m_PreviewPass;
		Rhi::Handle<Rhi::Framebuffer> m_PreviewFramebuffer;
		Rhi::Handle<Rhi::Texture> m_PreviewColor;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityId;
		Rhi::Handle<Rhi::Texture> m_PreviewDepth;
		Rhi::Handle<Rhi::Texture> m_PreviewColorMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityMsaa;
		Rhi::Handle<Rhi::Texture> m_PreviewDepthMsaa;
		Rhi::Handle<Rhi::CommandBuffer> m_PreviewCommands[Renderer::FramesInFlight];
		Rhi::Handle<Rhi::Buffer> m_PreviewCameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_PreviewCameraSet;
	};
}