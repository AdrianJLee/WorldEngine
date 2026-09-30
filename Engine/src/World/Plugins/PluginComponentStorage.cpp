#include "wldpch.h"
#include "PluginComponentStorage.h"

#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"

#include <array>
#include <cstddef>
#include <unordered_map>
#include <utility>

namespace World::Plugins
{
	namespace
	{
		// 插件组件的 blob 存储类:尺寸由 id 的档位位决定,对齐 = 宿主上限(16)。
		// 一个 id 一个类型(档位进 id)⇒ entt 的 storage<Blob>(id) 永远不会撞类型。
		template <uint32_t Id>
		struct PluginComponentBlob
		{
			static constexpr uint32_t Tier = (Id >> 8) & 0x7u;
			static_assert((Id & kPluginComponentIdFlag) != 0,
				"plugin component id must live in the reserved high segment");
			static_assert(Tier < kPluginComponentTierCount,
				"plugin component id carries an out-of-range tier");

			alignas(kPluginComponentAlignmentCap) std::uint8_t Bytes[kPluginComponentTierBytes[Tier]] {};
		};

		// 字段访问器按 "实例指针 + offset" 读写;实例指针 = blob 地址,所以 Bytes 必须在 offset 0。
		template <uint32_t Id>
		constexpr bool BlobBytesAtOffsetZero()
		{
			return offsetof(PluginComponentBlob<Id>, Bytes) == 0;
		}

		// Add:给实体加一个清零的 blob(与引擎组件的 StorageBinding::Add 同语义)。
		// 走 Scene::GetRegistry()(内部 AssertStructuralWrite)—— 与 Entity::AddComponent<T>()
		// 的结构写门禁一致,只是按宿主分配的 id 取存储而不是按 C++ 类型哈希。
		template <uint32_t Id>
		void AddPluginComponentBlob(void* rawEntity)
		{
			auto* entity = static_cast<World::Entity*>(rawEntity);
			entt::registry& registry = entity->GetScene()->GetRegistry();
			registry.storage<PluginComponentBlob<Id>>(Id)
				.emplace(static_cast<entt::entity>(*entity));
		}

		// Copy:单实体复制(Scene::DuplicateEntity);目标实体上已有 = 覆盖。
		template <uint32_t Id>
		void CopyPluginComponentBlob(void* rawDst, void* rawSrc)
		{
			auto* dst = static_cast<World::Entity*>(rawDst);
			auto* src = static_cast<World::Entity*>(rawSrc);
			entt::registry& dstRegistry = dst->GetScene()->GetRegistry();
			entt::registry& srcRegistry = src->GetScene()->GetRegistry();
			auto& srcStorage = srcRegistry.storage<PluginComponentBlob<Id>>(Id);
			auto& dstStorage = dstRegistry.storage<PluginComponentBlob<Id>>(Id);

			const entt::entity srcEntity = static_cast<entt::entity>(*src);
			const entt::entity dstEntity = static_cast<entt::entity>(*dst);
			dstStorage.remove(dstEntity);   // 幂等:没有该组件时是 no-op
			dstStorage.emplace(dstEntity, srcStorage.get(srcEntity));
		}

		// CopyAll:整场景复制(Scene::CopyScene;编辑器 Play 的活动场景副本)。
		// 与引擎组件的写法同一口径:只处理能按 UUID 找到目标实体的源实体。
		template <uint32_t Id>
		void CopyAllPluginComponentBlobs(void* rawDstRegistry, void* rawSrcRegistry,
			const void* rawEntityMap)
		{
			auto& dstRegistry = *static_cast<entt::registry*>(rawDstRegistry);
			auto& srcRegistry = *static_cast<entt::registry*>(rawSrcRegistry);
			const auto& entityMap = *static_cast<
				const std::unordered_map<World::UUID, entt::entity>*>(rawEntityMap);

			// 源场景没有这个存储(或它已空)= 无事可做;不为了查询凭空建存储。
			const auto* probe = srcRegistry.storage(Id);
			if (!probe || probe->size() == 0)
				return;

			auto& srcStorage = srcRegistry.storage<PluginComponentBlob<Id>>(Id);
			auto& dstStorage = dstRegistry.storage<PluginComponentBlob<Id>>(Id);
			for (auto&& [entity, blob] : srcStorage.each())
			{
				const World::UUIDComponent* identity =
					srcRegistry.try_get<World::UUIDComponent>(entity);
				if (!identity)
					continue;
				const auto mapped = entityMap.find(identity->ID);
				if (mapped == entityMap.end())
					continue;
				dstStorage.remove(mapped->second);   // 幂等
				dstStorage.emplace(mapped->second, blob);
			}
		}

