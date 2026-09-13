#pragma once

#include "EditorPanel.h"

#include "World/Renderer/EditorCamera.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Renderer/Texture.h"
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
		virtual Ref<Texture2D> GetIcon(int index) const = 0;
		virtual uint64_t GetIconId(int index) const = 0;
		virtual uint64_t GetSceneTextureId() const = 0;
		virtual Entity PickEntityAt(glm::vec2 viewportLocal) = 0;
		virtual EditorCamera& GetEditorCamera() = 0;
		virtual Wui::GizmoOperation GetGizmoOperation() const = 0;
	};
}
