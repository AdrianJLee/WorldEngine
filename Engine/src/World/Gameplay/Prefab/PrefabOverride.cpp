#include "wldpch.h"
#include "World/Gameplay/Prefab/Prefab.h"

#include "World/Core/Log.h"
#include "World/Schema/Schema.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"

#include <memory>
#include <unordered_set>

namespace World::Gameplay
{
	namespace
	{
		// 按先序把子树展平成实体列表(实例与 prefab 同构,顺序一一对应)。
		void Flatten(const entt::registry& registry, entt::entity root, std::vector<entt::entity>& out)
		{
			std::vector<entt::entity> stack { root };
			while (!stack.empty())
			{
				const entt::entity current = stack.back();
				stack.pop_back();
				if (!registry.valid(current))
					continue;
				out.push_back(current);
				if (!registry.any_of<HierarchyComponent>(current))
					continue;
				const auto& children = Hierarchy::ChildrenOf(registry, current);
				for (auto it = children.rbegin(); it != children.rend(); ++it)
					stack.push_back(*it);
			}
		}

		template <typename T>
		void RestoreComponentIfPresent(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			if (const auto* component = source.try_get<T>(from))
				destination.emplace_or_replace<T>(to, *component);
		}

		void RestoreBuiltinComponents(const entt::registry& source, entt::registry& destination,
			entt::entity from, entt::entity to)
		{
			RestoreComponentIfPresent<TagComponent>(source, destination, from, to);
			RestoreComponentIfPresent<TransformComponent>(source, destination, from, to);
			RestoreComponentIfPresent<SpriteComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CircleRendererComponent>(source, destination, from, to);
			RestoreComponentIfPresent<MeshRendererComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CameraComponent>(source, destination, from, to);
			// WP3(PECS 1.1):句柄不落组件 ⇒ 与其它组件同一条整体回滚路径。
			RestoreComponentIfPresent<RigidBody2DComponent>(source, destination, from, to);
			RestoreComponentIfPresent<BoxCollider2DComponent>(source, destination, from, to);
			RestoreComponentIfPresent<CircleCollider2DComponent>(source, destination, from, to);
		}

		// ---- P2-b:ApplyPrefabChanges 的字段解析与快照辅助 ----

		// 覆盖登记串形如 "<类型名>.<字段名>"(属性面板登记的是 schema.DisplayName;
		// 引擎内部与测试也用短名/全名,三者都要命中)。首个 '.' 之前 = 类型名,
		// 之后是字段名(保留字段名里可能存在的 '.')。没有 '.' → 字段名为空(无法解析)。
		void SplitOverrideField(const std::string& entry, std::string& typeName, std::string& fieldName)
		{
			const size_t separator = entry.find('.');
			if (separator == std::string::npos)
			{
				typeName = entry;
				fieldName.clear();
				return;
			}
			typeName = entry.substr(0, separator);
			fieldName = entry.substr(separator + 1);
		}

		// 只接受可落到实体存储上的组件类型(Storage != nullptr)。
		const Schema::TypeSchema* FindOverrideComponentSchema(const Schema::SchemaRegistry& schemas,
			const std::string& name)
		{
			const Schema::TypeSchema* exact = schemas.Find(name);
			if (exact && exact->Category == Schema::TypeCategory::Component && exact->Storage)
				return exact;
			// Find 已覆盖 TypeId 全名 + 短名别名;这里补显示名回退。
			for (const Schema::TypeSchema* candidate : schemas.List(Schema::TypeCategory::Component))
				if (candidate && candidate->Storage && candidate->DisplayName == name)
					return candidate;
			return nullptr;
		}

		const Schema::FieldSchema* FindOverrideField(const Schema::TypeSchema& schema, const std::string& name)
		{
			for (const Schema::FieldSchema& field : schema.Fields)
				if (field.Name == name || (!field.Meta.DisplayName.empty() && field.Meta.DisplayName == name))
					return &field;
			return nullptr;
		}