		// 把 (档位, 槽位) 固定成一个编译期 id ⇒ 每个 id 一枚 blob 类型与一组回调。
		template <uint32_t Tier, uint32_t Slot>
		void FillPluginComponentBinding(World::Schema::StorageBinding* outBinding)
		{
			constexpr uint32_t Id = MakePluginComponentId(Tier, Slot);
			static_assert(BlobBytesAtOffsetZero<Id>(),
				"plugin component blob must keep its byte array at offset 0");
			outBinding->ComponentId = Id;
			outBinding->Add = &AddPluginComponentBlob<Id>;
			outBinding->Copy = &CopyPluginComponentBlob<Id>;
			outBinding->CopyAll = &CopyAllPluginComponentBlobs<Id>;
		}

		using FillBindingFn = void (*)(World::Schema::StorageBinding*);

		template <uint32_t Slot, uint32_t... Tiers>
		constexpr std::array<FillBindingFn, sizeof...(Tiers)>
			MakePluginComponentTierRow(std::integer_sequence<uint32_t, Tiers...>)
		{
			return { &FillPluginComponentBinding<Tiers, Slot>... };
		}

		template <uint32_t... Slots>
		constexpr std::array<std::array<FillBindingFn, kPluginComponentTierCount>, sizeof...(Slots)>
			MakePluginComponentBindingTable(std::integer_sequence<uint32_t, Slots...>)
		{
			return { MakePluginComponentTierRow<Slots>(
				std::make_integer_sequence<uint32_t, kPluginComponentTierCount>{})... };
		}

		// [槽位][档位] → 填充函数(运行时按 (Size, slot) 查表;编译期全表展开)。
		constexpr auto kPluginComponentBindingTable = MakePluginComponentBindingTable(
			std::make_integer_sequence<uint32_t, kPluginComponentSlotCount>{});

		// ---- PLUG-T6:实例级操作(按 id 的档位/槽位寻址;与上面同一张编译期展开表)----
		struct PluginComponentInstanceOps
		{
			void* (*Value)(entt::registry&, entt::entity) = nullptr;
			const void* (*ValueConst)(const entt::registry&, entt::entity) = nullptr;
			bool (*Add)(entt::registry&, entt::entity) = nullptr;
			bool (*Remove)(entt::registry&, entt::entity) = nullptr;
		};

		template <uint32_t Id>
		void* ValueOfPluginComponentBlob(entt::registry& registry, entt::entity entity)
		{
			auto& storage = registry.storage<PluginComponentBlob<Id>>(Id);
			return storage.contains(entity) ? &storage.get(entity) : nullptr;
		}

		template <uint32_t Id>
		const void* ValueConstOfPluginComponentBlob(const entt::registry& registry, entt::entity entity)
		{
			const auto* storage = registry.storage<PluginComponentBlob<Id>>(Id);
			return (storage && storage->contains(entity)) ? &storage->get(entity) : nullptr;
		}

		template <uint32_t Id>
		bool AddOfPluginComponentBlob(entt::registry& registry, entt::entity entity)
		{
			auto& storage = registry.storage<PluginComponentBlob<Id>>(Id);
			if (storage.contains(entity))
				return false;
			storage.emplace(entity);   // 值初始化 = 全零(与 AddPluginComponentBlob 同语义)
			return true;
		}

		template <uint32_t Id>
		bool RemoveOfPluginComponentBlob(entt::registry& registry, entt::entity entity)
		{
			auto& storage = registry.storage<PluginComponentBlob<Id>>(Id);
			if (!storage.contains(entity))
				return false;
			storage.remove(entity);
			return true;
		}

		template <uint32_t Tier, uint32_t Slot>
		constexpr void FillPluginComponentInstanceOps(PluginComponentInstanceOps* out)
		{
			constexpr uint32_t Id = MakePluginComponentId(Tier, Slot);
			out->Value = &ValueOfPluginComponentBlob<Id>;
			out->ValueConst = &ValueConstOfPluginComponentBlob<Id>;
			out->Add = &AddOfPluginComponentBlob<Id>;
			out->Remove = &RemoveOfPluginComponentBlob<Id>;
		}

