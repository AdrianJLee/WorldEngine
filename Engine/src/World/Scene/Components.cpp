#include "wldpch.h"
#include "World/Scene/Components.h"

namespace World
{

	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source)
	{
		RigidBody2DComponent copy;
		copy.Type = source.Type;
		copy.FixedRotation = source.FixedRotation;
		return copy;
	}

}
