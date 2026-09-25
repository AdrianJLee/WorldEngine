#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureImportSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	// M4-TEX P1:纹理烘焙(内核)—— 源图 + 设置 → `.wtexc`。
	//
	// 纯函数式核心(`BakeBytes`):同输入两次产出**逐字节一致**(测试锁死);
	// 目录入口(`BakeDirectory`)只做遍历 / sidecar 读取 / 缓存复用 / 原子写盘。
	struct WLD_API TextureBakeStats
	{
		size_t Baked = 0;      // 真烘了
		size_t UpToDate = 0;   // 缓存命中(源与设置都没变)
		size_t Skipped = 0;    // 扩展名不支持 / 空文件
		size_t Failed = 0;     // 出错(错误文本进 Errors)
		std::vector<std::string> Errors;
	};

	struct WLD_API TextureBakeOptions
	{
		bool Force = false;    // 忽略缓存,全部重烘
		bool Verbose = false;  // 每条产物的日志(默认只在出错时记)
	};

	class WLD_API TextureCompiler
	{
	public:
		// 源字节 → 产物字节(含头)。失败填 error(人话)。
		static bool BakeBytes(const std::vector<uint8_t>& sourceBytes,
			const TextureImportSettings& settings, std::vector<uint8_t>& outArtifact,
			TextureArtifactHeader& outHeader, std::string& error);

		// 源文件 → 产物字节(读文件 + 计算源 sha256)。
		static bool BakeFile(const std::filesystem::path& sourceFile,
			const TextureImportSettings& settings, std::vector<uint8_t>& outArtifact,
			TextureArtifactHeader& outHeader, std::string& error);

	// 目录烘焙:<sourceRoot> 下所有源图 → <outputRoot>/<逻辑路径>.wtexc。
	// sidecar 与源同目录(<源完整名>.wtex);cacheDir 非空时按 "源 sha256 + 设置 hash" 复用。
		static TextureBakeStats BakeDirectory(const std::filesystem::path& sourceRoot,
			const std::filesystem::path& outputRoot, const std::filesystem::path& cacheDir,
			const TextureBakeOptions& options);

		// 源图扩展名(小写,含点):.png/.jpg/.jpeg/.tga/.bmp。
		static bool IsTextureSourceExtension(const std::string& extension);
	};
}
