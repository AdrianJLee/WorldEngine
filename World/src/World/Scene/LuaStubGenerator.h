#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	struct LuaTypeReflection;

	class LuaStubGenerator
	{
	public:
		// Pure rendering: neither the registry, output nor filesystem changes on failure.
		static bool Render(const std::vector<LuaTypeReflection>& types, std::string& output, std::string& error);

		// A successful unchanged generation leaves the destination timestamp intact.
		// A failed generation retains the last valid destination and reports its path.
		static bool Generate(const std::filesystem::path& outputPath, std::string& error);
		static bool Generate(const std::filesystem::path& outputPath, const std::vector<LuaTypeReflection>& types, std::string& error);
	};
}
