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
}
