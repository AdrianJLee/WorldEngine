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

	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source)
	{
		RigidBody2DComponent copy;
		copy.Type = source.Type;
		copy.FixedRotation = source.FixedRotation;
		return copy;
	}

}
