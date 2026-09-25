#include "wldpch.h"

#include "World/Renderer/TextureCompiler.h"

#include "World/Core/Sha256.h"

#include "bc7enc16.h"
#include "rgbcx.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace World
{
	namespace
	{
		struct ImageRGBA8
		{
			std::vector<uint8_t> Pixels;
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
		{
			return (value + divisor - 1) / divisor;
		}

		void EnsureEncoderInit()
		{
			static std::once_flag once;
			std::call_once(once, []()
			{
				bc7enc16_compress_block_init();
				rgbcx::init();
			});
		}

		bool DecodeImage(const std::vector<uint8_t>& bytes, bool flipY, ImageRGBA8& out,
			std::string& error)
		{
			int width = 0;
			int height = 0;
			int channels = 0;
			// 统一解成 RGBA8:调用方(压缩器)只认一种内存形态。
			stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
				&width, &height, &channels, 4);
			if (!pixels || width <= 0 || height <= 0)
			{
				const char* reason = stbi_failure_reason();
				error = std::string("decode failed: ") + (reason ? reason : "unknown");
				if (pixels)
					stbi_image_free(pixels);
				return false;
			}
			out.Width = static_cast<uint32_t>(width);
			out.Height = static_cast<uint32_t>(height);
			const size_t rowBytes = static_cast<size_t>(width) * 4;
			out.Pixels.resize(rowBytes * static_cast<size_t>(height));
			if (flipY)
			{
				for (int row = 0; row < height; ++row)
					std::memcpy(out.Pixels.data() + static_cast<size_t>(row) * rowBytes,
						pixels + static_cast<size_t>(height - 1 - row) * rowBytes, rowBytes);
			}
			else
			{
				std::memcpy(out.Pixels.data(), pixels, out.Pixels.size());
			}
			stbi_image_free(pixels);
			return true;
		}

		// sRGB <-> 线性(快速近似;与我们着色器侧的 pow(2.2) 口径一致)。
		inline uint8_t ToLinearByte(uint8_t value)
		{
			const float linear = std::pow(static_cast<float>(value) / 255.0f, 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0f, 1.0f) * 255.0f));
		}

		inline uint8_t ToSrgbByte(uint8_t value)
		{
			const float srgb = std::pow(static_cast<float>(value) / 255.0f, 1.0f / 2.2f);
			return static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
		}

		// 盒式降采样(2x2 → 1);srgb=true 时先转线性再平均(避免缩小后整体变暗)。
		void DownsampleBox(const ImageRGBA8& source, ImageRGBA8& out, bool srgb)
		{
			const uint32_t width = std::max(1u, source.Width / 2);
			const uint32_t height = std::max(1u, source.Height / 2);
			out.Width = width;
			out.Height = height;
			out.Pixels.assign(static_cast<size_t>(width) * height * 4, 0);

			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					uint32_t accumulators[4] = { 0, 0, 0, 0 };
					uint32_t samples = 0;
					for (uint32_t dy = 0; dy < 2; ++dy)
					{
						const uint32_t sourceY = std::min(source.Height - 1, y * 2 + dy);
						for (uint32_t dx = 0; dx < 2; ++dx)
						{
							const uint32_t sourceX = std::min(source.Width - 1, x * 2 + dx);
							const uint8_t* texel = source.Pixels.data()
								+ (static_cast<size_t>(sourceY) * source.Width + sourceX) * 4;
							for (int channel = 0; channel < 4; ++channel)
							{
								const uint8_t value = srgb && channel < 3 ? ToLinearByte(texel[channel])
									: texel[channel];
								accumulators[channel] += value;
							}
							++samples;
						}
					}
					uint8_t* target = out.Pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
					for (int channel = 0; channel < 4; ++channel)
					{
						const uint8_t averaged = static_cast<uint8_t>(
							(accumulators[channel] + samples / 2) / samples);
						target[channel] = srgb && channel < 3 ? ToSrgbByte(averaged) : averaged;
					}
				}
			}
		}

		// 等比缩放到"最长边 = maxSize"(maxSize=0 或未超限时不动)。
		void ResizeToMaxSize(ImageRGBA8& image, uint32_t maxSize, bool srgb)
		{
			if (maxSize == 0)
				return;
			const uint32_t longest = std::max(image.Width, image.Height);
			if (longest <= maxSize)
				return;
			const double scale = static_cast<double>(maxSize) / static_cast<double>(longest);
			const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::floor(image.Width * scale)));
			const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::floor(image.Height * scale)));

			ImageRGBA8 current = image;
			ImageRGBA8 next;
			while (current.Width / 2 >= targetWidth && current.Height / 2 >= targetHeight
				&& (current.Width > 1 || current.Height > 1))
			{
				DownsampleBox(current, next, srgb);
				current = std::move(next);
			}
			if (current.Width != targetWidth || current.Height != targetHeight)
			{
				// 非 2 的幂比例:做一次最近邻采样兜底(只在 max_size 非 2 的幂时走到)。
				ImageRGBA8 resized;
				resized.Width = targetWidth;
				resized.Height = targetHeight;
				resized.Pixels.assign(static_cast<size_t>(targetWidth) * targetHeight * 4, 0);
				for (uint32_t y = 0; y < targetHeight; ++y)
				{
					const uint32_t sourceY = std::min(current.Height - 1,
						static_cast<uint32_t>(static_cast<uint64_t>(y) * current.Height / targetHeight));
					for (uint32_t x = 0; x < targetWidth; ++x)
					{
						const uint32_t sourceX = std::min(current.Width - 1,
							static_cast<uint32_t>(static_cast<uint64_t>(x) * current.Width / targetWidth));
						std::memcpy(resized.Pixels.data()
								+ (static_cast<size_t>(y) * targetWidth + x) * 4,
							current.Pixels.data()
								+ (static_cast<size_t>(sourceY) * current.Width + sourceX) * 4, 4);
					}
				}
				current = std::move(resized);
			}
			image = std::move(current);
		}

		void PremultiplyAlpha(ImageRGBA8& image)
		{
			for (size_t offset = 0; offset + 3 < image.Pixels.size(); offset += 4)
			{
				const uint32_t alpha = image.Pixels[offset + 3];
				for (int channel = 0; channel < 3; ++channel)
					image.Pixels[offset + channel] = static_cast<uint8_t>(
						(image.Pixels[offset + channel] * alpha + 127) / 255);
			}
		}

		// 单张 mip 编码:块格式逐 4x4 取块(边缘 clamp),未压缩则整幅拷。
		bool EncodeMip(TextureBlockFormat format, const ImageRGBA8& image, bool srgb,
			std::vector<uint8_t>& out, std::string& error)
		{
			const uint32_t width = image.Width;
			const uint32_t height = image.Height;
			if (IsBlockCompressed(format))
			{
				const uint32_t blockBytes = TextureBlockBytes(format);
				const uint32_t blocksX = DivideRoundUp(width, 4);
				const uint32_t blocksY = DivideRoundUp(height, 4);
				out.assign(static_cast<size_t>(blocksX) * blocksY * blockBytes, 0);

				for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
				{
					for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
					{
						uint8_t block[16 * 4];
						for (uint32_t pixel = 0; pixel < 16; ++pixel)
						{
							const uint32_t localX = pixel % 4;
							const uint32_t localY = pixel / 4;
							const uint32_t sourceX = std::min(width - 1, blockX * 4 + localX);
							const uint32_t sourceY = std::min(height - 1, blockY * 4 + localY);
							std::memcpy(block + pixel * 4,
								image.Pixels.data() + (static_cast<size_t>(sourceY) * width + sourceX) * 4, 4);
						}
						uint8_t* target = out.data()
							+ (static_cast<size_t>(blockY) * blocksX + blockX) * blockBytes;
						switch (format)
						{
							case TextureBlockFormat::Bc7:
							{
								bc7enc16_compress_block_params params;
								bc7enc16_compress_block_params_init(&params);
								if (!srgb)
									bc7enc16_compress_block_params_init_linear_weights(&params);
								bc7enc16_compress_block(target, block, &params);
								break;
							}
							case TextureBlockFormat::Bc5:
								rgbcx::encode_bc5(target, block, 0, 1, 4);
								break;
							case TextureBlockFormat::Bc4:
								rgbcx::encode_bc4(target, block, 4);
								break;
							case TextureBlockFormat::Bc1:
								rgbcx::encode_bc1(target, block, 0);
								break;
							case TextureBlockFormat::Bc3:
								rgbcx::encode_bc3(target, block);
								break;
							default:
								error = "unsupported block format";
								return false;
						}
					}
				}
				return true;
			}

			if (format == TextureBlockFormat::Rgba8)
			{
				out = image.Pixels;
				return true;
			}
			error = std::string("unsupported format ") + TextureBlockFormatName(format);
			return false;
		}

		bool WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
			std::string& error)
		{
			std::error_code directoryError;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), directoryError);
			const std::filesystem::path temporary = path.string() + ".tmp-write";
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				if (!output.is_open())
				{
					error = "cannot write " + temporary.generic_string();
					return false;
				}
				output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			}
			std::error_code renameError;
			std::filesystem::rename(temporary, path, renameError);
			if (renameError)
			{
				std::error_code removeError;
				std::filesystem::remove(path, removeError);
				renameError.clear();
				std::filesystem::rename(temporary, path, renameError);
			}
			if (renameError)
			{
				error = "cannot replace " + path.generic_string() + ": " + renameError.message();
				std::error_code cleanupError;
				std::filesystem::remove(temporary, cleanupError);
				return false;
			}
			return true;
		}

		bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out,
			std::string& error)
		{
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input.is_open())
			{
				error = "cannot open " + path.generic_string();
				return false;
			}
			const std::streamsize size = input.tellg();
			input.seekg(0, std::ios::beg);
			out.resize(static_cast<size_t>(std::max<std::streamsize>(0, size)));
			if (!out.empty())
				input.read(reinterpret_cast<char*>(out.data()), size);
			return true;
		}

		TextureBlockFormat BlockFormatFor(TextureCompression compression)
		{
			switch (compression)
			{
				case TextureCompression::BC7: return TextureBlockFormat::Bc7;
				case TextureCompression::BC5: return TextureBlockFormat::Bc5;
				case TextureCompression::BC4: return TextureBlockFormat::Bc4;
				case TextureCompression::BC1: return TextureBlockFormat::Bc1;
				case TextureCompression::BC3: return TextureBlockFormat::Bc3;
				case TextureCompression::None:
				case TextureCompression::Auto:
				default:
					return TextureBlockFormat::Rgba8;
			}
		}

		std::string LowerExtension(const std::filesystem::path& path)
		{
			std::string extension = path.extension().generic_string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return extension;
		}
	}

	bool TextureCompiler::IsTextureSourceExtension(const std::string& extension)
	{
		return extension == ".png" || extension == ".jpg" || extension == ".jpeg"
			|| extension == ".tga" || extension == ".bmp";
	}

	bool TextureCompiler::BakeBytes(const std::vector<uint8_t>& sourceBytes,
		const TextureImportSettings& settings, std::vector<uint8_t>& outArtifact,
		TextureArtifactHeader& outHeader, std::string& error)
	{
		error.clear();
		outArtifact.clear();
		outHeader = TextureArtifactHeader {};
		if (sourceBytes.empty())
		{
			error = "empty source";
			return false;
		}
		EnsureEncoderInit();

		ImageRGBA8 image;
		if (!DecodeImage(sourceBytes, settings.FlipY, image, error))
			return false;
		if (image.Width > 65535 || image.Height > 65535)
		{
			error = "texture larger than 65535 px is not supported by the artifact format";
			return false;
		}

		const bool srgb = settings.EffectiveSrgb();
		const TextureCompression compression = settings.EffectiveCompression();
		const TextureBlockFormat format = BlockFormatFor(compression);
		ResizeToMaxSize(image, settings.MaxSize, srgb);
		if (settings.PremultiplyAlpha)
			PremultiplyAlpha(image);

		const bool wantMips = settings.EffectiveMipmaps();
		const uint32_t mipCount = wantMips ? TextureArtifactHeader::MipCountFor(image.Width, image.Height) : 1;

		TextureArtifactHeader header;
		header.Format = format;
		// BC4/BC5 只有 1~2 个通道,sRGB 标志对它们没有意义。
		header.Srgb = srgb && format != TextureBlockFormat::Bc4 && format != TextureBlockFormat::Bc5;
		header.Premultiplied = settings.PremultiplyAlpha;
		header.Width = image.Width;
		header.Height = image.Height;
		header.MipCount = mipCount;
		header.Wrap = static_cast<uint32_t>(settings.Wrap);
		header.Filter = static_cast<uint32_t>(settings.Filter);
		header.Anisotropy = std::clamp(settings.Anisotropy, 1u, 16u);
		header.Usage = static_cast<uint32_t>(settings.Usage);
		header.SettingsHash = settings.Hash();
		const Crypto::Sha256Digest digest = Crypto::Sha256(sourceBytes);
		std::memcpy(header.SourceSha256, digest.Bytes, sizeof(digest.Bytes));

		std::vector<uint8_t> payload;
		ImageRGBA8 current = image;
		ImageRGBA8 next;
		for (uint32_t mip = 0; mip < mipCount; ++mip)
		{
			if (mip > 0)
			{
				DownsampleBox(current, next, srgb);
				current = next;
			}
			std::vector<uint8_t> encoded;
			if (!EncodeMip(format, current, header.Srgb, encoded, error))
			{
				error = "mip " + std::to_string(mip) + ": " + error;
				return false;
			}
			header.Mips[mip].Offset = payload.size();
			header.Mips[mip].Size = encoded.size();
			payload.insert(payload.end(), encoded.begin(), encoded.end());
		}
		header.DataSize = payload.size();
		if (!BuildTextureArtifact(header, payload, outArtifact, error))
			return false;
		outHeader = header;
		return true;
	}

	bool TextureCompiler::BakeFile(const std::filesystem::path& sourceFile,
		const TextureImportSettings& settings, std::vector<uint8_t>& outArtifact,
		TextureArtifactHeader& outHeader, std::string& error)
	{
		std::vector<uint8_t> sourceBytes;
		if (!ReadFileBytes(sourceFile, sourceBytes, error))
			return false;
		return BakeBytes(sourceBytes, settings, outArtifact, outHeader, error);
	}

	TextureBakeStats TextureCompiler::BakeDirectory(const std::filesystem::path& sourceRoot,
		const std::filesystem::path& outputRoot, const std::filesystem::path& cacheDir,
		const TextureBakeOptions& options)
	{
		TextureBakeStats stats;
		std::error_code rootError;
		if (!std::filesystem::is_directory(sourceRoot, rootError))
		{
			stats.Errors.push_back("source root is not a directory: " + sourceRoot.generic_string());
			return stats;
		}

		// M4-TEX P0(资产形态):设置的家是**资产** `<主名>.wtex`(与 .wmodel 同构),
		// 不是旁路 sidecar。一轮烘焙 = ① 遍历资产(用资产里的设置)→ ② 没有资产的源图按默认设置。
		// 产物一律按**源图**逻辑路径命名(`textures/Icon.png.wtexc`),运行时只有一条查找规则。
		std::vector<std::filesystem::path> assets;
		std::vector<std::filesystem::path> images;
		for (std::filesystem::recursive_directory_iterator iterator(sourceRoot,
				std::filesystem::directory_options::skip_permission_denied, rootError), end;
			iterator != end; iterator.increment(rootError))
		{
			if (rootError)
				break;
			if (!iterator->is_regular_file(rootError))
				continue;
			const std::string extension = LowerExtension(iterator->path());
			if (extension == ".wtex")
				assets.push_back(iterator->path());
			else if (IsTextureSourceExtension(extension))
				images.push_back(iterator->path());
		}
		std::sort(assets.begin(), assets.end());
		std::sort(images.begin(), images.end());

		struct BakeItem
		{
			std::filesystem::path Source;
			std::filesystem::path Relative;
			TextureImportSettings Settings;
			bool FromAsset = false;
		};
		std::vector<BakeItem> items;
		std::vector<std::string> claimed;

		for (const std::filesystem::path& asset : assets)
		{
			TextureImportSettings settings;
			std::string error;
			if (!LoadTextureImportSettings(asset, settings, error))
			{
				++stats.Failed;
				stats.Errors.push_back(error);
				continue;
			}

			std::filesystem::path source;
			if (!settings.Source.empty())
			{
				// 显式 source:内容根相对路径;不许绝对路径、不许 ".." 逃出内容根。
				const std::filesystem::path declared(settings.Source);
				bool escapes = declared.is_absolute() || declared.has_root_name();
				for (const auto& part : declared)
					if (part == "..")
						escapes = true;
				if (escapes)
				{
					++stats.Failed;
					stats.Errors.push_back(asset.generic_string()
						+ ": source must be a content-root relative path without '..'");
					continue;
				}
				source = sourceRoot / declared;
				if (!IsTextureSourceExtension(LowerExtension(source)))
				{
					++stats.Failed;
					stats.Errors.push_back(asset.generic_string() + ": source '" + settings.Source
						+ "' is not a supported image extension");
					continue;
				}
			}
			else
			{
				// 缺省:同目录、同主名的图片(按固定扩展名顺序找第一个存在的)。
				static const char* const kCandidates[] =
					{ ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
				for (const char* extension : kCandidates)
				{
					const std::filesystem::path candidate =
						asset.parent_path() / (asset.stem().string() + extension);
					std::error_code existsError;
					if (std::filesystem::is_regular_file(candidate, existsError))
					{
						source = candidate;
						break;
					}
				}
			}
			std::error_code existsError;
			if (source.empty() || !std::filesystem::is_regular_file(source, existsError))
			{
				++stats.Failed;
				stats.Errors.push_back(asset.generic_string()
					+ ": no source image (put the image next to the asset or set `source:`)");
				continue;
			}

			std::error_code relativeError;
			const std::filesystem::path relative =
				std::filesystem::relative(source, sourceRoot, relativeError);
			if (relativeError || relative.empty())
			{
				++stats.Failed;
				stats.Errors.push_back("cannot relativize " + source.generic_string());
				continue;
			}
			const std::string logical = relative.generic_string();
			if (std::find(claimed.begin(), claimed.end(), logical) != claimed.end())
			{
				++stats.Failed;
				stats.Errors.push_back("two texture assets point at the same source: " + logical);
				continue;
			}
			claimed.push_back(logical);
			items.push_back(BakeItem { source, relative, settings, true });
		}

		for (const std::filesystem::path& image : images)
		{
			// 同主名资产在场 ⇒ 这张源图归资产管:资产坏就**失败在资产上**,
			// 不在这里用默认设置偷偷烘一份(否则"坏资产"会变成静默的错误贴图)。
			const std::filesystem::path siblingAsset =
				image.parent_path() / (image.stem().string() + ".wtex");
			std::error_code siblingError;
			if (std::filesystem::is_regular_file(siblingAsset, siblingError))
				continue;

			std::error_code relativeError;
			const std::filesystem::path relative =
				std::filesystem::relative(image, sourceRoot, relativeError);
			if (relativeError || relative.empty())
			{
				++stats.Failed;
				stats.Errors.push_back("cannot relativize " + image.generic_string());
				continue;
			}
			const std::string logical = relative.generic_string();
			if (std::find(claimed.begin(), claimed.end(), logical) != claimed.end())
				continue;   // 已由资产接管
			items.push_back(BakeItem { image, relative, TextureImportSettings {}, false });
		}

		for (const BakeItem& item : items)
		{
			const std::filesystem::path& source = item.Source;
			const TextureImportSettings& settings = item.Settings;
			std::string error;

			std::vector<uint8_t> sourceBytes;
			if (!ReadFileBytes(source, sourceBytes, error) || sourceBytes.empty())
			{
				++stats.Skipped;
				stats.Errors.push_back("empty/unreadable source: " + source.generic_string());
				continue;
			}
			const Crypto::Sha256Digest digest = Crypto::Sha256(sourceBytes);
			const std::string cacheKey = Crypto::Sha256Hex(digest) + "-"
				+ std::to_string(settings.Hash()) + "-v" + std::to_string(kTextureArtifactVersion) + ".wtexc";
			const std::filesystem::path cachePath = cacheDir.empty() ? std::filesystem::path()
				: cacheDir / cacheKey;
			const std::filesystem::path outputPath =
				outputRoot / std::filesystem::path(item.Relative.generic_string() + ".wtexc");

			std::vector<uint8_t> artifact;
			bool reused = false;
			if (!options.Force && !cachePath.empty())
			{
				std::error_code existsError;
				if (std::filesystem::exists(cachePath, existsError)
					&& ReadFileBytes(cachePath, artifact, error) && !artifact.empty())
				{
					reused = true;
				}
				else
				{
					artifact.clear();
				}
			}

			if (!reused)
			{
				TextureArtifactHeader header;
				if (!BakeBytes(sourceBytes, settings, artifact, header, error))
				{
					++stats.Failed;
					stats.Errors.push_back(source.generic_string() + ": " + error);
					continue;
				}
				if (!cachePath.empty() && !WriteFileBytes(cachePath, artifact, error))
				{
					++stats.Failed;
					stats.Errors.push_back(error);
					continue;
				}
				++stats.Baked;
			}
			else
			{
				++stats.UpToDate;
			}

			// 产物与已存在的逐字节一致时不重写(增量日志干净,时间戳不抖)。
			std::error_code outputError;
			if (std::filesystem::exists(outputPath, outputError))
			{
				std::vector<uint8_t> existing;
				std::string readError;
				if (ReadFileBytes(outputPath, existing, readError) && existing == artifact)
					continue;
			}
			if (!WriteFileBytes(outputPath, artifact, error))
			{
				++stats.Failed;
				stats.Errors.push_back(error);
			}
		}
		return stats;
	}
}
