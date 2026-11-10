#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>

namespace World
{
	namespace CharacterCollisionFlags
	{
		constexpr uint8_t None  = 0;
		constexpr uint8_t Sides = 1 << 0;
		constexpr uint8_t Above = 1 << 1;
		constexpr uint8_t Below = 1 << 2;
	}

	class WLD_API CharacterControllerSystem
	{
	public:
		// 每帧更新: 倒计时土狼时间与跳跃预输入时间, 计算重力加速度
		static void Update(Scene& scene, float dt);

		// 核心运动学投影位移算法 Move-and-Slide
		// 根据期望位移在障碍物接触面切线做投影滑行, 包含陡坡下滑与台阶处理
		// 返回实际移动产生的位移
		static glm::vec3 MoveAndSlide(Scene& scene, entt::entity entity, const glm::vec3& displacement, float dt);

		// 跳跃请求 (考虑土狼时间与跳跃缓冲)
		// 若当前处于土狼时间内或直接在地面上, 立即起跳 (返回 true); 否则存入跳跃预输入缓冲 (返回 false)
		static bool TryJump(CharacterControllerComponent& kcc, float jumpSpeed);

		// 判定当前是否可以起跳 (地面或土狼时间内)
		static bool CanJump(const CharacterControllerComponent& kcc);

		// 强制触发跳跃 (立即修改速度)
		static void ForceJump(CharacterControllerComponent& kcc, float jumpSpeed);

		// 工具: 切线投影计算 (根据接触法线投影出滑行向量, 支持 2D 与 3D)
		static glm::vec3 ProjectTangent(const glm::vec3& vector, const glm::vec3& normal);

		// 工具: 双接触面交线投影 (消除两相交接触面卡死)
		static glm::vec3 ProjectCrease(const glm::vec3& vector, const glm::vec3& n1, const glm::vec3& n2);
	};
}
