#include "TextureSettingsPanel_Internal.h"

namespace World
{

using namespace TextureSettingsPanelDetail;

namespace TextureSettingsPanelDetail
{

		// 盒式降采样(2x2 → 1);srgb=true 时先转线性再平均(避免缩小后整体变暗)。
		// 与 TextureCompiler.cpp 的 DownsampleBox 逐字段同口径。
void DownsampleBox2x(const std::vector<uint8_t>& source, uint32_t sourceWidth, uint32_t sourceHeight, bool srgb, std::vector<uint8_t>& out, uint32_t& outWidth, uint32_t& outHeight){
			outWidth = std::max(1u, sourceWidth / 2);
			outHeight = std::max(1u, sourceHeight / 2);
			out.assign(static_cast<size_t>(outWidth) * outHeight * 4u, 0);
			for (uint32_t y = 0; y < outHeight; ++y)
			{
				for (uint32_t x = 0; x < outWidth; ++x)
				{
					uint32_t accumulators[4] = { 0, 0, 0, 0 };
					uint32_t samples = 0;
					for (uint32_t dy = 0; dy < 2; ++dy)
					{
						const uint32_t sourceY = std::min(sourceHeight - 1, y * 2 + dy);
						for (uint32_t dx = 0; dx < 2; ++dx)
						{
							const uint32_t sourceX = std::min(sourceWidth - 1, x * 2 + dx);
							const uint8_t* texel = source.data()
								+ (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4u;
							for (int channel = 0; channel < 4; ++channel)
							{
								const uint8_t value = srgb && channel < 3 ? ToLinearByte(texel[channel])
									: texel[channel];
								accumulators[channel] += value;
							}
							++samples;
						}
					}
					uint8_t* target = out.data() + (static_cast<size_t>(y) * outWidth + x) * 4u;
					for (int channel = 0; channel < 4; ++channel)
					{
						const uint8_t averaged = static_cast<uint8_t>(
							(accumulators[channel] + samples / 2) / samples);
						target[channel] = srgb && channel < 3 ? ToSrgbByte(averaged) : averaged;
					}
				}
			}
		}


		// 等比缩放到"最长边 = maxEdge"(maxEdge = 0 或未超限时原样返回)。逐级 2x 盒式 +
		// 最后一步最近邻补齐 —— 与 TextureCompiler::ResizeToMaxSize 同一口径(预览与产物尺寸一致)。
std::vector<uint8_t> ResampleToMaxEdge(const std::vector<uint8_t>& source, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t maxEdge, bool srgb, uint32_t& outWidth, uint32_t& outHeight){
			outWidth = sourceWidth;
			outHeight = sourceHeight;
			if (maxEdge == 0 || std::max(sourceWidth, sourceHeight) <= maxEdge
				|| sourceWidth == 0 || sourceHeight == 0)
				return source;
			const double scale = static_cast<double>(maxEdge)
				/ static_cast<double>(std::max(sourceWidth, sourceHeight));
			const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::floor(sourceWidth * scale)));
			const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::floor(sourceHeight * scale)));

			std::vector<uint8_t> current = source;
			uint32_t width = sourceWidth;
			uint32_t height = sourceHeight;
			while (width / 2 >= targetWidth && height / 2 >= targetHeight && (width > 1 || height > 1))
			{
				std::vector<uint8_t> next;
				uint32_t nextWidth = 0;
				uint32_t nextHeight = 0;
				DownsampleBox2x(current, width, height, srgb, next, nextWidth, nextHeight);
				current.swap(next);
				width = nextWidth;
				height = nextHeight;
			}
			if (width != targetWidth || height != targetHeight)
			{
				std::vector<uint8_t> resized(static_cast<size_t>(targetWidth) * targetHeight * 4u, 0);
				for (uint32_t y = 0; y < targetHeight; ++y)
				{
					const uint32_t sourceY = std::min(height - 1,
						static_cast<uint32_t>(static_cast<uint64_t>(y) * height / targetHeight));
					for (uint32_t x = 0; x < targetWidth; ++x)
					{
						const uint32_t sourceX = std::min(width - 1,
							static_cast<uint32_t>(static_cast<uint64_t>(x) * width / targetWidth));
						std::memcpy(resized.data() + (static_cast<size_t>(y) * targetWidth + x) * 4u,
							current.data() + (static_cast<size_t>(sourceY) * width + sourceX) * 4u, 4u);
					}
				}
				current.swap(resized);
				width = targetWidth;
				height = targetHeight;
			}
			outWidth = width;
			outHeight = height;
			return current;
		}


