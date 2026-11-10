#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace World::EQS
{
	struct WLD_API CoverReservation
	{
		glm::vec3 Position { 0.0f };
		uint32_t OwnerEntity = 0;
		float RemainingTime = 5.0f;
	};

	class WLD_API CoverReservationPool
	{
	public:
		bool ReserveCover(const glm::vec3& pos, uint32_t ownerEntity, float leaseTime = 5.0f);
		void ReleaseCover(const glm::vec3& pos, uint32_t ownerEntity);
		bool IsCoverReserved(const glm::vec3& pos, uint32_t excludeEntity, float radius = 1.5f) const;
		void Update(float dt);
		void Clear();

		const std::vector<CoverReservation>& GetReservations() const { return m_Reservations; }

	private:
		mutable std::mutex m_Mutex;
		std::vector<CoverReservation> m_Reservations;
	};

	struct WLD_API EnvQueryRequest
	{
		uint32_t RequestId = 0;
		std::shared_ptr<EQSQuery> Query;
		EQSQueryContext Context;
		std::function<void(const EQSQueryResult&)> Callback;
		float Priority = 1.0f;
	};

	class WLD_API EnvQueryManager
	{
	public:
		static EnvQueryManager& Get();

		uint32_t RequestQuery(
			std::shared_ptr<EQSQuery> query,
			const EQSQueryContext& ctx,
			std::function<void(const EQSQueryResult&)> callback,
			float priority = 1.0f);

		void CancelRequest(uint32_t requestId);
		void Clear();

		// 单帧时间预算控制更新 (默认预算 0.5ms)
		void Update(float dt, float maxBudgetMs = 0.5f);

		CoverReservationPool& GetReservationPool() { return m_ReservationPool; }
		const CoverReservationPool& GetReservationPool() const { return m_ReservationPool; }

		size_t GetPendingCount() const;

	private:
		EnvQueryManager() = default;
		~EnvQueryManager() = default;

		mutable std::mutex m_Mutex;
		std::deque<EnvQueryRequest> m_PendingQueue;
		CoverReservationPool m_ReservationPool;
		uint32_t m_NextRequestId = 1;
	};
}
