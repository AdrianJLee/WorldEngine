#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace World
{
	struct MeshLODComponent;

	class WLD_API MeshLODEvaluator
	{
	public:
		constexpr static uint8_t kCulled = 255;

		// 带有滞后防抖带的 LOD 级别计算 (0=LOD0, 1=LOD1, 2=LOD2, 255=裁剪剔除)
		static uint8_t EvaluateLOD(
			float distanceToCamera,
			float lod1Distance,
			float lod2Distance,
			float cullDistance,
			uint8_t currentLOD,
			float hysteresis = 2.0f);
	};

	struct MeshLODStats
	{
		uint32_t TotalLODEntities = 0;
		uint32_t LOD0Count = 0;
		uint32_t LOD1Count = 0;
		uint32_t LOD2Count = 0;
		uint32_t CulledCount = 0;
	};

	class WLD_API MeshLODManager
	{
	public:
		static MeshLODManager& Get();

		uint8_t EvaluateLOD(float distance, const MeshLODComponent& comp, uint8_t currentLOD);

		const MeshLODStats& GetStats() const { return m_Stats; }
		void ResetStats() { m_Stats = MeshLODStats{}; }
		void RecordLODResult(uint8_t lod);

		void SetForcedLOD(int8_t level) { m_ForcedLOD = level; } // -1 = Auto, 0=LOD0, 1=LOD1, 2=LOD2
		int8_t GetForcedLOD() const { return m_ForcedLOD; }

		void SetLODColorationEnabled(bool enable) { m_ColorationEnabled = enable; }
		bool IsLODColorationEnabled() const { return m_ColorationEnabled; }

		void SetGlobalDistanceScale(float scale) { m_GlobalDistanceScale = scale; }
		float GetGlobalDistanceScale() const { return m_GlobalDistanceScale; }

		glm::vec4 GetLODColor(uint8_t lod) const;

	private:
		MeshLODManager() = default;

		MeshLODStats m_Stats;
		int8_t m_ForcedLOD { -1 };
		bool m_ColorationEnabled { false };
		float m_GlobalDistanceScale { 1.0f };
	};
}