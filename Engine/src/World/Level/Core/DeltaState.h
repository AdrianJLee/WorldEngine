#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace World::Level
{
	class DeltaStateRegistry
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

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
