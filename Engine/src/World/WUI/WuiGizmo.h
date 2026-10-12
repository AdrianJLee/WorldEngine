#pragma once

#include "World/WUI/WuiContext.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"

namespace World::Wui
{
	enum class GizmoOperation
	{
		None = -1,
		Translate,
		Rotate,
		Scale,
		Rect,
	};

	// D7-1b:相机视图摘要 —— 2D(EditorCamera)与 3D(EditorCamera3D)都能提供,
	// gizmo 只依赖这份数据(投影矩阵 + 相机基向量 + 距离/FOV),不依赖具体相机类型。
	struct GizmoCamera
	{
		glm::mat4 ViewProjection { 1.0f };   // GL 约定(未做 Vulkan 适配):与视口图像方向一致
		glm::vec3 Right { 1.0f, 0.0f, 0.0f };
		glm::vec3 Up { 0.0f, 1.0f, 0.0f };
		glm::vec3 Forward { 0.0f, 0.0f, 1.0f };
		// 相机世界位置:面朝向判定用(选中盒只画朝向相机的那几条棱)。
		glm::vec3 Position { 0.0f, 0.0f, 0.0f };
		float Distance = 10.0f;
		float FovDegrees = 45.0f;
		bool IsOrthographic = false;
		float OrthographicSize = 10.0f;
	};

	// WUI Gizmo:在视口内直接操纵实体的 Transform。返回本帧是否正在拖拽。
	//  - 平移:三根轴箭头(X 红 / Y 绿 / Z 蓝),沿轴约束拖动;
	//  - 缩放:三根轴 + 方块端点(中心方块 = 等比缩放);
	//  - 旋转:三根轴各自的圆环(投影成椭圆,按轴约束);
	//  - allowManipulation=false(Play/Simulate 只读查看):只画不响应拖拽。
	WLD_API bool ManipulateGizmo(const GizmoCamera& camera, GizmoOperation operation,
		TransformComponent& transform, const WuiRect& viewport, WuiContext& ctx,
		bool allowManipulation = true);

	// WUI 2D 矩形变换手柄 (Rect Transform Tool: 四角缩放/四边拉伸/中心锚点)
	WLD_API bool ManipulateRect2D(const GizmoCamera& camera,
		TransformComponent& transform, glm::vec2& pivot, glm::vec2 size,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation = true);

	// WUI 2D 碰撞体编辑手柄 (Edit Collider Gizmos)
	WLD_API bool ManipulateBoxCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, glm::vec2& size,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation = true);

	WLD_API bool ManipulateCircleCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, float& radius,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation = true);

	WLD_API bool ManipulateCapsuleCollider2D(const GizmoCamera& camera,
		const TransformComponent& transform, glm::vec2& offset, float& radius, float& length, int direction,
		const WuiRect& viewport, WuiContext& ctx, bool allowManipulation = true);
}
