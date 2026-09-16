#include "wldpch.h"

#include "World/Renderer/TextureData.h"

#include "World/Core/Application.h"
#include "World/Core/Log.h"

#include <stb_image.h>

#include <filesystem>

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

			// 2. 磁盘回退(未挂载 VFS 的开发环境)。
			std::string diskPath;
			if (std::filesystem::exists(std::string(WLD_GAME_DIR) + path))
				diskPath = std::string(WLD_GAME_DIR) + path;
			else
				diskPath = std::string(WLD_EDITOR_DIR) + path;
			return stbi_load(diskPath.c_str(), &width, &height, &channels, desiredChannels);
		}
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
}
