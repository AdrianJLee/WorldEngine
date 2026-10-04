#include "wldpch.h"
#include "World/Scene/Components.h"

#include <type_traits>

namespace World
{
	// WP3(PECS 1.1):2D physics runtime handles moved into the Scene-internal tables, so these
	// components are pure data again. Locked here so a future field can not silently
	// reintroduce a non-trivial or runtime-only member.
	static_assert(std::is_trivially_copyable_v<RigidBody2DComponent>,
		"RigidBody2DComponent must stay trivially copyable (no runtime handles)");
	static_assert(std::is_trivially_copyable_v<JointComponent>,
		"JointComponent must stay trivially copyable (no runtime handles)");
	static_assert(sizeof(RigidBody2DComponent) <= 64, "RigidBody2DComponent must stay within one cache line");
	static_assert(sizeof(JointComponent) <= 64, "JointComponent must stay within one cache line");
	static_assert(std::is_trivially_copyable_v<HierarchyComponent>,
		"HierarchyComponent must stay trivially copyable (the child list lives in HierarchyChildrenComponent)");
	static_assert(sizeof(HierarchyComponent) == 8, "HierarchyComponent must stay Parent + InheritTransform");

	// 相机数据导向化(2026-10-04):组件只留权威参数,投影矩阵是派生量(CameraViewComponent 缓存)。
	// 锁死 64B(一条 cache line)并禁止派生成员回流到组件。
	static_assert(std::is_trivially_copyable_v<CameraSettings>, "CameraSettings must stay trivially copyable (pure parameters)");
	static_assert(std::is_trivially_copyable_v<CameraComponent>, "CameraComponent must stay trivially copyable (no derived cache)");
	static_assert(sizeof(CameraComponent) <= 64, "CameraComponent must stay within one cache line");
}
