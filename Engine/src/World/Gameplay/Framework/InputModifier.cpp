#include "wldpch.h"
#include "World/Gameplay/Framework/InputModifier.h"
#include <algorithm>
#include <cmath>

namespace World::Gameplay
{
	InputModifier InputModifier::MakeDeadZone(float lower, float upper, bool radial)
	{
		InputModifier m;
		m.Type = ModifierType::DeadZone;
		m.LowerThreshold = lower;
		m.UpperThreshold = upper;
		m.Radial = radial;
		return m;
	}

	InputModifier InputModifier::MakeInvert(bool x, bool y, bool z)
	{
		InputModifier m;
		m.Type = ModifierType::Invert;
		m.InvertX = x;
		m.InvertY = y;
		m.InvertZ = z;
		return m;
	}

	InputModifier InputModifier::MakeScale(float uniformScale)
	{
		InputModifier m;
		m.Type = ModifierType::Scale;
		m.Scalar = glm::vec3(uniformScale);
		return m;
	}

	InputModifier InputModifier::MakeScale(glm::vec3 scale)
	{
		InputModifier m;
		m.Type = ModifierType::Scale;
		m.Scalar = scale;
		return m;
	}

	InputModifier InputModifier::MakeResponseCurve(float exponent)
	{
		InputModifier m;
		m.Type = ModifierType::ResponseCurve;
		m.Exponent = exponent;
		return m;
	}

	InputModifier InputModifier::MakeSwizzle(int x, int y, int z)
	{
		InputModifier m;
		m.Type = ModifierType::Swizzle;
		m.SwizzleOrder[0] = x;
		m.SwizzleOrder[1] = y;
		m.SwizzleOrder[2] = z;
		return m;
	}

	InputModifier InputModifier::MakeNormalize(float maxLength)
	{
		InputModifier m;
		m.Type = ModifierType::Normalize;
		m.MaxLength = maxLength;
		return m;
	}

	glm::vec3 InputModifier::Apply(const glm::vec3& inValue, float dt) const
	{
		(void)dt;
		glm::vec3 result = inValue;
		switch (Type)
		{
		case ModifierType::DeadZone:
		{
			if (Radial)
			{
				const float length = glm::length(result);
				if (length < LowerThreshold)
					return glm::vec3(0.0f);
				if (length > UpperThreshold)
					return glm::normalize(result) * UpperThreshold;
				// 重映射防止死区边缘阶跃
				const float mapped = (length - LowerThreshold) / (UpperThreshold - LowerThreshold);
				return glm::normalize(result) * mapped;
			}
			else
			{
				auto applyAxis = [this](float val) -> float {
					const float sign = val < 0.0f ? -1.0f : 1.0f;
					const float absVal = std::fabs(val);
					if (absVal < LowerThreshold)
						return 0.0f;
					if (absVal > UpperThreshold)
						return sign * UpperThreshold;
					return sign * ((absVal - LowerThreshold) / (UpperThreshold - LowerThreshold));
				};
				result.x = applyAxis(result.x);
				result.y = applyAxis(result.y);
				result.z = applyAxis(result.z);
			}
			break;
		}
		case ModifierType::Invert:
			if (InvertX) result.x = -result.x;
			if (InvertY) result.y = -result.y;
			if (InvertZ) result.z = -result.z;
			break;
		case ModifierType::Scale:
			result *= Scalar;
			break;
		case ModifierType::ResponseCurve:
		{
			auto applyCurve = [this](float val) -> float {
				const float sign = val < 0.0f ? -1.0f : 1.0f;
				return sign * std::pow(std::fabs(val), Exponent);
			};
			result.x = applyCurve(result.x);
			result.y = applyCurve(result.y);
			result.z = applyCurve(result.z);
			break;
		}
		case ModifierType::Swizzle:
		{
			glm::vec3 src = inValue;
			float coords[3] = { src.x, src.y, src.z };
			result.x = coords[std::clamp(SwizzleOrder[0], 0, 2)];
			result.y = coords[std::clamp(SwizzleOrder[1], 0, 2)];
			result.z = coords[std::clamp(SwizzleOrder[2], 0, 2)];
			break;
		}
		case ModifierType::Normalize:
		{
			const float len = glm::length(result);
			if (len > MaxLength && len > 1e-5f)
				result = (result / len) * MaxLength;
			break;
		}
		}
		return result;
	}
}
