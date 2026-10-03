#pragma once

#include "World/Core/Export.h"

#include <entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>

namespace World::Physics
{
	// P5:物理事实的阶段(ECS 口径,与 Bevy 的 CollisionEvent 同构)。
	//
	//   Begin   —— 本固定步开始接触/重叠
	//   Persist —— 持续接触(2D 由每步接触枚举补齐,3D 走 Jolt 的 OnContactPersisted)
	//   End     —— 本固定步分离(几何量此时已不可靠,一律为零)
	//
	// 契约:**只在固定步长阶段产出、按帧聚簇**。同一段真实时间、同一固定步序列输入,
	// 事件序列(类型/配对/阶段/顺序)必须逐字节可复现 —— 帧率无关。
	enum class ContactPhase : std::uint8_t
	{
		Begin = 0,
		Persist,
		End
	};

	// 一次**实体接触**(非传感器)。几何量取自后端本步的流形:
	//   * Box2D:b2Contact_GetData 的 b2Manifold(normal 指向 shape A → shape B);
	//   * Jolt:ContactManifold(世界系法线 + 第一个接触点)。
	// End 阶段拿不到流形 ⇒ Point/Normal 为零、PenetrationDepth = 0。
	struct ContactEvent
	{
		// 顺序由后端决定(Box2D:shapeA/shapeB;Jolt:body1/body2),
		// 但同一固定步输入下**确定**,所以事件序列可比对。
		entt::entity EntityA = entt::null;
		entt::entity EntityB = entt::null;
		ContactPhase Phase = ContactPhase::Begin;
		glm::vec3 Point { 0.0f };            // 世界系接触点(2D 的 z 恒为 0)
		glm::vec3 Normal { 0.0f };           // 单位法线,指向 A → B
		float PenetrationDepth = 0.0f;       // 穿透深度(多接触点取最深;End 时为 0)
	};

	// **传感器/触发器**重叠:只上报事实,不产生任何碰撞响应。
	// SensorEntity 是挂了 IsSensor 的那个实体(2D 逐 shape / 3D 逐刚体,见文档)。
	struct TriggerEvent
	{
		entt::entity SensorEntity = entt::null;
		entt::entity OtherEntity = entt::null;
		ContactPhase Phase = ContactPhase::Begin;
	};

	// 实体句柄 ↔ 后端句柄的打包约定(两端必须一致;实体索引 0 是合法值,
	// 所以 +1 偏移以便用 nullptr 表示"没有实体")。
	inline void* PackEntityUserData(entt::entity entity)
	{
		return reinterpret_cast<void*>(static_cast<std::uintptr_t>(entt::to_integral(entity)) + 1u);
	}

	inline entt::entity UnpackEntityUserData(const void* userData)
	{
		if (userData == nullptr)
			return entt::null;
		return static_cast<entt::entity>(reinterpret_cast<std::uintptr_t>(userData) - 1u);
	}
}
