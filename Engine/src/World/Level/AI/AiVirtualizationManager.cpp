#include "wldpch.h"
#include "World/Level/AI/AiVirtualizationManager.h"
#include <algorithm>
#include <vector>

namespace World::Level::AI
{
	struct AgentEntry
	{
		uint64_t EntityUUID{ 0 };
		uint64_t HomeChunkID{ 0 };
		glm::vec3 Position{ 0.0f };
		float Health{ 100.0f };
		uint32_t Waypoint{ 0 };
		AiLodTier Tier{ AiLodTier::Tier1_Active };
	};

	struct AiVirtualizationManager::Impl
	{
		std::vector<AgentEntry> Agents;
		std::vector<AiVirtualSnapshot> Snapshots;
	};

	AiVirtualizationManager::AiVirtualizationManager()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	AiVirtualizationManager::~AiVirtualizationManager() = default;

	void AiVirtualizationManager::RegisterAi(uint64_t entityUuid, uint64_t homeChunkId, const glm::vec3& pos, float health, uint32_t waypoint)
	{
		auto it = std::find_if(m_Impl->Agents.begin(), m_Impl->Agents.end(), [entityUuid](const AgentEntry& a) {
			return a.EntityUUID == entityUuid;
		});

		if (it != m_Impl->Agents.end())
		{
			it->Position = pos;
			it->Health = health;
			it->Waypoint = waypoint;
		}
		else
		{
			m_Impl->Agents.push_back({ entityUuid, homeChunkId, pos, health, waypoint, AiLodTier::Tier1_Active });
		}
	}

	void AiVirtualizationManager::UnregisterAi(uint64_t entityUuid)
	{
		m_Impl->Agents.erase(
			std::remove_if(m_Impl->Agents.begin(), m_Impl->Agents.end(), [entityUuid](const AgentEntry& a) {
				return a.EntityUUID == entityUuid;
			}),
			m_Impl->Agents.end());
	}

	void AiVirtualizationManager::UpdateLOD(const glm::vec3& playerPos, float activeRadius, float hibernateRadius)
	{
		for (auto& agent : m_Impl->Agents)
		{
			const float dist = glm::distance(playerPos, agent.Position);
			if (dist <= activeRadius)
			{
				agent.Tier = AiLodTier::Tier1_Active;
			}
			else if (dist <= hibernateRadius)
			{
				agent.Tier = AiLodTier::Tier2_Hibernated;
			}
			else
			{
				agent.Tier = AiLodTier::Tier3_Virtual;
			}
		}
	}

	AiLodTier AiVirtualizationManager::GetAiTier(uint64_t entityUuid) const
	{
		for (const auto& agent : m_Impl->Agents)
		{
			if (agent.EntityUUID == entityUuid)
			{
				return agent.Tier;
			}
		}
		return AiLodTier::Tier3_Virtual;
	}

	void AiVirtualizationManager::OnChunkUnloaded(uint64_t chunkId, DeltaStateRegistry& deltaState)
	{
		for (const auto& agent : m_Impl->Agents)
		{
			if (agent.HomeChunkID == chunkId)
			{
				AiVirtualSnapshot snap;
				snap.EntityUUID = agent.EntityUUID;
				snap.HomeChunkID = agent.HomeChunkID;
				snap.Position = agent.Position;
				snap.Health = agent.Health;
				snap.CurrentWaypointIndex = agent.Waypoint;
				snap.AlertLevel = 0;

				m_Impl->Snapshots.push_back(snap);

				deltaState.SetProperty(agent.EntityUUID, "AI_Health", std::to_string(agent.Health));
				deltaState.SetProperty(agent.EntityUUID, "AI_Waypoint", std::to_string(agent.Waypoint));
			}
		}

		m_Impl->Agents.erase(
			std::remove_if(m_Impl->Agents.begin(), m_Impl->Agents.end(), [chunkId](const AgentEntry& a) {
				return a.HomeChunkID == chunkId;
			}),
			m_Impl->Agents.end());
	}

	void AiVirtualizationManager::OnChunkLoaded(uint64_t chunkId, const DeltaStateRegistry& deltaState, std::vector<AiVirtualSnapshot>& outRestoredAi)
	{
		std::vector<AiVirtualSnapshot> toRestore;

		for (auto it = m_Impl->Snapshots.begin(); it != m_Impl->Snapshots.end();)
		{
			if (it->HomeChunkID == chunkId)
			{
				if (deltaState.HasProperty(it->EntityUUID, "AI_Health"))
				{
					it->Health = std::stof(deltaState.GetProperty(it->EntityUUID, "AI_Health"));
				}
				if (deltaState.HasProperty(it->EntityUUID, "AI_Waypoint"))
				{
					it->CurrentWaypointIndex = static_cast<uint32_t>(std::stoul(deltaState.GetProperty(it->EntityUUID, "AI_Waypoint")));
				}

				toRestore.push_back(*it);
				RegisterAi(it->EntityUUID, it->HomeChunkID, it->Position, it->Health, it->CurrentWaypointIndex);
				it = m_Impl->Snapshots.erase(it);
			}
			else
			{
				++it;
			}
		}

		outRestoredAi = std::move(toRestore);
	}

	size_t AiVirtualizationManager::GetTrackedAiCount() const
	{
		return m_Impl->Agents.size();
	}
}
