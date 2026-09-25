// M4-TEX P1:纹理导入设置 + 烘焙产物(确定性 / 格式选择 / mip 链 / 缓存 / 解析健壮性)。

#include "World/Core/Core.h"
#include "World/Core/Sha256.h"
#include "World/Renderer/TextureArtifact.h"
#include "World/Renderer/TextureCompiler.h"
#include "World/Renderer/TextureImportSettings.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
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
			const uint64_t unique =
				static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			path = std::filesystem::temp_directory_path(ec) / ("we-teximport-" + std::to_string(unique));
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

	// 未压缩 32 位 TGA(顶左原点):测试用的最小源图,避免依赖 PNG 编码器。
	std::vector<uint8_t> MakeTga(uint32_t width, uint32_t height,
		const std::vector<uint8_t>& rgba)
	{
		if (rgba.size() != static_cast<size_t>(width) * height * 4)
			throw std::runtime_error("MakeTga: pixel buffer size mismatch");
		std::vector<uint8_t> bytes(18 + rgba.size(), 0);
		bytes[2] = 2;                                   // uncompressed true-color
		bytes[12] = static_cast<uint8_t>(width & 0xFF);
		bytes[13] = static_cast<uint8_t>((width >> 8) & 0xFF);
		bytes[14] = static_cast<uint8_t>(height & 0xFF);
		bytes[15] = static_cast<uint8_t>((height >> 8) & 0xFF);
		bytes[16] = 32;                                 // bpp
		bytes[17] = 0x28;                               // top-left origin + 8 alpha bits
		for (size_t index = 0; index < rgba.size(); index += 4)
		{
			bytes[18 + index + 0] = rgba[index + 2];    // B
			bytes[18 + index + 1] = rgba[index + 1];    // G
			bytes[18 + index + 2] = rgba[index + 0];    // R
			bytes[18 + index + 3] = rgba[index + 3];    // A
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

	void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	}
}

