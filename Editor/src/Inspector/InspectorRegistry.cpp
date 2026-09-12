#include "InspectorRegistry.h"

namespace World
{
	// 定义在 ComponentInspectors.cpp 的反射自动检查器。
	bool DrawAutoInspector(const Schema::TypeSchema& componentSchema, uint32_t componentId, Entity entity);

	std::unordered_map<entt::id_type, InspectorRegistry::InspectorFunc>& InspectorRegistry::Registry()
	{
		static std::unordered_map<entt::id_type, InspectorFunc> registry;
		return registry;
	}

	void InspectorRegistry::Register(entt::id_type componentId, InspectorFunc inspector)
	{
		if (inspector)
			Registry()[componentId] = inspector; // 重复注册：后者覆盖。
	}

	bool InspectorRegistry::Draw(const Schema::TypeSchema& componentSchema, entt::id_type componentId, Entity entity)
	{
		auto& registry = Registry();
		auto it = registry.find(componentId);
		if (it != registry.end() && it->second)
			return it->second(entity);
		return DrawAutoInspector(componentSchema, componentId, entity);
	}
}
