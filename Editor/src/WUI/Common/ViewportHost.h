#pragma once

#include "WUI/Common/EditorPanel.h"

#include "World/Renderer/EditorCamera.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/Texture/TextureLibrary.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/WUI/WuiGizmo.h"

namespace World
{
	// 视口面板所需的主机能力。视口与场景渲染/相机/播放状态强耦合,
	// 单独拆出该接口,避免把整组能力摊进所有面板共用的 PanelHost。
	class ViewportHost : public PanelHost
	{
	public:
		virtual bool HasRenderedScene() const = 0;
		virtual Ref<SceneRenderer>& GetSceneRenderer() = 0;
		virtual void SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]) = 0;
		virtual void SetViewportRect(const Wui::WuiRect& rect) = 0;
		virtual bool IsPlaying() const = 0;
		virtual bool IsSimulating() const = 0;
		virtual bool IsPaused() const = 0;
		virtual void TogglePlay() = 0;
		virtual void ToggleSimulate() = 0;
		virtual void TogglePause() = 0;
		virtual uint64_t GetIconId(int index) const = 0;
		virtual uint64_t GetSceneTextureId() const = 0;
		// 相机可视化:预览小窗(用场景相机渲染的 PiP)与视锥显示开关。
		virtual uint64_t GetCameraPreviewTextureId() const = 0;
		virtual bool IsCameraPreviewEnabled() const = 0;
		virtual void ToggleCameraPreview() = 0;
		virtual std::string CameraPreviewLabel() const = 0;
		virtual Entity PickEntityAt(glm::vec2 viewportLocal) = 0;
		virtual EditorCamera& GetEditorCamera() = 0;
		virtual Wui::GizmoOperation GetGizmoOperation() const = 0;

		// Level 2.0 数据层:当前文档声明的层(名字 + 掩码)与场景的激活掩码。
		// 视口工具栏按这里返回的层渲染开关 —— 开关改的是**真实**渲染剔除掩码,
		// 不是一个只翻状态、场景毫无变化的摆设。
		virtual const std::vector<Level::DataLayerDefinition>& GetLevelDataLayers() const = 0;
		virtual uint32_t GetActiveDataLayers() const = 0;
		virtual void SetActiveDataLayers(uint32_t mask) = 0;
	};
}
