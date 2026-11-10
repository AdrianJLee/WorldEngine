#include "wldpch.h"
#include "World/Level/AI/EQS/EnvQueryManager.h"
#include <algorithm>
#include <chrono>

namespace World::EQS
{
	bool CoverReservationPool::ReserveCover(const glm::vec3& pos, uint32_t ownerEntity, float leaseTime)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		// 1. 检查是否有其他实体已经预定了此掩体点位（1.0m 半径内），杜绝抢占
		for (const auto& r : m_Reservations)
		{
			if (r.OwnerEntity != ownerEntity && glm::distance(r.Position, pos) < 1.0f)
			{
				return false; // 已被其他友军预定，拒绝抢占
			}
		}

		// 2. 若自身已持有预定，更新位置与租约
		for (auto& r : m_Reservations)
		{
			if (r.OwnerEntity == ownerEntity)
			{
				r.Position = pos;
				r.RemainingTime = leaseTime;
				return true;
			}
		}

		// 3. 全新预定加入池中
		CoverReservation res;
		res.Position = pos;
		res.OwnerEntity = ownerEntity;
		res.RemainingTime = leaseTime;
		m_Reservations.push_back(res);
		return true;
	}

	void CoverReservationPool::ReleaseCover(const glm::vec3& pos, uint32_t ownerEntity)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Reservations.erase(std::remove_if(m_Reservations.begin(), m_Reservations.end(),
			[&](const CoverReservation& r) {
				return r.OwnerEntity == ownerEntity || glm::distance(r.Position, pos) < 0.5f;
			}), m_Reservations.end());
	}

	bool CoverReservationPool::IsCoverReserved(const glm::vec3& pos, uint32_t excludeEntity, float radius) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (const auto& r : m_Reservations)
		{
			if (r.OwnerEntity != excludeEntity && glm::distance(r.Position, pos) <= radius)
			{
				return true;
			}
		}
		return false;
	}

	void CoverReservationPool::Update(float dt)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (auto it = m_Reservations.begin(); it != m_Reservations.end(); )
		{
			it->RemainingTime -= dt;
			if (it->RemainingTime <= 0.0f)
				it = m_Reservations.erase(it);
			else
				++it;
		}
	}

	void CoverReservationPool::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Reservations.clear();
	}

	EnvQueryManager& EnvQueryManager::Get()
	{
		static EnvQueryManager s_Instance;
		return s_Instance;
	}

	uint32_t EnvQueryManager::RequestQuery(
		std::shared_ptr<EQSQuery> query,
		const EQSQueryContext& ctx,
		std::function<void(const EQSQueryResult&)> callback,
		float priority)
	{
		if (!query)
			return 0;

		std::lock_guard<std::mutex> lock(m_Mutex);
		uint32_t reqId = m_NextRequestId++;
		EnvQueryRequest req;
		req.RequestId = reqId;
		req.Query = std::move(query);
		req.Context = ctx;
		req.Callback = std::move(callback);
		req.Priority = priority;

		m_PendingQueue.push_back(std::move(req));
		return reqId;
	}

	void EnvQueryManager::CancelRequest(uint32_t requestId)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_PendingQueue.erase(std::remove_if(m_PendingQueue.begin(), m_PendingQueue.end(),
			[requestId](const EnvQueryRequest& r) { return r.RequestId == requestId; }), m_PendingQueue.end());
	}

	void EnvQueryManager::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_PendingQueue.clear();
		m_ReservationPool.Clear();
	}

	size_t EnvQueryManager::GetPendingCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_PendingQueue.size();
	}

	void EnvQueryManager::Update(float dt, float maxBudgetMs)
	{
		m_ReservationPool.Update(dt);

		const auto startTime = std::chrono::high_resolution_clock::now();
		const double budgetUs = static_cast<double>(maxBudgetMs) * 1000.0;

		while (true)
		{
			EnvQueryRequest req;
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				if (m_PendingQueue.empty())
					break;
				req = std::move(m_PendingQueue.front());
				m_PendingQueue.pop_front();
			}

			if (req.Query)
			{
				EQSQueryResult result;
				req.Query->Execute(req.Context, result);

				if (result.Success && req.Context.QuerierEntityRaw != 0)
				{
					m_ReservationPool.ReserveCover(result.BestPosition, req.Context.QuerierEntityRaw, 5.0f);
				}

				if (req.Callback)
				{
					req.Callback(result);
				}
			}

			const auto now = std::chrono::high_resolution_clock::now();
			const double elapsedUs = std::chrono::duration<double, std::micro>(now - startTime).count();
			if (elapsedUs >= budgetUs)
			{
				break; // 超出本帧时间预算，保留剩余请求至下一帧，保证高帧率平滑
			}
		}
	}
}
