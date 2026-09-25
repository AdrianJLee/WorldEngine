#include "wldpch.h"

#include "World/Renderer/TextureData.h"

#include "World/Core/Application.h"
#include "World/Core/Log.h"
#include "World/Renderer/TextureImportSettings.h"

#include <stb_image.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace World
{
	namespace
	{
		TextureData MakeWhiteFallback()
		{
			TextureData data;
			data.Pixels = { 255, 255, 255, 255 };
			data.Width = 1;
			data.Height = 1;
			data.Channels = 4;
			data.Valid = false;
			return data;
		}

		// 返回加载到的像素(交给 stbi_image_free),失败返回 nullptr。
		// desiredChannels = 4:统一按 RGBA8 解码,调用方无需再做格式转换。
		stbi_uc* LoadPixels(const std::string& path, bool flipVertically, int desiredChannels,
			int& width, int& height, int& channels)
		{
			stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

			// 1. VFS 优先:开发目录 provider 或发行包 provider。
			if (Application::HasInstance())
			{
				std::error_code vfsError;
				std::vector<uint8_t> bytes;
				if (Application::Get().GetContext().Vfs().Read(path, bytes, vfsError) && !bytes.empty())
				{
					stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
						&width, &height, &channels, desiredChannels);
					if (pixels)
						return pixels;
				}
			}

			// 2. 磁盘回退(未挂载 VFS 的 headless / 开发环境)。
			// 绝对路径(编辑器自带资源,经 EditorResourcePath 拼出来)= 直接读:
			// 它本来就不在内容根里,拼上内容根只会拼出个不存在的路径。
			const std::filesystem::path requested(path);
			if (requested.is_absolute())
				return stbi_load(path.c_str(), &width, &height, &channels, desiredChannels);

			// 相对路径 = 项目内容根(WLD_ASSETPATH)里的资产逻辑路径,与材质路径书写约定一致。
			// P4-U12:删掉早先"Game/ 与 Editor/ 也算内容根"的旧布局回退 —— 内容根只有一个,
			// 多候选会让"路径写错却恰好命中旧目录"变成静默成功。
			std::error_code ec;
			const std::filesystem::path diskPath =
				std::filesystem::path(std::string(WLD_PROJECT_DIR)) / "assets" / path;
			return stbi_load(diskPath.string().c_str(), &width, &height, &channels, desiredChannels);
		}

		// 与 LoadPixels 同一套解析口径的**字节**读取(VFS 优先 → 磁盘回退),
		// 给烘焙产物 `.wtexc` 用:产物不需要解码,只需要原始字节。
		bool ReadAssetBytes(const std::string& path, std::vector<uint8_t>& out)
		{
			out.clear();
			if (Application::HasInstance())
			{
				std::error_code vfsError;
				if (Application::Get().GetContext().Vfs().Read(path, out, vfsError) && !out.empty())
					return true;
			}

			const std::filesystem::path requested(path);
			const std::filesystem::path diskPath = requested.is_absolute()
				? requested
				: std::filesystem::path(std::string(WLD_PROJECT_DIR)) / "assets" / path;

			std::ifstream input(diskPath, std::ios::binary | std::ios::ate);
			if (!input)
				return false;
			const std::streamoff size = input.tellg();
			if (size <= 0)
				return false;
			out.resize(static_cast<size_t>(size));
			input.seekg(0);
			input.read(reinterpret_cast<char*>(out.data()), size);
			if (!input)
			{
				out.clear();
				return false;
			}
			return true;
		}

		bool EndsWith(const std::string& text, const char* suffix)
		{
			const size_t suffixLength = std::strlen(suffix);
			return text.size() >= suffixLength
				&& text.compare(text.size() - suffixLength, suffixLength, suffix) == 0;
		}

		// 逻辑路径 → 产物候选(按顺序找,第一个能解出头的生效):
		//   ① `<同目录>/<主名>.wtexc` —— 契约命名(2026-09-25 起,不再带源图扩展名);
		//   ② `<逻辑路径>.wtexc` —— 容错:手工放置的旧命名(如 `Icon.png.wtexc`)仍能读到。
		std::vector<std::string> ArtifactPathCandidates(const std::string& path)
		{
			std::vector<std::string> candidates;
			if (EndsWith(path, ".wtexc"))
			{
				candidates.push_back(path);
				return candidates;
			}
			const size_t slash = path.find_last_of('/');
			const size_t dot = path.find_last_of('.');
			if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
				candidates.push_back(path.substr(0, dot) + ".wtexc");
			candidates.push_back(path + ".wtexc");
			return candidates;
		}

		// 引用可以是**源图**(`textures/Icon.png`)或**纹理资产**(`textures/Icon.wtex`)。
		// 走资产且没有产物时的回退:读资产里的 `source:`(缺省 = 同目录同主名图片)再解码。
		// 只看磁盘:打包态源图/资产已被剥离 ⇒ 没有产物就是没有数据(契约如此)。
		std::string SourcePathForFallback(const std::string& path)
		{
			if (!EndsWith(path, ".wtex"))
				return path;

			std::filesystem::path assetFile(path);
			if (!assetFile.is_absolute())
				assetFile = std::filesystem::path(std::string(WLD_PROJECT_DIR)) / "assets" / path;

		TextureImportSettings settings;
		std::string settingsError;
		const bool assetLoaded = LoadTextureImportSettings(assetFile, settings, settingsError);
		if (std::getenv("WLD_TRACE_TEX"))
			WLD_CORE_INFO("[tex-resolve] '{0}' assetFile='{1}' loaded={2} source='{3}' error='{4}'",
				path, assetFile.generic_string(), static_cast<int>(assetLoaded), settings.Source,
				settingsError);
		if (assetLoaded && !settings.Source.empty())
			return settings.Source;

			// source: 缺省(或资产读不了)= 同目录同主名的图片,与烘焙器的候选顺序一致。
			static const char* const kCandidates[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
			for (const char* extension : kCandidates)
			{
				std::error_code existsError;
				const std::filesystem::path candidate =
					assetFile.parent_path() / (assetFile.stem().string() + extension);
				if (std::filesystem::is_regular_file(candidate, existsError))
				{
					// 转回逻辑路径(绝对路径也能被 LoadTextureData 直接吃)。
					return candidate.is_absolute() ? candidate.generic_string()
						: candidate.generic_string();
				}
			}
			if (!settingsError.empty())
				WLD_CORE_WARN("Texture asset '{0}': {1}", path, settingsError);
			return path;   // 真找不到:交给 LoadTextureData 走它自己的兜底(白纹理 + 警告)
		}
	}

	// 公开入口(见 TextureData.h):资产引用 → 源图路径。内部实现就是上面那份(唯一口径)。
	std::string ResolveTextureSourcePath(const std::string& path)
	{
		return SourcePathForFallback(path);
	}

	TextureData LoadTextureData(const std::string& path, bool flipVertically)
	{
		if (path.empty())
			return MakeWhiteFallback();

		int width = 1;
		int height = 1;
		int channels = 4;
		stbi_uc* pixels = LoadPixels(path, flipVertically, 4, width, height, channels);
		if (!pixels)
		{
			// 坏资产不拖垮进程:1x1 白色兜底 + 警告(旧 GL 路径的既定行为)。
			WLD_CORE_WARN("Failed to load image '{0}'; using 1x1 white fallback", path);
			return MakeWhiteFallback();
		}

		TextureData data;
		data.Width = static_cast<uint32_t>(width);
		data.Height = static_cast<uint32_t>(height);
		data.Channels = channels;
		data.Valid = true;

		// 已按 RGBA8 解码;Channels 记录**原图通道数**,GL 侧据此选内部格式(RGB8/RGBA8)。
		data.Pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
		stbi_image_free(pixels);
		return data;
	}

	const uint8_t* TextureAsset::MipData(uint32_t mip) const
	{
		if (!FromArtifact || mip >= Header.MipCount)
			return nullptr;
		const uint64_t start = static_cast<uint64_t>(kTextureArtifactHeaderSize) + Header.Mips[mip].Offset;
		if (start + Header.Mips[mip].Size > Bytes.size())
			return nullptr;
		return Bytes.data() + start;
	}

	uint64_t TextureAsset::MipSize(uint32_t mip) const
	{
		const uint8_t* data = MipData(mip);
		return data ? Header.Mips[mip].Size : 0;
	}

	TextureAsset LoadTextureAsset(const std::string& path, bool flipVertically)
	{
		TextureAsset asset;
		if (path.empty())
		{
			asset.Source = MakeWhiteFallback();
			return asset;
		}

		const std::vector<std::string> candidates = ArtifactPathCandidates(path);
		for (const std::string& artifactPath : candidates)
		{
			std::vector<uint8_t> bytes;
			if (!ReadAssetBytes(artifactPath, bytes))
				continue;
			TextureArtifactHeader header;
			std::string error;
			if (ParseTextureArtifact(bytes, header, error))
			{
				asset.FromArtifact = true;
				asset.Header = header;
				asset.Bytes = std::move(bytes);
				return asset;
			}
			// 坏产物不静默:警告 + 回退源图(契约 §7.3),进程不受影响。
			WLD_CORE_WARN("Texture artifact '{0}' rejected: {1}; falling back to the source image",
				artifactPath, error);
		}

		asset.Source = LoadTextureData(SourcePathForFallback(path), flipVertically);
		return asset;
	}
}
