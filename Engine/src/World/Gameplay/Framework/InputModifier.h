#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World::Gameplay
{
	enum class ModifierType : uint8_t
	{
		DeadZone = 0,
		Invert,
		Scale,
		ResponseCurve,
		Swizzle,
		Normalize
	};

	struct WLD_API InputModifier
	{
		ModifierType Type = ModifierType::DeadZone;

		// DeadZone 参数
		float LowerThreshold = 0.15f;
		float UpperThreshold = 1.0f;
		bool Radial = false;

		// Invert 参数
		bool InvertX = false;
		bool InvertY = false;
		bool InvertZ = false;

		// Scale 参数
		glm::vec3 Scalar{ 1.0f, 1.0f, 1.0f };

		// ResponseCurve 参数 (指数/幂次)
		float Exponent = 1.0f;

		// Swizzle 顺序 (0:X, 1:Y, 2:Z)
		int SwizzleOrder[3] = { 0, 1, 2 };

		// Normalize 最大长度
		float MaxLength = 1.0f;

		glm::vec3 Apply(const glm::vec3& inValue, float dt = 0.0f) const;

		// 静态快捷构造器
		static InputModifier MakeDeadZone(float lower = 0.15f, float upper = 1.0f, bool radial = false);
		static InputModifier MakeInvert(bool x = true, bool y = true, bool z = false);
		static InputModifier MakeScale(float uniformScale);
		static InputModifier MakeScale(glm::vec3 scale);
		static InputModifier MakeResponseCurve(float exponent);
		static InputModifier MakeSwizzle(int x, int y, int z = 2);
		static InputModifier MakeNormalize(float maxLength = 1.0f);
	};
}
