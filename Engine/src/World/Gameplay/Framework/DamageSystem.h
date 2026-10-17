#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>

namespace World::Gameplay
{
	// 伤害减免计算模式
	enum class MitigationMode : uint8_t
	{
		Hyperbolic = 0, // 双曲减伤 (LoL/WC3: 100 / (100 + Armor))
		Flat = 1,       // 固定减免 (RPG/D&D: max(1.0, RawDamage - Armor))
		Percentage = 2, // 百分比减免 (RawDamage * (1 - clamp(Armor, 0, 100) / 100))
		Custom = 3      // 动态自定义求解器
	};

	// 伤害行为标志位
	struct DamageFlags
	{
		static constexpr uint32_t None = 0;
		static constexpr uint32_t IgnoreArmor = 1 << 0;          // 穿透护甲
		static constexpr uint32_t IgnoreShield = 1 << 1;         // 穿透护盾
		static constexpr uint32_t IgnoreInvulnerability = 1 << 2;// 穿透无敌帧
	};

	// 经典伤害类型枚举 (保留向后兼容)
	enum class DamageType : uint8_t
	{
		Physical = 0,
		Fire,
		TrueDamage,
		Custom
	};

	// 伤害事件请求输入
	struct DamageEvent
	{
		entt::entity Source = entt::null;
		entt::entity Target = entt::null;
		float Amount = 0.0f;
		DamageType Type = DamageType::Physical;
		NameId Tag;                                     // 标签化伤害类型 (如 Damage.Physical, Damage.Fire 等)
		glm::vec3 HitPoint { 0.0f };
		glm::vec3 HitNormal { 0.0f, 1.0f, 0.0f };
		uint32_t Flags = DamageFlags::None;             // 穿透/行为标志位
	};

	// 受击结算产出事件 (纯净数据，解耦全屏闪白/后处理副作用)
	struct DamageTakenEvent
	{
		entt::entity Target = entt::null;
		entt::entity Source = entt::null;
		float RawDamage = 0.0f;
		float MitigatedDamage = 0.0f;
		float ShieldAbsorbed = 0.0f;
		float HealthDamage = 0.0f;
		DamageType Type = DamageType::Physical;
		NameId Tag;
		glm::vec3 HitPoint { 0.0f };
		glm::vec3 HitNormal { 0.0f, 1.0f, 0.0f };
		uint32_t Flags = DamageFlags::None;
	};

	struct DeathEvent
	{
		entt::entity Entity = entt::null;
		entt::entity Killer = entt::null;
	};

	// 自定义伤害计算策略接口
	using DamageCalculatorFn = std::function<float(const DamageEvent&, const HealthComponent&)>;

	class WLD_API DamageSystem
	{
	public:
		// 核心应用伤害接口 (纯净数据，不触发全屏后处理)
		static float ApplyDamage(Scene& scene, const DamageEvent& event);
		static float Heal(HealthComponent& health, float amount);
		static void Update(Scene& scene, float dt);

		// 事件监听器注册 (由业务系统/主角控制器按需触发闪白、震屏、音效与受击硬直)
		static void SetDamageCallback(std::function<void(const DamageTakenEvent&)> callback);
		static void SetDeathCallback(std::function<void(const DeathEvent&)> callback);

		// 自定义计算策略注入
		static void SetDamageCalculator(DamageCalculatorFn calculator);
		static void ResetDamageCalculator();

		// 多模式减免计算
		static float CalculateMitigatedDamage(float rawDamage, float armor, MitigationMode mode = MitigationMode::Hyperbolic);
		static float CalculateMitigatedDamage(float rawDamage, float armor, DamageType type);
	};
}