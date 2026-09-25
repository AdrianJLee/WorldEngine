// M4-TEX P2:运行时消费烘焙产物(产物优先加载 / 回退源图 / 坏产物保护 / BC5 法线开关)。
//
// 这一套是**无设备**回归:只覆盖 CPU 侧与源码生成侧(真正的上传/渲染在编辑器 e2e 探针里验)。
//  - LoadTextureAsset:命中 <path>.wtexc → FromArtifact + 逐 mip 数据切片;没有/坏产物 → stb 回退;
//  - 包装层 BC5 开关:默认与 RGBA8 时代同源,打开后多一条 Z 重建 + 一条 #define。

#include "World/Core/Core.h"
#include "World/Renderer/MaterialSurface.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureCompiler.h"
#include "World/Renderer/TextureData.h"
#include "World/Renderer/TextureImportSettings.h"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace World;

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	struct TempDir
	{
		std::filesystem::path path;

		TempDir()
		{
			std::error_code ec;
			const uint64_t unique = static_cast<uint64_t>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			path = std::filesystem::temp_directory_path(ec) / ("we-texruntime-" + std::to_string(unique));
			std::filesystem::create_directories(path, ec);
			if (ec)
				throw std::runtime_error("failed to create temp dir: " + ec.message());
		}
		~TempDir()
		{
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	};

	void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	}

	// 未压缩 32 位 TGA(顶左原点):与 World.TextureImport 用同一份最小源图写法。
	std::vector<uint8_t> MakeTga(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
	{
		if (rgba.size() != static_cast<size_t>(width) * height * 4)
			throw std::runtime_error("MakeTga: pixel buffer size mismatch");
		std::vector<uint8_t> bytes(18 + rgba.size(), 0);
		bytes[2] = 2;
		bytes[12] = static_cast<uint8_t>(width & 0xFF);
		bytes[13] = static_cast<uint8_t>((width >> 8) & 0xFF);
		bytes[14] = static_cast<uint8_t>(height & 0xFF);
		bytes[15] = static_cast<uint8_t>((height >> 8) & 0xFF);
		bytes[16] = 32;
		bytes[17] = 0x28;
		for (size_t index = 0; index < rgba.size(); index += 4)
		{
			bytes[18 + index + 0] = rgba[index + 2];
			bytes[18 + index + 1] = rgba[index + 1];
			bytes[18 + index + 2] = rgba[index + 0];
			bytes[18 + index + 3] = rgba[index + 3];
		}
		return bytes;
	}

	std::vector<uint8_t> MakeGradient(uint32_t width, uint32_t height)
	{
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
		for (uint32_t y = 0; y < height; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				uint8_t* texel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
				texel[0] = static_cast<uint8_t>(x * 255 / std::max(1u, width - 1));
				texel[1] = static_cast<uint8_t>(y * 255 / std::max(1u, height - 1));
				texel[2] = static_cast<uint8_t>((x + y) * 255 / std::max(1u, width + height - 2));
				texel[3] = 255;
			}
		}
		return pixels;
	}

	bool Contains(const std::string& text, const char* needle)
	{
		return text.find(needle) != std::string::npos;
	}

	class ScopedEnv
	{
	public:
		ScopedEnv(const char* name, const char* value)
			: m_Name(name)
		{
#ifdef _WIN32
			_putenv_s(m_Name.c_str(), value ? value : "");
#else
			if (value)
				setenv(m_Name.c_str(), value, 1);
			else
				unsetenv(m_Name.c_str());
#endif
		}
		~ScopedEnv()
		{
#ifdef _WIN32
			_putenv_s(m_Name.c_str(), "");
#else
			unsetenv(m_Name.c_str());
#endif
		}
		ScopedEnv(const ScopedEnv&) = delete;
		ScopedEnv& operator=(const ScopedEnv&) = delete;

	private:
		std::string m_Name;
	};

	// 探针入口(--bake-fixture …):用**同一套内核调用**把一份源图烘成 `.wtexc`,
	// 供运行时 e2e 在开发内容根里铺夹具(否则只能靠 P3 的 cook 产物手工搬运)。
	// 它不参与上面的断言;测试本体是无参数运行。
	int BakeFixture(int argc, char** argv)
	{
		if (argc < 5)
		{
			std::fprintf(stderr, "usage: %s --bake-fixture <source> <color|normal|data|ui> <out.wtexc>\n",
				argv[0]);
			return 2;
		}
		const std::string usage = argv[3];
		TextureImportSettings settings;
		if (usage == "color") settings.Usage = TextureUsage::Color;
		else if (usage == "normal") settings.Usage = TextureUsage::Normal;
		else if (usage == "data") settings.Usage = TextureUsage::Data;
		else if (usage == "ui") settings.Usage = TextureUsage::Ui;
		else
		{
			std::fprintf(stderr, "unknown usage '%s'\n", usage.c_str());
			return 2;
		}

		std::ifstream input(argv[2], std::ios::binary | std::ios::ate);
		if (!input)
		{
			std::fprintf(stderr, "cannot read source '%s'\n", argv[2]);
			return 1;
		}
		const std::streamoff size = input.tellg();
		std::vector<uint8_t> sourceBytes(static_cast<size_t>(size));
		input.seekg(0);
		input.read(reinterpret_cast<char*>(sourceBytes.data()), size);

		std::vector<uint8_t> artifact;
		TextureArtifactHeader header;
		std::string error;
		if (!TextureCompiler::BakeBytes(sourceBytes, settings, artifact, header, error))
		{
			std::fprintf(stderr, "bake failed: %s\n", error.c_str());
			return 1;
		}
		WriteFile(argv[4], artifact);
		std::printf("baked format=%s %ux%u mips=%u bytes=%zu srgb=%d\n",
			TextureBlockFormatName(header.Format), header.Width, header.Height, header.MipCount,
			artifact.size(), header.Srgb ? 1 : 0);
		return 0;
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc >= 2 && std::strcmp(argv[1], "--bake-fixture") == 0)
			return BakeFixture(argc, argv);

		TempDir temp;
		const std::filesystem::path sourcePath = temp.path / "textures" / "T.tga";
		// 契约命名 = `<同目录>/<主名>.wtexc`(P6b 前叫 `<源图全名>.wtexc`,只作容错候选)。
		const std::filesystem::path artifactPath = temp.path / "textures" / "T.wtexc";
		const std::vector<uint8_t> sourceBytes = MakeTga(8, 8, MakeGradient(8, 8));
		WriteFile(sourcePath, sourceBytes);

		TextureImportSettings settings;   // 默认:color = BC7 + mip + sRGB
		std::vector<uint8_t> artifactBytes;
		TextureArtifactHeader bakedHeader;
		std::string error;
		CHECK(TextureCompiler::BakeBytes(sourceBytes, settings, artifactBytes, bakedHeader, error));
		CHECK(bakedHeader.Format == TextureBlockFormat::Bc7);
		CHECK(bakedHeader.MipCount == 4);

		// 1. 命中产物:FromArtifact + 头一致 + 逐 mip 切片正好落在数据段内。
		{
			WriteFile(artifactPath, artifactBytes);
			const TextureAsset asset = LoadTextureAsset(sourcePath.string());
			CHECK(asset.FromArtifact);
			CHECK(asset.Header.Format == TextureBlockFormat::Bc7);
			CHECK(asset.Header.Width == 8 && asset.Header.Height == 8);
			CHECK(asset.Header.MipCount == bakedHeader.MipCount);
			CHECK(asset.Header.Srgb == bakedHeader.Srgb);
			CHECK(asset.Header.SettingsHash == bakedHeader.SettingsHash);
			CHECK(asset.Bytes.size() == kTextureArtifactHeaderSize + asset.Header.DataSize);
			uint64_t totalMipBytes = 0;
			for (uint32_t mip = 0; mip < asset.Header.MipCount; ++mip)
			{
				const uint8_t* data = asset.MipData(mip);
				const uint64_t size = asset.MipSize(mip);
				CHECK(data != nullptr);
				CHECK(size == asset.Header.MipDataSize(mip));
				// 切片必须落在产物内部:头 + offset + size ≤ 文件长度。
				CHECK(static_cast<uint64_t>(data - asset.Bytes.data())
					+ size <= asset.Bytes.size());
				totalMipBytes += size;
			}
			CHECK(totalMipBytes == asset.Header.DataSize);
			CHECK(asset.MipData(asset.Header.MipCount) == nullptr);
			CHECK(asset.MipSize(asset.Header.MipCount) == 0);
			CHECK(asset.Source.Pixels.empty());   // 命中产物就不解码源图
		}

		// 2. 没有产物:回退源图,像素与 LoadTextureData 逐字节一致(P2 之前的行为)。
		{
			std::error_code ec;
			std::filesystem::remove(artifactPath, ec);
			const TextureAsset asset = LoadTextureAsset(sourcePath.string());
			CHECK(!asset.FromArtifact);
			CHECK(asset.Source.Valid);
			CHECK(asset.Source.Width == 8 && asset.Source.Height == 8);
			CHECK(asset.Source.Pixels.size() == 8u * 8u * 4u);
			const TextureData direct = LoadTextureData(sourcePath.string());
			CHECK(direct.Valid);
			CHECK(direct.Pixels == asset.Source.Pixels);   // 逐字节一致
		}

		// 3. 坏产物(截断 / 头版本不对 / 未压缩数据长度撒谎)不崩:警告 + 回退源图。
		{
			std::vector<uint8_t> truncated(artifactBytes.begin(), artifactBytes.begin() + 64);
			WriteFile(artifactPath, truncated);
			const TextureAsset asset = LoadTextureAsset(sourcePath.string());
			CHECK(!asset.FromArtifact);
			CHECK(asset.Source.Valid && asset.Source.Pixels.size() == 8u * 8u * 4u);

			std::vector<uint8_t> badVersion = artifactBytes;
			badVersion[4] = 0xEE;   // version u32 小端第 0 字节
			WriteFile(artifactPath, badVersion);
			const TextureAsset asset2 = LoadTextureAsset(sourcePath.string());
			CHECK(!asset2.FromArtifact);
			CHECK(asset2.Source.Valid);

			std::vector<uint8_t> badMagic = artifactBytes;
			badMagic[0] = 'X';
			WriteFile(artifactPath, badMagic);
			const TextureAsset asset3 = LoadTextureAsset(sourcePath.string());
			CHECK(!asset3.FromArtifact);
			CHECK(asset3.Source.Valid);
		}

		// 4. 不存在的源文件:产物也不在 = 白色兜底(与 LoadTextureData 同款),不抛异常。
		{
			std::error_code ec;
			std::filesystem::remove(artifactPath, ec);
			const TextureAsset asset = LoadTextureAsset((temp.path / "textures" / "missing.png").string());
			CHECK(!asset.FromArtifact);
			CHECK(!asset.Source.Valid);
			CHECK(asset.Source.Width == 1 && asset.Source.Height == 1);
		}

		// 4b. 物质引用 `.wtex`:没有产物时按资产里的 source(缺省 = 同目录同主名图片)回退解码。
		{
			const std::filesystem::path assetPath = temp.path / "textures" / "T.wtex";
			const std::string assetText = "usage: color\n";
			WriteFile(assetPath, std::vector<uint8_t>(assetText.begin(), assetText.end()));
			const TextureAsset asset = LoadTextureAsset(assetPath.string());
			CHECK(!asset.FromArtifact);
			CHECK(asset.Source.Valid);
			CHECK(asset.Source.Width == 8 && asset.Source.Height == 8);
			const TextureData direct = LoadTextureData(sourcePath.string());
			CHECK(direct.Valid && direct.Pixels == asset.Source.Pixels);

			// 有产物时走产物(资产引用同样按 `<主名>.wtexc` 找)。
			WriteFile(artifactPath, artifactBytes);
			const TextureAsset withArtifact = LoadTextureAsset(assetPath.string());
			CHECK(withArtifact.FromArtifact);
			CHECK(withArtifact.Header.Format == TextureBlockFormat::Bc7);
			std::error_code ec;
			std::filesystem::remove(artifactPath, ec);
		}

		// 4c. 旧命名 `<源图全名>.wtexc` 仍是容错候选(手工放置/老产物不会被丢)。
		{
			const std::filesystem::path legacyArtifact = temp.path / "textures" / "T.tga.wtexc";
			WriteFile(legacyArtifact, artifactBytes);
			const TextureAsset asset = LoadTextureAsset(sourcePath.string());
			CHECK(asset.FromArtifact && asset.Header.Width == 8);
			std::error_code ec;
			std::filesystem::remove(legacyArtifact, ec);
		}

		// 5. 包装层 BC5 开关:默认关(与 RGBA8 时代同源),打开后多一条 #define 与 Z 重建。
		{
			const std::string defaultWrapper =
				MaterialSurfaceCompiler::WrapSurfaceSourceWithParams("", {}, SurfaceShaderBackend::VulkanSpirV);
			CHECK(!defaultWrapper.empty());
			CHECK(Contains(defaultWrapper, "#define WE_NORMAL_TEXTURE_BC5 0"));
			CHECK(!Contains(defaultWrapper, "#define WE_NORMAL_TEXTURE_BC5 1"));
			CHECK(Contains(defaultWrapper,
				"tangentNormal.z = sqrt(saturate(1.0f - dot(tangentNormal.xy, tangentNormal.xy)));"));
			// BC5 分支必须让重建出来的 Z 参与合并(默认走 #else,RGBA8 行为逐值不变)。
			CHECK(Contains(defaultWrapper, "surface.Normal.z * tangentNormal.z"));
			CHECK(Contains(defaultWrapper, "#else\n    float3 combined"));

			std::string forcedWrapper;
			{
				const ScopedEnv force("WLD_SURFACE_NORMAL_BC5", "1");
				forcedWrapper = MaterialSurfaceCompiler::WrapSurfaceSourceWithParams("", {},
					SurfaceShaderBackend::VulkanSpirV);
			}
			CHECK(!forcedWrapper.empty());
			CHECK(Contains(forcedWrapper, "#define WE_NORMAL_TEXTURE_BC5 1"));
			CHECK(Contains(forcedWrapper, "normal texture is BC5"));
			CHECK(forcedWrapper.find("#define WE_NORMAL_TEXTURE_BC5 1")
				< forcedWrapper.find("#ifndef WE_NORMAL_TEXTURE_BC5"));
			CHECK(forcedWrapper != defaultWrapper);

			// 还原后必须回到默认(开关是进程级诊断覆盖,不能粘住)。
			const std::string after =
				MaterialSurfaceCompiler::WrapSurfaceSourceWithParams("", {}, SurfaceShaderBackend::VulkanSpirV);
			CHECK(after == defaultWrapper);
		}

		std::printf("World.TextureRuntime: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.TextureRuntime: FAILED: %s\n", error.what());
		return 1;
	}
}