		template <uint32_t Slot, uint32_t... Tiers>
		constexpr std::array<PluginComponentInstanceOps, sizeof...(Tiers)>
			MakePluginComponentInstanceOpsRow(std::integer_sequence<uint32_t, Tiers...>)
		{
			std::array<PluginComponentInstanceOps, sizeof...(Tiers)> row {};
			((FillPluginComponentInstanceOps<Tiers, Slot>(&row[Tiers])), ...);
			return row;
		}

		template <uint32_t... Slots>
		constexpr std::array<std::array<PluginComponentInstanceOps, kPluginComponentTierCount>,
			sizeof...(Slots)>
			MakePluginComponentInstanceOpsTable(std::integer_sequence<uint32_t, Slots...>)
		{
			return { MakePluginComponentInstanceOpsRow<Slots>(
				std::make_integer_sequence<uint32_t, kPluginComponentTierCount>{})... };
		}

		// [槽位][档位] → 实例操作(与绑定表同一索引口径)。
		constexpr auto kPluginComponentInstanceOpsTable = MakePluginComponentInstanceOpsTable(
			std::make_integer_sequence<uint32_t, kPluginComponentSlotCount>{});

		const PluginComponentInstanceOps* LookupPluginComponentInstanceOps(uint32_t componentId)
		{
			if ((componentId & kPluginComponentIdFlag) == 0)
				return nullptr;
			const uint32_t tier = (componentId >> 8) & 0x7u;
			const uint32_t slot = componentId & 0xFFu;
			if (tier >= kPluginComponentTierCount || slot >= kPluginComponentSlotCount)
				return nullptr;
			return &kPluginComponentInstanceOpsTable[slot][tier];
		}
	}

	bool MakePluginComponentStorageBinding(uint32_t componentSize, uint32_t alignment,
		uint32_t slot, World::Schema::StorageBinding* outBinding, std::string* error)
	{
		const auto reject = [error](const std::string& reason)
		{
			if (error) *error = reason;
			return false;
		};

		if (!outBinding)
			return reject("no output binding");
		if (componentSize == 0)
			return reject("declared size 0 means schema-only (no storage bridge)");
		if (componentSize > kPluginComponentMaxBytes)
			return reject("declared size " + std::to_string(componentSize)
				+ " exceeds the host blob maximum (" + std::to_string(kPluginComponentMaxBytes) + ")");
		if (alignment != 0)
		{
			const bool powerOfTwo = (alignment & (alignment - 1u)) == 0;
			if (!powerOfTwo || alignment > kPluginComponentAlignmentCap)
				return reject("declared alignment " + std::to_string(alignment)
					+ " must be a power of two <= " + std::to_string(kPluginComponentAlignmentCap));
			if (componentSize % alignment != 0)
				return reject("declared size " + std::to_string(componentSize)
					+ " is not a multiple of the declared alignment " + std::to_string(alignment));
		}
		if (slot >= kPluginComponentSlotCount)
			return reject("plugin component slot " + std::to_string(slot)
				+ " is out of range (max " + std::to_string(kPluginComponentSlotCount) + ")");

		const uint32_t tier = PluginComponentTierFor(componentSize);
		if (tier >= kPluginComponentTierCount)
			return reject("no blob tier covers declared size " + std::to_string(componentSize));

		kPluginComponentBindingTable[slot][tier](outBinding);
		return true;
	}

	bool AddPluginComponentInstance(entt::registry& registry, uint32_t componentId,
		entt::entity entity)
	{
		const PluginComponentInstanceOps* ops = LookupPluginComponentInstanceOps(componentId);
		return ops && ops->Add ? ops->Add(registry, entity) : false;
	}

	bool RemovePluginComponentInstance(entt::registry& registry, uint32_t componentId,
		entt::entity entity)
	{
		const PluginComponentInstanceOps* ops = LookupPluginComponentInstanceOps(componentId);
		return ops && ops->Remove ? ops->Remove(registry, entity) : false;
	}

	void* PluginComponentInstancePointer(entt::registry& registry, uint32_t componentId,
		entt::entity entity)
	{
		const PluginComponentInstanceOps* ops = LookupPluginComponentInstanceOps(componentId);
		return ops && ops->Value ? ops->Value(registry, entity) : nullptr;
	}

	const void* PluginComponentInstancePointer(const entt::registry& registry, uint32_t componentId,
		entt::entity entity)
	{
		const PluginComponentInstanceOps* ops = LookupPluginComponentInstanceOps(componentId);
		return ops && ops->ValueConst ? ops->ValueConst(registry, entity) : nullptr;
	}
}
