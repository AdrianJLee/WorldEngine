#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <cstddef>

namespace World
{
	struct FoliageInteractorSource
	{
		uint64_t SourceId { 0 };
		glm::vec3 Position { 0.0f };
		glm::vec3 Velocity { 0.0f };
		float PushRadius { 1.5f };
		float PushStrength { 1.0f };
		float RecoverySpeed { 3.0f };
		bool LeaveTracks { true };
	};

	class WLD_API FoliageInteractionSystem
	{
	public:
		static void ComputePushDisplacement(
			const glm::vec3& foliagePos,
			const glm::vec3& pusherPos,
			const glm::vec3& pusherVelocity,
			float pushRadius,
			float pushStrength,
			glm::vec3& outDisplacement);

		static void UpdateSpringRecovery(
			glm::vec3& displacement,
			float recoverySpeed,
			float dt);

		// 有状态空间交互场接口
		static void BeginFrame(float dt);
		static void SubmitInteractor(const FoliageInteractorSource& source);
		static void TriggerImpulse(const glm::vec3& worldPos, float radius, float strength, float recoverySpeed = 3.0f);
		static void StepSimulation(float dt);

		// 采样任意世界坐标处的合成弯折位移向量，并生成绕植物根部支点(Root Pivot)的保长弯曲变换矩阵
		static glm::vec3 SampleFieldDisplacement(const glm::vec3& foliageWorldPos);
		static glm::mat4 ApplyPivotBendTransform(const glm::mat4& baseInstanceTransform, const glm::vec3& bendDisplacement, float stiffness = 0.5f);
		static size_t GetActiveCellCount();
		static void ClearState();
	};
}
