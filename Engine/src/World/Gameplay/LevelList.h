#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Gameplay
{
	// 关卡清单条目(P2a W2):关卡是"可被加载/卸载的一段世界"。
	// 场景内容仍在 .wd 里;清单只描述 id、显示名、场景资产与依赖包。
	struct LevelEntry
	{
		std::string Id;
		std::string DisplayName;
		std::string ScenePath;
		std::vector<std::string> Packages;
	};

	// 关卡清单资产(levels.welevel,YAML):与 project.we.yaml 同级放置,
	// 策划/Mod 可覆盖(后续 P4 的覆盖层只需替换该文件)。
	class WLD_API LevelList
	{
	public:
		static bool Load(const std::filesystem::path& path, LevelList* out, std::string* error = nullptr);
		static bool Save(const std::filesystem::path& path, const LevelList& list, std::string* error = nullptr);

		const std::vector<LevelEntry>& Entries() const { return m_Entries; }
		std::vector<LevelEntry>& Entries() { return m_Entries; }
		const LevelEntry* Find(const std::string& id) const;
		bool IsEmpty() const { return m_Entries.empty(); }

	private:
		std::vector<LevelEntry> m_Entries;
	};
}
