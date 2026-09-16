#pragma once

#include "EditorPanel.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/Mesh.h"
#include "World/RHI/Rhi.h"

#include <string>
#include <vector>

namespace World
{
	// D3:材质编辑器(默认独立窗口形态,与 Widget Gallery / Input Map 同级)。
	//
	// 职责:
	//  - 查看:显示材质路径、脏标记、加载警告;
	//  - 预览:离屏渲染材质球,左键拖拽旋转、滚轮缩放(镜头远近);
	//  - 编辑:BaseColor/Metallic/Roughness/Emissive/BlendMode/DoubleSided + 两个贴图槽,
	//    Save/Revert;新建材质走内容浏览器(New Material)。
	//  - 每个材质一个面板/窗口(id = "material:<path>"),可同时打开多个。
	class MaterialEditorPanel final : public EditorPanel
	{
	public:
		MaterialEditorPanel();
		explicit MaterialEditorPanel(std::string materialPath);
		~MaterialEditorPanel() override;

		const char* Id() const override { return m_PanelId.c_str(); }
		const char* Title() const override { return m_PanelTitle.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

		// 由内容浏览器/Window 菜单调用:打开指定 .wmat(失败时面板显示错误而不是弹窗)。
		void OpenMaterial(const std::string& path);
		bool HasMaterial() const { return m_Material != nullptr; }
		const std::string& GetMaterialPath() const { return m_Path; }
		void SetMaterialPathForPanel(const std::string& path);

	private:
		void EnsureGpuResources();
		void ReleaseGpuResources();
		// 渲染预览球到离屏目标;返回可交给 WuiImage 的纹理 id(0 = 不可用)。
		uint64_t RenderPreview();
		void DrawToolbar(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void DrawParameters(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host);
		void SaveCurrent();
		void RefreshCatalog();
		void RefreshPickerIndices();

		// ---- 文档状态 ----
		std::string m_PanelId = "material:";
		std::string m_PanelTitle = "Material";
		Ref<Material> m_Material;
		std::string m_Path;                 // 当前材质路径(空 = 未落盘新材质)
		std::string m_NewPathBuffer;        // 未落盘时的目标路径输入
		std::string m_Status;               // 最近一次操作结果
		bool m_StatusIsError = false;
		std::vector<std::string> m_MaterialPaths;
		std::vector<std::string> m_TexturePaths;
		int m_MaterialPickIndex = -1;
		int m_AlbedoPickIndex = 0;
		int m_NormalPickIndex = 0;
		double m_CatalogRefreshTime = 0.0;

		// ---- 预览状态 ----
		uint32_t m_PreviewSize = 256;
		uint64_t m_PreviewTextureId = 0;
		uint32_t m_UiTextureGeneration = 0;
		void* m_GpuDevice = nullptr;        // 记录资源所属设备,设备重建时整体失效
		bool m_Orbiting = false;
		float m_OrbitYaw = 0.6f;            // 弧度:绕 Y
		float m_OrbitPitch = 0.25f;         // 弧度:绕 X
		float m_CameraDistance = 2.6f;      // 预览相机距离(滚轮缩放)
		glm::vec2 m_LastMouse { 0.0f };

		// ---- GPU 资源(预览专用,惰性创建) ----
		Rhi::Handle<Rhi::RenderPass> m_PreviewPass;
		Rhi::Handle<Rhi::Framebuffer> m_PreviewFramebuffer;
		Rhi::Handle<Rhi::Texture> m_PreviewColor;
		Rhi::Handle<Rhi::Texture> m_PreviewEntityId;
		Rhi::Handle<Rhi::Texture> m_PreviewDepth;
		Rhi::Handle<Rhi::CommandBuffer> m_PreviewCommandBuffer;
		Rhi::Handle<Rhi::Buffer> m_PreviewCameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_PreviewCameraSet;
		Ref<Mesh> m_PreviewSphere;
	};
}
