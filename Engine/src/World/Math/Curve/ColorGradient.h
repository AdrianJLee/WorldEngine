#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace World
{
	struct ColorStop
	{
		float Time = 0.0f;
		glm::vec3 Color { 1.0f };

		ColorStop() = default;
		ColorStop(float time, const glm::vec3& color)
			: Time(time), Color(color) {}
	};

	struct AlphaStop
	{
		float Time = 0.0f;
		float Alpha = 1.0f;

		AlphaStop() = default;
		AlphaStop(float time, float alpha)
			: Time(time), Alpha(alpha) {}
	};

	class WLD_API ColorGradient
	{
	public:
		ColorGradient() = default;
		~ColorGradient() = default;

		void AddColorStop(float time, const glm::vec3& color);
		void AddAlphaStop(float time, float alpha);
		void Clear();

		size_t ColorStopCount() const { return m_ColorStops.size(); }
		size_t AlphaStopCount() const { return m_AlphaStops.size(); }
		const std::vector<ColorStop>& GetColorStops() const { return m_ColorStops; }
		const std::vector<AlphaStop>& GetAlphaStops() const { return m_AlphaStops; }

		glm::vec4 Evaluate(float time) const;
		void PrecomputeLut(size_t sampleCount = 256);
		glm::vec4 EvaluateFastLut(float normalizedT) const;
		const std::vector<glm::vec4>& GetLut() const { return m_Lut; }

		std::string SerializeToYaml() const;
		bool DeserializeFromYaml(const std::string& yaml);
		bool SaveToFile(const std::string& filepath) const;
		bool LoadFromFile(const std::string& filepath);

		static ColorGradient DefaultFire();
		static ColorGradient DefaultSmoke();

	private:
		std::vector<ColorStop> m_ColorStops;
		std::vector<AlphaStop> m_AlphaStops;
		std::vector<glm::vec4> m_Lut;
	};
}
