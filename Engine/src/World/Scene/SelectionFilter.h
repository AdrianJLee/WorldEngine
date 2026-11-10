#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"

#include <cstdint>

namespace World
{
	class Scene;

	constexpr uint32_t Layer_Background = 1 << 0;
	constexpr uint32_t Layer_Decor      = 1 << 1;
	constexpr uint32_t Layer_Trigger    = 1 << 2;
	constexpr uint32_t Layer_Collider   = 1 << 3;
	constexpr uint32_t Layer_Light      = 1 << 4;
	constexpr uint32_t Layer_Actor      = 1 << 5;
	constexpr uint32_t Layer_UI         = 1 << 6;
	constexpr uint32_t Layer_All        = 0xFFFFFFFF;

	class WLD_API SelectionFilter
	{
	public:
		SelectionFilter() = default;
		~SelectionFilter() = default;

		void SetLayerSelectable(uint32_t layerMask, bool selectable);
		bool IsLayerSelectable(uint32_t layerMask) const;

		void SetLayerVisible(uint32_t layerMask, bool visible);
		bool IsLayerVisible(uint32_t layerMask) const;

		void SetLayerLocked(uint32_t layerMask, bool locked);
		bool IsLayerLocked(uint32_t layerMask) const;

		uint32_t GetSelectableMask() const { return m_SelectableMask; }
		uint32_t GetVisibleMask() const { return m_VisibleMask; }
		uint32_t GetLockedMask() const { return m_LockedMask; }

		void Reset();

		bool CanSelectEntity(Entity entity, const Scene& scene) const;
		static uint32_t ClassifyEntityLayer(Entity entity, const Scene& scene);

	private:
		uint32_t m_SelectableMask = Layer_All;
		uint32_t m_VisibleMask    = Layer_All;
		uint32_t m_LockedMask     = 0;
	};
}
