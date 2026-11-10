#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace World::Level
{
	class WLD_API DeltaStateRegistry
	{
	public:
		DeltaStateRegistry();
		~DeltaStateRegistry();

		void MarkDestroyed(uint64_t entityUuid);
		bool IsDestroyed(uint64_t entityUuid) const;

		void SetProperty(uint64_t entityUuid, const std::string& key, const std::string& val);
		std::string GetProperty(uint64_t entityUuid, const std::string& key, const std::string& fallback = "") const;
		bool HasProperty(uint64_t entityUuid, const std::string& key) const;

		void Clear();
		size_t DestroyedCount() const;
		size_t PropertyCount() const;

		// 序列化与持久化 (.wdelta 空间差分切片补丁)
		bool SaveToFile(const std::string& filepath) const;
		bool LoadFromFile(const std::string& filepath);

		// 访问内部脏标记数据
		const std::unordered_set<uint64_t>& GetDestroyedEntities() const;
		const std::unordered_map<uint64_t, std::unordered_map<std::string, std::string>>& GetPropertyOverrides() const;

		// 流送唤醒反序列化重放: 将差分补丁重放到刚换入的场景实体上
		static void ApplyDeltaState(Scene& scene, const DeltaStateRegistry& delta);

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
