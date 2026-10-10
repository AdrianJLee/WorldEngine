#include "wldpch.h"
#include "World/Gameplay/Prefab/PrefabDiffSerializer.h"

#include "World/Schema/SchemaWriter.h"

#include <yaml-cpp/yaml.h>
#include <cstring>

namespace World::Gameplay
{
	bool PrefabDiffSerializer::DiffEntityComponents(
		const Entity& instanceEntity,
		const Entity& templateEntity,
		const Schema::SchemaRegistry& schemas,
		std::unordered_map<std::string, std::string>& outOverrides)
	{
		outOverrides.clear();
		if (!instanceEntity.IsValid() || !templateEntity.IsValid())
			return false;

		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage)
				continue;
			if (schema->NotInPrefab || schema->Id.Name == "World::UUIDComponent"
				|| schema->Id.Name == "World::HierarchyComponent")
				continue;

			const bool hasInst = instanceEntity.HasComponent(schema->Storage->ComponentId);
			const bool hasTmpl = templateEntity.HasComponent(schema->Storage->ComponentId);

			if (!hasInst && !hasTmpl)
				continue;

			void* instPtr = hasInst ? const_cast<Entity&>(instanceEntity).GetComponent(schema->Storage->ComponentId) : nullptr;
			void* tmplPtr = hasTmpl ? const_cast<Entity&>(templateEntity).GetComponent(schema->Storage->ComponentId) : nullptr;

			// 实例有、模板无：整组件为外来新增
			if (hasInst && !hasTmpl && instPtr)
			{
				YAML::Emitter out;
				out << YAML::BeginMap;
				Schema::YamlSchemaWriter writer(out);
				writer.BeginType(*schema, instPtr);
				for (const Schema::FieldSchema& field : schema->Fields)
					writer.WriteField(field, instPtr);
				writer.EndType(*schema);
				out << YAML::EndMap;
				outOverrides[schema->Id.Name] = out.c_str();
				continue;
			}

			// 两侧皆有：两级快速 Diff
			if (hasInst && hasTmpl && instPtr && tmplPtr)
			{
				// L1 快速初筛：平凡小组件且无 padding 时 memcmp 为 0 说明 100% 相等
				if (schema->Size <= 64 && std::memcmp(instPtr, tmplPtr, schema->Size) == 0)
					continue;

				// L2 字段级精细比对：仅将产生差异的字段写出
				YAML::Emitter out;
				out << YAML::BeginMap;
				Schema::YamlSchemaWriter writer(out);
				bool anyFieldDiff = false;

				for (const Schema::FieldSchema& field : schema->Fields)
				{
					if (field.Meta.Transient)
						continue;
					const Schema::Value instVal = Schema::ReadSchemaField(field, instPtr);
					const Schema::Value tmplVal = Schema::ReadSchemaField(field, tmplPtr);
					if (instVal != tmplVal)
					{
						anyFieldDiff = true;
						writer.WriteField(field, instPtr);
					}
				}

				out << YAML::EndMap;
				if (anyFieldDiff)
					outOverrides[schema->Id.Name] = out.c_str();
			}
		}

		return !outOverrides.empty();
	}
}