		void* ComponentInstance(entt::registry& registry, uint32_t componentId, entt::entity entity)
		{
			auto* storage = registry.storage(componentId);
			return storage && storage->contains(entity) ? storage->value(entity) : nullptr;
		}

		constexpr size_t kNoOrderIndex = static_cast<size_t>(-1);

		// 先序列表里该实体的父节点下标;父在列表外(如实例根挂在宿主实体下)或缺失 → kNoOrderIndex。
		size_t ParentOrderIndex(const entt::registry& registry, entt::entity entity,
			const std::unordered_map<entt::entity, size_t>& order)
		{
			const auto* hierarchy = registry.try_get<HierarchyComponent>(entity);
			if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
				return kNoOrderIndex;
			const auto it = order.find(hierarchy->Parent);
			return it == order.end() ? kNoOrderIndex : it->second;
		}
	}

	void MarkOverride(PrefabInstanceRecord& record, entt::entity entity, const std::string& field)
	{
		if (entity == entt::null || field.empty())
			return;
		auto& fields = record.Overrides[static_cast<uint32_t>(entity)];
		if (std::find(fields.begin(), fields.end(), field) == fields.end())
			fields.push_back(field);
	}

	bool HasOverride(const PrefabInstanceRecord& record, entt::entity entity)
	{
		const auto it = record.Overrides.find(static_cast<uint32_t>(entity));
		return it != record.Overrides.end() && !it->second.empty();
	}

	size_t GetOverrideCount(const PrefabInstanceRecord& record)
	{
		size_t count = 0;
		for (const auto& [entity, fields] : record.Overrides)
			count += fields.size();
		return count;
	}

	void ClearOverrides(PrefabInstanceRecord& record)
	{
		record.Overrides.clear();
	}

	bool CanRevert(const PrefabInstanceRecord& record, const Scene& scene)
	{
		return record.IsValid() && !record.PrefabPath.empty() && scene.GetRegistry().valid(record.Root);
	}

	bool UnpackInstance(PrefabInstanceRecord& record)
	{
		if (!record.IsValid())
			return false;
		ClearOverrides(record);
		record.PrefabPath.clear();
		WLD_CORE_INFO("Prefab::UnpackInstance: subtree at entity {0} is no longer a prefab instance",
			static_cast<uint32_t>(record.Root));
		return true;
	}

	bool RevertInstance(PrefabInstanceRecord& record, Scene& scene)
	{
		if (!record.IsValid() || record.PrefabPath.empty())
		{
			WLD_CORE_WARN("Prefab::RevertInstance: record has no prefab source");
			return false;
		}
		if (!scene.GetRegistry().valid(record.Root))
		{
			WLD_CORE_WARN("Prefab::RevertInstance: instance root is no longer valid");
			return false;
		}

		// 重新实例化的临时场景(与 InstantiateFromFile 同一路径,保证"回滚 = 回到 prefab 原值")。
		const Ref<Scene> fresh = CreateRef<Scene>(scene.GetContext());
		const PrefabInstanceResult staged = InstantiateFromFile(record.PrefabPath, *fresh);
		if (!staged.IsValid())
			return false;

		std::vector<entt::entity> sourceOrder;
		std::vector<entt::entity> instanceOrder;
		const entt::registry& sourceRegistry = fresh->GetRegistry();
		entt::registry& instanceRegistry = scene.GetRegistry();
		Flatten(sourceRegistry, static_cast<entt::entity>(staged.Root), sourceOrder);
		Flatten(instanceRegistry, record.Root, instanceOrder);
		if (sourceOrder.size() != instanceOrder.size())
		{
			WLD_CORE_WARN("Prefab::RevertInstance: instance structure differs from prefab ({0} vs {1} entities)",
				instanceOrder.size(), sourceOrder.size());
			return false;
		}

		for (size_t i = 0; i < sourceOrder.size(); ++i)
			RestoreBuiltinComponents(sourceRegistry, instanceRegistry, sourceOrder[i], instanceOrder[i]);

		ClearOverrides(record);
		WLD_CORE_INFO("Prefab::RevertInstance: {0} entities restored from '{1}'",
			instanceOrder.size(), record.PrefabPath);
		return true;
	}

