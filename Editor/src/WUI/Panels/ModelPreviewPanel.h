#pragma once

#include "EditorPanel.h"
#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Asset/WModelIO.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"
#include "World/RHI/Rhi.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace World
{
	// P1b D5:模型预览面板 —— 每个 `.wmodel` 一个独立窗口(id = "model:<逻辑路径>")。
	//
	// 用户反馈"双击 .wmodel 每次都往场景里叠一份"之后的口径(draft → 2026-09-18):
	//   - **双击 `.wmodel` 只打开本预览,不改场景**;要放进场景请在预览里点"放进当前场景";
	//   - 预览用轨道相机(拖拽旋转 / 滚轮缩放),按 .wmodel 的节点树逐 submesh + 材质槽绘制;
	//   - 统计行给出节点/mesh/submesh/顶点/索引/包围盒,便于排查导入结果。
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
		// P1b D5b-2:用当前 `.wimport` 重新导入并刷新(与内容浏览器双击/CLI 同一条 `Asset::ImportFile` 路径)。
		bool Reimport(std::string* message = nullptr);
		const std::string& StatusText() const { return m_Status; }

	private:
		void EnsureGpuResources();
		void ReleaseGpuResources();
		// 渲染预览到离屏目标;返回可交给 WuiImage 的纹理 id(0 = 不可用)。
		uint64_t RenderPreview();
		// 无障碍诊断:WLD_PREVIEW_TEX_CAPTURE=<目录> 把预览纹理连续写成 PPM(后端无关 RHI 读回)。
		void CapturePreviewTextureSequence();
		// 节点树 × 各 mesh 局部包围盒 → 世界空间包围盒(相机取景用)。
		MeshBounds ComputeWorldBounds() const;
		// 由 `.wmodel` 逻辑路径推源路径(同目录同名 `.gltf`/`.glb`;引擎导入约定)。
		void ResolveSource();
		// 刷新"需要重导"判断(源内容指纹 + 设置哈希 + 导入器版本)。
		void RefreshSyncState();
		void DrawAssetView(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		// D5c-4b:动画控制条(有 skin + ≥1 条动画时显示)。返回控制条之后的 y。
		float DrawAnimationControls(Wui::WuiContext& ctx, float x, float y, float width,
			const Wui::WuiTheme& theme);
		// 资产里是否存在绑定到有效 skin 的 mesh(决定是否显示控制条)。
		bool HasSkinnedMesh() const;
		// 预览播放的是第 0 条 clip(与状态行/时间滑杆同一来源)。
		float AnimationClipDuration() const;

		std::string m_PanelId = "model:";
		std::string m_PanelTitle = "Model";
		std::string m_LogicalPath;
		std::string m_Status;
		bool m_StatusIsError = false;
		Ref<Mesh> m_Mesh;
		// 按 .wmodel 材质槽下标缓存材质(加载失败保持 null → 走常量色)。
		std::vector<Ref<Material>> m_SlotMaterials;
		// D5b-2:磁盘上的 .wmodel 数据(meta/节点/子网格/材质槽;与 m_Mesh 同源,取元信息用)。
		Asset::WModelData m_Data;
		bool m_DataValid = false;
		// 源与设置状态。
		std::string m_SourceLogical;                 // models/x.gltf(空 = 找不到源)
		bool m_SourceExists = false;
		bool m_NeedsReimport = false;
		std::string m_SyncDetail;                    // 需要重导的原因(给 UI/自动化看)
		Asset::ModelImportSettings m_Settings;       // 当前 .wimport(可编辑 → 保存 → 重导)
		bool m_SettingsLoaded = false;
		// 上次读入 `.wimport` 时的内容指纹:外部改动自动重载(用户正在编辑但未保存的字段不被覆盖)。
		uint64_t m_SettingsFileFingerprint = 0;
		// "需要重导"状态的轮询节流(秒;源文件很小,1s 读一次足够)。
		double m_NextSyncCheck = 0.0;

		// ---- 预览状态 ----
		uint32_t m_PreviewSize = 384;
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

		// ---- D5c-4b:动画播放状态(全部只在面板内,不改场景) ----
		bool m_AnimPlaying = false;
		bool m_AnimLoop = true;
		float m_AnimSpeed = 1.0f;
		float m_AnimTime = 0.0f;
		// 面板自己的帧间 dt:首帧 0,clamp [0,0.25](不动 EditorLayer/PanelHost 接口)。
		double m_LastAnimClock = 0.0;
		bool m_AnimClockValid = false;
		// 蒙皮提交被拒的一次性 warn 去重(透明材质 / 调色板不可用)。
		bool m_TransparentSkipWarned = false;
		bool m_SkinnedSubmitWarned = false;

		// ---- GPU 资源(预览专用,惰性创建) ----
		Rhi::Handle<Rhi::RenderPass> m_PreviewPass;
		Rhi::Handle<Rhi::Framebuffer> m_PreviewFramebuffer;
		Rhi::Handle<Rhi::Texture> m_PreviewColor;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityId;
		Rhi::Handle<Rhi::Texture> m_PreviewDepth;
		Rhi::Handle<Rhi::CommandBuffer> m_PreviewCommandBuffer;
		Rhi::Handle<Rhi::Buffer> m_PreviewCameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_PreviewCameraSet;
	};
}