int main()
{
	try
	{
		// 1. usage 派生的默认值表(自动压缩格式 + sRGB + mip)。
		{
			TextureImportSettings color;
			color.Usage = TextureUsage::Color;
			CHECK(color.EffectiveSrgb());
			CHECK(color.EffectiveMipmaps());
			CHECK(color.EffectiveCompression() == TextureCompression::BC7);

			TextureImportSettings normal;
			normal.Usage = TextureUsage::Normal;
			CHECK(!normal.EffectiveSrgb());
			CHECK(normal.EffectiveCompression() == TextureCompression::BC5);

			TextureImportSettings data;
			data.Usage = TextureUsage::Data;
			CHECK(!data.EffectiveSrgb());
			CHECK(data.EffectiveCompression() == TextureCompression::BC4);

			TextureImportSettings ui;
			ui.Usage = TextureUsage::Ui;
			CHECK(ui.EffectiveSrgb());
			CHECK(!ui.EffectiveMipmaps());
			CHECK(ui.EffectiveCompression() == TextureCompression::None);

			TextureImportSettings hdr;
			hdr.Usage = TextureUsage::Hdr;
			CHECK(!hdr.EffectiveSrgb());
			CHECK(hdr.EffectiveCompression() == TextureCompression::None);
		}

		// 2. YAML 解析:显式字段 / 未知字段报错 / 越界报错 / 序列化往返。
		{
			TextureImportSettings settings;
			std::string error;
			const std::string text =
				"usage: normal\n"
				"compression: bc7\n"
				"srgb: false\n"
				"mipmaps: false\n"
				"max_size: 512\n"
				"wrap: clamp\n"
				"filter: point\n"
				"anisotropy: 8\n";
			CHECK(TextureImportSettings::Parse(text, settings, error));
			CHECK(settings.Usage == TextureUsage::Normal);
			CHECK(settings.Compression == TextureCompression::BC7);
			CHECK(settings.SrgbExplicit && !settings.Srgb);
			CHECK(settings.MipmapsExplicit && !settings.Mipmaps);
			CHECK(settings.MaxSize == 512);
			CHECK(settings.Wrap == TextureWrap::Clamp);
			CHECK(settings.Filter == TextureFilter::Point);
			CHECK(settings.Anisotropy == 8);

			const std::string serialized = settings.Serialize();
			TextureImportSettings reparsed;
			CHECK(TextureImportSettings::Parse(serialized, reparsed, error));
			CHECK(reparsed.Hash() == settings.Hash());

			TextureImportSettings unknown;
			CHECK(!TextureImportSettings::Parse("compresion: bc7\n", unknown, error));
			CHECK(error.find("unknown field") != std::string::npos);

			TextureImportSettings outOfRange;
			CHECK(!TextureImportSettings::Parse("anisotropy: 64\n", outOfRange, error));
			CHECK(error.find("out of range") != std::string::npos);

			TextureImportSettings badValue;
			CHECK(!TextureImportSettings::Parse("compression: bz7\n", badValue, error));
			CHECK(error.find("unknown value") != std::string::npos);
		}

		// 3. Hash 口径:显式但等效的字段不改变键;实质变化必须改变键。
		{
			TextureImportSettings a;
			TextureImportSettings b;
			b.SrgbExplicit = true;
			b.Srgb = true;   // Color 默认就是 sRGB ⇒ 等效
			CHECK(a.Hash() == b.Hash());
			b.Compression = TextureCompression::None;
			CHECK(a.Hash() != b.Hash());
		}

		// 4. sidecar 磁盘往返 + 缺文件 = 默认。
		{
			TempDir temp;
			const std::filesystem::path sidecar = temp.path / "textures" / "Icon.png.wtex";
			TextureImportSettings original;
			original.Usage = TextureUsage::Normal;
			original.MaxSize = 1024;
			std::string error;
			CHECK(SaveTextureImportSettings(sidecar, original, error));
			CHECK(World::TextureSettingsPath("textures/Icon.png") == "textures/Icon.png.wtex");

			TextureImportSettings loaded;
			CHECK(LoadTextureImportSettings(sidecar, loaded, error));
			CHECK(loaded.Usage == TextureUsage::Normal);
			CHECK(loaded.MaxSize == 1024);
			CHECK(loaded.Hash() == original.Hash());

			TextureImportSettings missing;
			CHECK(LoadTextureImportSettings(temp.path / "nope.wtex", missing, error));
			CHECK(missing.Usage == TextureUsage::Color);
		}

		// 5. SHA-256 标准向量(空串 + "abc")。
		{
			const std::vector<uint8_t> abc = { 'a', 'b', 'c' };
			CHECK(World::Crypto::Sha256Hex(abc)
				== "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
			CHECK(World::Crypto::Sha256Hex(std::vector<uint8_t> {})
				== "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
		}

		// 6. 烘焙确定性 + 格式/mip 尺寸口径。
		{
			const std::vector<uint8_t> source = MakeTga(8, 8, MakeGradient(8, 8));
			TextureImportSettings color;   // 默认颜色 = BC7 + mip
			std::vector<uint8_t> first;
			std::vector<uint8_t> second;
			TextureArtifactHeader firstHeader;
			TextureArtifactHeader secondHeader;
			std::string error;
			CHECK(TextureCompiler::BakeBytes(source, color, first, firstHeader, error));
			CHECK(TextureCompiler::BakeBytes(source, color, second, secondHeader, error));
			CHECK(first == second);   // 逐字节确定
			CHECK(firstHeader.Format == TextureBlockFormat::Bc7);
			CHECK(firstHeader.Srgb);
			CHECK(firstHeader.MipCount == 4);
			CHECK(firstHeader.MipDataSize(0) == 4 * 16);
			CHECK(firstHeader.MipDataSize(1) == 1 * 16);
			CHECK(firstHeader.MipDataSize(2) == 1 * 16);
			CHECK(firstHeader.MipDataSize(3) == 1 * 16);
			CHECK(firstHeader.DataSize == 4 * 16 + 16 + 16 + 16);

			TextureImportSettings normal;
			normal.Usage = TextureUsage::Normal;
			std::vector<uint8_t> normalArtifact;
			TextureArtifactHeader normalHeader;
			CHECK(TextureCompiler::BakeBytes(source, normal, normalArtifact, normalHeader, error));
			CHECK(normalHeader.Format == TextureBlockFormat::Bc5);
			CHECK(!normalHeader.Srgb);
			CHECK(normalHeader.MipDataSize(0) == 4 * 16);

			TextureImportSettings ui;
			ui.Usage = TextureUsage::Ui;
			std::vector<uint8_t> uiArtifact;
			TextureArtifactHeader uiHeader;
			CHECK(TextureCompiler::BakeBytes(source, ui, uiArtifact, uiHeader, error));
			CHECK(uiHeader.Format == TextureBlockFormat::Rgba8);
			CHECK(uiHeader.MipCount == 1);
			CHECK(uiHeader.DataSize == 8 * 8 * 4);
			CHECK(uiHeader.Srgb);
		}

		// 7. max_size 等比缩放(8x4 → 4x2)+ 非 4 倍边长补齐。
		{
			const std::vector<uint8_t> source = MakeTga(8, 4, MakeGradient(8, 4));
			TextureImportSettings settings;
			settings.Usage = TextureUsage::Data;
			settings.MaxSize = 4;
			std::vector<uint8_t> artifact;
			TextureArtifactHeader header;
			std::string error;
			CHECK(TextureCompiler::BakeBytes(source, settings, artifact, header, error));
			CHECK(header.Width == 4 && header.Height == 2);
			CHECK(header.Format == TextureBlockFormat::Bc4);
			CHECK(header.MipDataSize(0) == 1 * 8);    // 4x2 = 1 个 4x4 块
			CHECK(header.MipCount == 3);              // 4x2 → 2x1 → 1x1

			const std::vector<uint8_t> odd = MakeTga(5, 3, MakeGradient(5, 3));
			TextureImportSettings oddSettings;   // 不带 max_size:专门验"非 4 倍边长按块补齐"
			oddSettings.Usage = TextureUsage::Data;
			std::vector<uint8_t> oddArtifact;
			TextureArtifactHeader oddHeader;
			CHECK(TextureCompiler::BakeBytes(odd, oddSettings, oddArtifact, oddHeader, error));
			CHECK(oddHeader.Width == 5 && oddHeader.Height == 3);
			CHECK(oddHeader.MipDataSize(0) == 2 * 1 * 8);   // 5x3 → ceil(5/4)xceil(3/4) 块
		}

		// 8. 产物解析:往返一致 + 坏头/截断被拒。
		{
			const std::vector<uint8_t> source = MakeTga(4, 4, MakeGradient(4, 4));
			TextureImportSettings settings;
			std::vector<uint8_t> artifact;
			TextureArtifactHeader baked;
			std::string error;
			CHECK(TextureCompiler::BakeBytes(source, settings, artifact, baked, error));

			TextureArtifactHeader parsed;
			CHECK(ParseTextureArtifact(artifact, parsed, error));
			CHECK(parsed.Format == baked.Format);
			CHECK(parsed.Srgb == baked.Srgb);
			CHECK(parsed.Width == baked.Width && parsed.Height == baked.Height);
			CHECK(parsed.MipCount == baked.MipCount);
			CHECK(parsed.SettingsHash == baked.SettingsHash);
			CHECK(std::memcmp(parsed.SourceSha256, baked.SourceSha256, 32) == 0);

			std::vector<uint8_t> truncated(artifact.begin(), artifact.begin() + 40);
			TextureArtifactHeader rejected;
			CHECK(!ParseTextureArtifact(truncated, rejected, error));

			std::vector<uint8_t> corrupt = artifact;
			corrupt[0] = 'X';
			CHECK(!ParseTextureArtifact(corrupt, rejected, error));
			CHECK(!error.empty());
		}

		// 9. 目录烘焙:sidecar 生效 + 缓存命中 + 改设置后重烘。
		{
			TempDir temp;
			const std::filesystem::path sourceRoot = temp.path / "assets";
			const std::filesystem::path outputRoot = temp.path / "cooked";
			const std::filesystem::path cacheDir = temp.path / "cache";
			WriteFile(sourceRoot / "textures" / "icons" / "A.tga", MakeTga(8, 8, MakeGradient(8, 8)));
			WriteFile(sourceRoot / "textures" / "B.tga", MakeTga(8, 8, MakeGradient(8, 8)));
			{
				std::ofstream sidecar(sourceRoot / "textures" / "B.tga.wtex", std::ios::trunc);
				sidecar << "usage: normal\n";
			}

			TextureBakeOptions options;
			TextureBakeStats first = TextureCompiler::BakeDirectory(sourceRoot, outputRoot, cacheDir, options);
			CHECK(first.Baked == 2);
			CHECK(first.Failed == 0);
			CHECK(std::filesystem::exists(outputRoot / "textures" / "icons" / "A.tga.wtexc"));
			CHECK(std::filesystem::exists(outputRoot / "textures" / "B.tga.wtexc"));

			std::vector<uint8_t> bBytes;
			{
				std::ifstream input(outputRoot / "textures" / "B.tga.wtexc", std::ios::binary | std::ios::ate);
				bBytes.resize(static_cast<size_t>(input.tellg()));
				input.seekg(0);
				input.read(reinterpret_cast<char*>(bBytes.data()), static_cast<std::streamsize>(bBytes.size()));
			}
			TextureArtifactHeader bHeader;
			std::string error;
			CHECK(ParseTextureArtifact(bBytes, bHeader, error));
			CHECK(bHeader.Format == TextureBlockFormat::Bc5);

			TextureBakeStats second = TextureCompiler::BakeDirectory(sourceRoot, outputRoot, cacheDir, options);
			CHECK(second.Baked == 0 && second.UpToDate == 2);

			{
				std::ofstream sidecar(sourceRoot / "textures" / "B.tga.wtex", std::ios::trunc);
				sidecar << "usage: data\n";
			}
			TextureBakeStats third = TextureCompiler::BakeDirectory(sourceRoot, outputRoot, cacheDir, options);
			CHECK(third.Baked == 1 && third.UpToDate == 1);
			std::printf("World.TextureImport: dir bake baked=%zu uptodate=%zu\n",
				third.Baked, third.UpToDate);
		}

		std::printf("World.TextureImport: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.TextureImport: FAILED: %s\n", error.what());
		return 1;
	}
}
