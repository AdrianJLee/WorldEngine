#include "wldpch.h"
#include "World/Foliage/FoliageInteraction.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	namespace
	{
		constexpr float kCellSize = 0.75f;

		struct CellKey
		{
			int32_t x { 0 };
			int32_t z { 0 };

			bool operator==(const CellKey& o) const
			{
				return x == o.x && z == o.z;
			}
		};

		struct CellKeyHash
		{
			size_t operator()(const CellKey& k) const noexcept
			{
				const uint32_t ux = static_cast<uint32_t>(k.x);
				const uint32_t uz = static_cast<uint32_t>(k.z);
				return (static_cast<size_t>(ux) * 73856093u) ^ (static_cast<size_t>(uz) * 19349663u);
			}
		};

		struct FieldCell
		{
			glm::vec3 Displacement { 0.0f };
			float RecoverySpeed { 3.0f };
			float TrackHoldTimer { 0.0f };
			float GroundY { 0.0f };
			bool TouchedThisFrame { false };
		};

		static std::unordered_map<CellKey, FieldCell, CellKeyHash> s_FieldCells;
		static std::unordered_map<uint64_t, glm::vec3> s_InteractorPrevPos;
		static std::vector<FoliageInteractorSource> s_ActiveInteractors;
		static float s_CurrentDt = 1.0f / 60.0f;
	}

	void FoliageInteractionSystem::ComputePushDisplacement(
		const glm::vec3& foliagePos,
		const glm::vec3& pusherPos,
		const glm::vec3& pusherVelocity,
		float pushRadius,
		float pushStrength,
		glm::vec3& outDisplacement)
	{
		const glm::vec3 delta = foliagePos - pusherPos;
		const float dist2D = std::sqrt(delta.x * delta.x + delta.z * delta.z);

		if (dist2D >= pushRadius || pushRadius <= 0.001f)
		{
			outDisplacement = glm::vec3(0.0f);
			return;
		}

		const float factor = 1.0f - (dist2D / pushRadius);
		glm::vec3 pushDir = (dist2D > 0.001f) ? glm::normalize(glm::vec3(delta.x, 0.0f, delta.z)) : glm::vec3(1.0f, 0.0f, 0.0f);

		// 融合速度动力学冲击
		if (glm::length(pusherVelocity) > 0.01f)
		{
			pushDir = glm::normalize(pushDir + glm::normalize(pusherVelocity) * 0.5f);
		}

		outDisplacement = pushDir * (factor * pushStrength);
	}

	void FoliageInteractionSystem::UpdateSpringRecovery(
		glm::vec3& displacement,
		float recoverySpeed,
		float dt)
	{
		if (glm::length(displacement) < 1e-4f)
		{
			displacement = glm::vec3(0.0f);
			return;
		}

		// 阻尼指数衰减回弹至原位
		const float decay = std::exp(-recoverySpeed * dt);
		displacement *= decay;
	}

	void FoliageInteractionSystem::BeginFrame(float dt)
	{
		s_CurrentDt = std::max(1e-4f, dt);
		s_ActiveInteractors.clear();
		for (auto& [key, cell] : s_FieldCells)
		{
			cell.TouchedThisFrame = false;
		}
	}

	void FoliageInteractionSystem::SubmitInteractor(const FoliageInteractorSource& inSource)
	{
		FoliageInteractorSource source = inSource;

		if (source.SourceId != 0)
		{
			auto it = s_InteractorPrevPos.find(source.SourceId);
			if (it != s_InteractorPrevPos.end())
			{
				if (glm::length(source.Velocity) < 0.001f)
				{
					glm::vec3 computedVel = (source.Position - it->second) / std::max(s_CurrentDt, 0.001f);
					const float speed = glm::length(computedVel);
					if (speed > 25.0f)
						computedVel = (computedVel / speed) * 25.0f;
					source.Velocity = computedVel;
				}
				it->second = source.Position;
			}
			else
			{
				s_InteractorPrevPos[source.SourceId] = source.Position;
			}
		}

		s_ActiveInteractors.push_back(source);

		if (source.PushRadius <= 0.001f || source.PushStrength <= 0.0f)
			return;

		const int minX = static_cast<int>(std::floor((source.Position.x - source.PushRadius) / kCellSize));
		const int maxX = static_cast<int>(std::ceil((source.Position.x + source.PushRadius) / kCellSize));
		const int minZ = static_cast<int>(std::floor((source.Position.z - source.PushRadius) / kCellSize));
		const int maxZ = static_cast<int>(std::ceil((source.Position.z + source.PushRadius) / kCellSize));

		for (int z = minZ; z <= maxZ; ++z)
		{
			for (int x = minX; x <= maxX; ++x)
			{
				const glm::vec3 cellCenter(
					(static_cast<float>(x) + 0.5f) * kCellSize,
					source.Position.y,
					(static_cast<float>(z) + 0.5f) * kCellSize);

				glm::vec3 targetDisp;
				ComputePushDisplacement(cellCenter, source.Position, source.Velocity, source.PushRadius, source.PushStrength, targetDisp);

				if (glm::length(targetDisp) > 1e-4f)
				{
					CellKey key { x, z };
					auto& cell = s_FieldCells[key];
					cell.RecoverySpeed = source.RecoverySpeed;
					cell.GroundY = source.Position.y;
					cell.TouchedThisFrame = true;
					if (source.LeaveTracks)
					{
						cell.TrackHoldTimer = 1.2f;
					}

					if (glm::length(targetDisp) > glm::length(cell.Displacement))
					{
						cell.Displacement = glm::mix(cell.Displacement, targetDisp, 0.45f);
					}
					else
					{
						cell.Displacement = glm::mix(cell.Displacement, targetDisp, 0.20f);
					}
				}
			}
		}
	}

	void FoliageInteractionSystem::TriggerImpulse(const glm::vec3& worldPos, float radius, float strength, float recoverySpeed)
	{
		FoliageInteractorSource src;
		src.SourceId = 0;
		src.Position = worldPos;
		src.Velocity = glm::vec3(0.0f);
		src.PushRadius = radius;
		src.PushStrength = strength;
		src.RecoverySpeed = recoverySpeed;
		src.LeaveTracks = false;
		SubmitInteractor(src);
	}

	void FoliageInteractionSystem::StepSimulation(float dt)
	{
		const float stepDt = std::max(1e-4f, dt);

		for (auto it = s_FieldCells.begin(); it != s_FieldCells.end(); )
		{
			auto& cell = it->second;
			if (!cell.TouchedThisFrame)
			{
				if (cell.TrackHoldTimer > 0.0f)
				{
					cell.TrackHoldTimer -= stepDt;
					UpdateSpringRecovery(cell.Displacement, cell.RecoverySpeed * 0.25f, stepDt);
				}
				else
				{
					UpdateSpringRecovery(cell.Displacement, cell.RecoverySpeed, stepDt);
				}
			}

			if (glm::length(cell.Displacement) < 1e-3f)
			{
				it = s_FieldCells.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	glm::vec3 FoliageInteractionSystem::SampleFieldDisplacement(const glm::vec3& foliageWorldPos)
	{
		glm::vec3 fieldDisp(0.0f);

		if (!s_FieldCells.empty())
		{
			const float fx = foliageWorldPos.x / kCellSize - 0.5f;
			const float fz = foliageWorldPos.z / kCellSize - 0.5f;
			const int32_t x0 = static_cast<int32_t>(std::floor(fx));
			const int32_t z0 = static_cast<int32_t>(std::floor(fz));
			const int32_t x1 = x0 + 1;
			const int32_t z1 = z0 + 1;

			const float tx = fx - static_cast<float>(x0);
			const float tz = fz - static_cast<float>(z0);

			auto getCellDisp = [](int32_t cx, int32_t cz) -> glm::vec3 {
				CellKey k { cx, cz };
				auto it = s_FieldCells.find(k);
				return (it != s_FieldCells.end()) ? it->second.Displacement : glm::vec3(0.0f);
			};

			const glm::vec3 d00 = getCellDisp(x0, z0);
			const glm::vec3 d10 = getCellDisp(x1, z0);
			const glm::vec3 d01 = getCellDisp(x0, z1);
			const glm::vec3 d11 = getCellDisp(x1, z1);

			const glm::vec3 d0 = glm::mix(d00, d10, tx);
			const glm::vec3 d1 = glm::mix(d01, d11, tx);
			fieldDisp = glm::mix(d0, d1, tz);
		}

		// 叠加当前帧活跃交互源即时解析位移 (零延迟丝滑响应)
		for (const auto& src : s_ActiveInteractors)
		{
			glm::vec3 directDisp;
			ComputePushDisplacement(foliageWorldPos, src.Position, src.Velocity, src.PushRadius, src.PushStrength, directDisp);
			if (glm::length(directDisp) > glm::length(fieldDisp))
			{
				fieldDisp = directDisp;
			}
		}

		return fieldDisp;
	}

	glm::mat4 FoliageInteractionSystem::ApplyPivotBendTransform(
		const glm::mat4& baseInstanceTransform,
		const glm::vec3& bendDisplacement,
		float stiffness)
	{
		const glm::vec3 rootPos = glm::vec3(baseInstanceTransform[3]);
		const float scaleY = std::max(0.3f, glm::length(glm::vec3(baseInstanceTransform[1])));

		const float flex = 1.0f / (1.0f + std::max(0.0f, stiffness) * 0.6f);
		const glm::vec3 effDisp = glm::vec3(bendDisplacement.x, 0.0f, bendDisplacement.z) * flex;
		const float dispLen = glm::length(effDisp);

		if (dispLen < 1e-4f)
			return baseInstanceTransform;

		const float bendAngle = std::min(std::atan2(dispLen, scaleY), 1.15f);
		const glm::vec3 bendAxis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), effDisp));

		glm::mat4 result = baseInstanceTransform;
		result[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), bendAngle, bendAxis);
		result = rot * result;

		// 模拟压扁形变
		const float flatten = std::max(0.65f, std::cos(bendAngle * 0.5f));
		result[1] *= flatten;

		// 恢复根部平移
		result[3] = glm::vec4(rootPos, 1.0f);
		return result;
	}

	size_t FoliageInteractionSystem::GetActiveCellCount()
	{
		return s_FieldCells.size();
	}

	void FoliageInteractionSystem::ClearState()
	{
		s_FieldCells.clear();
		s_InteractorPrevPos.clear();
		s_ActiveInteractors.clear();
	}
}
