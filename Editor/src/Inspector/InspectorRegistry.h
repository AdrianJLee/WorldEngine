#pragma once

#include "World/Schema/Schema.h"
#include "World/Scene/Entity.h"

#include <entt.hpp>
#include <unordered_map>

namespace World
{
	// 组件属性检查器：有自定义绘制器则用之，否则走反射驱动的自动检查器。
	class InspectorRegistry
	{
	public:
		using InspectorFunc = bool (*)(Entity);

		static void Register(entt::id_type componentId, InspectorFunc inspector);
		// 返回本帧是否改动了可序列化数据。
		static bool Draw(const Schema::TypeSchema& componentSchema, entt::id_type componentId, Entity entity);

	private:
		static std::unordered_map<entt::id_type, InspectorFunc>& Registry();
	};
}