void FlipRowsInPlace(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height){
			const size_t rowBytes = static_cast<size_t>(width) * 4u;
			if (rowBytes == 0 || height < 2 || pixels.size() < rowBytes * height)
				return;
			std::vector<uint8_t> scratch(rowBytes);
			for (uint32_t row = 0; row < height / 2; ++row)
			{
				uint8_t* top = pixels.data() + static_cast<size_t>(row) * rowBytes;
				uint8_t* bottom = pixels.data() + static_cast<size_t>(height - 1u - row) * rowBytes;
				std::memcpy(scratch.data(), top, rowBytes);
				std::memcpy(top, bottom, rowBytes);
				std::memcpy(bottom, scratch.data(), rowBytes);
			}
		}


void PremultiplyAlphaInPlace(std::vector<uint8_t>& pixels){
			for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
			{
				const uint32_t alpha = pixels[offset + 3];
				for (int channel = 0; channel < 3; ++channel)
					pixels[offset + static_cast<size_t>(channel)] = static_cast<uint8_t>(
						(pixels[offset + static_cast<size_t>(channel)] * alpha + 127) / 255);
			}
		}


		// 通道开关:channel 0 = RGB(原样);1..4 = R/G/B/A 单通道 → 灰度(不透明)。
void ApplyChannelMask(std::vector<uint8_t>& pixels, int channel){
			if (channel <= 0)
				return;
			const size_t index = static_cast<size_t>(channel - 1);
			for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
			{
				const uint8_t value = pixels[offset + index];
				pixels[offset + 0] = value;
				pixels[offset + 1] = value;
				pixels[offset + 2] = value;
				pixels[offset + 3] = 255;
			}
		}


		// 产物块格式 → 预览用的 RHI 格式。**一律 UNORM**:WUI 与交换链都是非 sRGB 通道,
		// 预览要显示"文件里存的字节"(sRGB 是采样期解码,不改变像素内容);用 _SRGB 变体会让
		// 整个预览偏暗,也与图标/材质缩略图的显示口径不一致。压缩块本身与 sRGB 变体完全相同。
Rhi::Format PreviewDisplayFormat(TextureBlockFormat format){
			switch (format)
			{
				case TextureBlockFormat::Rgba8: return Rhi::Format::R8G8B8A8_UNORM;
				case TextureBlockFormat::Bc7: return Rhi::Format::BC7_UNORM;
				case TextureBlockFormat::Bc5: return Rhi::Format::BC5_UNORM;
				case TextureBlockFormat::Bc4: return Rhi::Format::BC4_UNORM;
				case TextureBlockFormat::Bc1: return Rhi::Format::BC1_UNORM;
				case TextureBlockFormat::Bc3: return Rhi::Format::BC3_UNORM;
				default: return Rhi::Format::Undefined;   // Rgba16f 等暂无上传路径
			}
		}


const char* PreviewChannelCode(int channel){
			switch (channel)
			{
				case 1: return "r";
				case 2: return "g";
				case 3: return "b";
				case 4: return "a";
				default: return "rgb";
			}
		}


const char* PreviewStateCode(int state){
			switch (state)
			{
				case 1: return "pending";
				case 2: return "encoding";
				case 3: return "baked";
				case 4: return "failed";
				default: return "idle";
			}
		}


		// 产物落点(与内核 `TextureCompiler::BakeDirectory` / `TextureData` 的查找口径一致):
		// 契约名 = `<同目录>/<源图主名>.wtexc`(2026-09-25 起产物名里不再带源图扩展名,
		// 见 tools/agents/dispatch/we_editor.md 的"命名口径")。
std::filesystem::path ArtifactPathForSource(const std::filesystem::path& sourceFile){
			return sourceFile.parent_path() / (sourceFile.stem().string() + ".wtexc");
		}


		// 容错候选:旧命名 `<源图全名>.wtexc`(内核 lookup 的第二候选)。编辑器只在契约名
		// 缺失时读它;`.wtexc` 不是资产,内容浏览器里一律不显示。
std::filesystem::path LegacyArtifactPathForSource(const std::filesystem::path& sourceFile){
			return std::filesystem::path(sourceFile.string() + ".wtexc");
		}


PreviewZoomRange PreviewZoomFor(const Wui::WuiRect& rect, uint32_t width, uint32_t height){
			PreviewZoomRange range;
			if (width == 0 || height == 0 || rect.W <= 1.0f || rect.H <= 1.0f)
				return range;
			const float textureW = static_cast<float>(width);
			const float textureH = static_cast<float>(height);
			range.Fit = std::min(rect.W / textureW, rect.H / textureH);
			const float minZoom = std::min(range.Fit, 1.0f);
			const float maxZoom = std::max(range.Fit, kMaxZoomFactor);
			range.MinFactor = range.Fit > 0.0f ? minZoom / range.Fit : 1.0f;
			range.MaxFactor = range.Fit > 0.0f ? maxZoom / range.Fit : 1.0f;
			return range;
		}

}
}
