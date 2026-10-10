#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <memory>
#include <string>

namespace World::Level
{
	class WLD_API DataLayerManager
	{
	public:
		DataLayerManager();
		~DataLayerManager();

		void RegisterLayer(uint32_t mask, const std::string& name, bool defaultActive = true);
		void SetLayerActive(const std::string& name, bool active);
		bool IsLayerActive(const std::string& name) const;

		uint32_t GetActiveMask() const;
		bool IsEntityVisible(uint32_t entityMask) const;

		size_t GetRegisteredLayerCount() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
