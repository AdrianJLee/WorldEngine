#pragma once

#include <string>
#include <vector>

namespace World::Gen
{
	struct StyleLock
	{
		std::string promptTemplate = "{prompt}, 16-bit pixel art, consistent palette";
		std::string negativePrompt = "blurry, low resolution, realistic photo, 3d render";
		std::vector<std::string> palette = { "#1a1c23", "#0eaf9b", "#f79617", "#e43b44" };
		int seedRangeMin = 10000;
		int seedRangeMax = 99999;
		std::string referenceHash;

		std::string Apply(const std::string& userPrompt) const;
		std::string ToYaml() const;
		static StyleLock FromYaml(const std::string& yamlStr);

		static StyleLock Load(const std::string& projectRoot);
		static bool Save(const std::string& projectRoot, const StyleLock& lock);
	};
}
