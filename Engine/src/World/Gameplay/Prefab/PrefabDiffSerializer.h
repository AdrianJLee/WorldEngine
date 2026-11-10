#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Prefab/PrefabTypes.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Schema/Schema.h"

#include <string>
#include <unordered_map>

namespace World::Gameplay
{
	// Prefab 2.0 (M1/M2):内存逆向差异序列化器。
	// 通过 Schema 反射在内存中将实例组件与模板原型比对，自动提取最小变更集。
	class WLD_API PrefabDiffSerializer
	{
	public:
		// 在内存中比对 instanceEntity 与 templateEntity，提取所有差异组件与差异字段。
		// 返回 outOverrides[ComponentName] = "FieldName: ...\n" 的 YAML 字符串字典。
		// 若完全无差异，返回 false，outOverrides 保持为空。
		static bool DiffEntityComponents(
			const Entity& instanceEntity,
			const Entity& templateEntity,
			const Schema::SchemaRegistry& schemas,
			std::unordered_map<std::string, std::string>& outOverrides);
	};
}