	bool ApplyPrefabChanges(PrefabInstanceRecord& record, Scene& scene, std::string* error)
	{
		const auto fail = [error](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};
		if (error)
			error->clear();

		if (!record.IsValid() || record.PrefabPath.empty())
			return fail("prefab instance record has no source (PrefabPath is empty)");
		entt::registry& instanceRegistry = scene.GetRegistry();
		if (!instanceRegistry.valid(record.Root))
			return fail("prefab instance root is no longer valid in the scene");

		// 1) 读盘 + 展平:所有结构检查都在改动任何实体之前完成(先检查、后动手)。
		//    与 RevertInstance 同一套机制(临时场景 + InstantiateFromFile),不新增拷贝路径。
		const Ref<Scene> fresh = CreateRef<Scene>(scene.GetContext());
		const PrefabInstanceResult staged = InstantiateFromFile(record.PrefabPath, *fresh);
		if (!staged.IsValid())
			return fail("failed to read prefab '" + record.PrefabPath
				+ "' (missing file, invalid content or no root entity)");

		const entt::registry& sourceRegistry = fresh->GetRegistry();
		std::vector<entt::entity> sourceOrder;
		std::vector<entt::entity> instanceOrder;
		Flatten(sourceRegistry, static_cast<entt::entity>(staged.Root), sourceOrder);
		Flatten(instanceRegistry, record.Root, instanceOrder);
		if (sourceOrder.size() != instanceOrder.size())
			return fail("instance structure differs from prefab ("
				+ std::to_string(instanceOrder.size()) + " vs " + std::to_string(sourceOrder.size())
				+ " entities)");

		// 层级同构:同一先序下标上,两侧父节点在各自列表里的位置必须一致(只看下标,
		// 不看句柄)。实例如根挂在场景宿主实体下时,该父在列表外,两侧同样不计。
		{
			std::unordered_map<entt::entity, size_t> sourceIndex;
			sourceIndex.reserve(sourceOrder.size());
			for (size_t i = 0; i < sourceOrder.size(); ++i)
				sourceIndex.emplace(sourceOrder[i], i);
			std::unordered_map<entt::entity, size_t> instanceIndex;
			instanceIndex.reserve(instanceOrder.size());
			for (size_t i = 0; i < instanceOrder.size(); ++i)
				instanceIndex.emplace(instanceOrder[i], i);

			// 先序起点(实例根)的父节点由调用方决定,不参与比较。
			for (size_t i = 1; i < sourceOrder.size(); ++i)
			{
				const size_t sourceParent = ParentOrderIndex(sourceRegistry, sourceOrder[i], sourceIndex);
				const size_t instanceParent = ParentOrderIndex(instanceRegistry, instanceOrder[i], instanceIndex);
				if (sourceParent != instanceParent)
					return fail("instance hierarchy differs from prefab at entity #" + std::to_string(i));
			}
		}

		// 2) 恢复前快照:record.Overrides 里每个 (实体, 字段) 取实例当前值。
		//    取到的 Value 是拷贝,恢复期的 emplace_or_replace 不会动它。
		struct OverrideSnapshot
		{
			entt::entity Entity = entt::null;
			const Schema::FieldSchema* Field = nullptr;
			uint32_t ComponentId = 0;
			Schema::Value Value;
		};
		const Schema::SchemaRegistry& schemas = scene.GetContext().Schemas();
		std::vector<OverrideSnapshot> snapshots;
		std::string skipped;   // 解析不了/取不到的字段:跳过但留可读原因(不中断其它字段)
		const auto noteSkipped = [&skipped](const std::string& reason)
		{
			if (!skipped.empty())
				skipped += "; ";
			skipped += reason;
		};
		for (const auto& [rawEntity, fields] : record.Overrides)
		{
			const entt::entity entity = static_cast<entt::entity>(rawEntity);
			if (!instanceRegistry.valid(entity))
			{
				noteSkipped("override entity " + std::to_string(rawEntity) + " no longer exists");
				continue;
			}
			for (const std::string& entry : fields)
			{
				std::string typeName;
				std::string fieldName;
				SplitOverrideField(entry, typeName, fieldName);
				const Schema::TypeSchema* schema = typeName.empty()
					? nullptr : FindOverrideComponentSchema(schemas, typeName);
				const Schema::FieldSchema* field = (schema && !fieldName.empty())
					? FindOverrideField(*schema, fieldName) : nullptr;
				if (!field)
				{
					noteSkipped("override '" + entry + "' has no matching schema field");
					continue;
				}
				void* instance = ComponentInstance(instanceRegistry, schema->Storage->ComponentId, entity);
				if (!instance)
				{
					noteSkipped("component '" + schema->Id.Name + "' is not on the override entity ("
						+ entry + ")");
					continue;
				}
				Schema::Value value = Schema::ReadSchemaField(*field, instance);
				if (std::holds_alternative<std::monostate>(value))
				{
					// Transient 字段(RotationQuat/Transform 缓存)与缺访问器都取不到可恢复的值。
					noteSkipped("override '" + entry + "' has no readable value (transient or missing accessor)");
					continue;
				}
				snapshots.push_back(OverrideSnapshot {
					entity, field, schema->Storage->ComponentId, std::move(value) });
			}
		}

		// 3) 逐实体恢复内建组件(与 RevertInstance 同一函数;句柄不重建,覆盖记录保持有效)。
		for (size_t i = 0; i < sourceOrder.size(); ++i)
			RestoreBuiltinComponents(sourceRegistry, instanceRegistry, sourceOrder[i], instanceOrder[i]);

		// 4) 把快照值写回。指针在恢复后**重新解析**:emplace_or_replace 会移动存储内的
		//    元素,恢复前拿到的 void* 不能跨这一步使用。
		const uint32_t transformComponentId =
			static_cast<uint32_t>(entt::type_id<TransformComponent>().hash());
		std::unordered_set<uint32_t> transformsTouched;
		for (const OverrideSnapshot& snapshot : snapshots)
		{
			void* instance = ComponentInstance(instanceRegistry, snapshot.ComponentId, snapshot.Entity);
			if (!instance || !Schema::WriteSchemaField(*snapshot.Field, instance, snapshot.Value))
			{
				noteSkipped("override '" + snapshot.Field->Name + "' could not be written back to entity "
					+ std::to_string(static_cast<uint32_t>(snapshot.Entity)));
				continue;
			}
			if (snapshot.ComponentId == transformComponentId)
				transformsTouched.insert(static_cast<uint32_t>(snapshot.Entity));
		}

		// 5) TransformComponent 的 RotationQuat/Transform 是派生缓存(Transient,不参与覆盖),
		//    字段级写回只动了 Location/Rotation/Scale 中的一项;按组件自身 API 重算缓存,
		//    被覆盖的位移/旋转/缩放在 Hierarchy::UpdateWorldTransforms 里才真正生效。
		for (const uint32_t rawEntity : transformsTouched)
		{
			if (auto* transform = instanceRegistry.try_get<TransformComponent>(
				static_cast<entt::entity>(rawEntity)))
				transform->SetTransform(transform->Location, transform->Rotation, transform->Scale);
		}

		if (error)
			*error = skipped;
		WLD_CORE_INFO("Prefab::ApplyPrefabChanges: {0} entities followed '{1}' ({2} override field(s) kept)",
			instanceOrder.size(), record.PrefabPath, snapshots.size());
		if (!skipped.empty())
			WLD_CORE_WARN("Prefab::ApplyPrefabChanges: skipped override field(s): {0}", skipped);
		return true;
	}
}
