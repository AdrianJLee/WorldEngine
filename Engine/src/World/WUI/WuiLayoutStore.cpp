#include "wldpch.h"
#include "World/WUI/WuiLayoutStore.h"

#include <fstream>
#include <iterator>

namespace World::Wui
{
	bool WuiLayoutStore::Save(const std::filesystem::path& path, const DockLayout& layout, std::string* error)
	{
		try
		{
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path());
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error) *error = "cannot open layout file for writing: " + path.string();
				return false;
			}
			stream << layout.Serialize();
			return stream.good();
		}
		catch (const std::exception& exception)
		{
			if (error) *error = exception.what();
			return false;
		}
	}

	bool WuiLayoutStore::Load(const std::filesystem::path& path, const DockLayout& fallback, DockLayout* out, std::string* error)
	{
		if (!std::filesystem::exists(path))
		{
			*out = fallback;
			return true;
		}
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			*out = fallback;
			return true;
		}
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		DockLayout parsed;
		if (!DockLayout::Deserialize(text, &parsed, error))
		{
			*out = fallback;
			return false;
		}
		*out = std::move(parsed);
		return true;
	}
}
