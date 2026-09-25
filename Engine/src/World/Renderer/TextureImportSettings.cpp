#include "wldpch.h"

#include "World/Renderer/TextureImportSettings.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace World
{
	namespace
	{
		const char* const kUsageNames[] = { "color", "normal", "data", "hdr", "ui" };
		const char* const kCompressionNames[] = { "auto", "none", "bc7", "bc5", "bc4", "bc1", "bc3" };
		const char* const kWrapNames[] = { "repeat", "clamp", "mirror" };
		const char* const kFilterNames[] = { "point", "bilinear", "trilinear" };
		const char* const kMipFilterNames[] = { "box", "gamma-correct" };

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		template <typename Enum>
		bool EnumFromName(const char* const* names, size_t count, const std::string& raw,
			Enum& out, const char* field, std::string& error)
		{
			const std::string value = Lower(raw);
			for (size_t index = 0; index < count; ++index)
				if (value == names[index])
				{
					out = static_cast<Enum>(index);
					return true;
				}
			std::string allowed;
			for (size_t index = 0; index < count; ++index)
			{
				if (!allowed.empty())
					allowed += " / ";
				allowed += names[index];
			}
			error = std::string(field) + ": unknown value '" + raw + "' (expected " + allowed + ")";
			return false;
		}

		bool ReadBool(const YAML::Node& node, const char* field, bool& out, std::string& error)
		{
			if (!node.IsScalar())
			{
				error = std::string(field) + ": expected a boolean";
				return false;
			}
			const std::string value = Lower(node.as<std::string>());
			if (value == "true" || value == "yes" || value == "on" || value == "1")
			{
				out = true;
				return true;
			}
			if (value == "false" || value == "no" || value == "off" || value == "0")
			{
				out = false;
				return true;
			}
			error = std::string(field) + ": expected true/false, got '" + value + "'";
			return false;
		}

		bool ReadUint(const YAML::Node& node, const char* field, uint32_t min, uint32_t max,
			uint32_t& out, std::string& error)
		{
			if (!node.IsScalar())
			{
				error = std::string(field) + ": expected an integer";
				return false;
			}
			uint32_t value = 0;
			try
			{
				value = node.as<uint32_t>();
			}
			catch (const std::exception&)
			{
				error = std::string(field) + ": expected an integer, got '" + node.as<std::string>() + "'";
				return false;
			}
			if (value < min || value > max)
			{
				error = std::string(field) + ": out of range [" + std::to_string(min) + ", "
					+ std::to_string(max) + "], got " + std::to_string(value);
				return false;
			}
			out = value;
			return true;
		}

		// 已知字段表:未知字段报错(拼错字段名不能静默变成"默认行为")。
		const std::unordered_map<std::string, bool>& KnownFields()
		{
			static const std::unordered_map<std::string, bool> fields = {
				{ "source", true },
				{ "usage", true }, { "srgb", true }, { "compression", true },
				{ "mipmaps", true }, { "mip_filter", true }, { "max_size", true },
				{ "wrap", true }, { "filter", true }, { "anisotropy", true },
				{ "premultiply_alpha", true }, { "flip_y", true },
			};
			return fields;
		}
	}

	const char* TextureUsageName(TextureUsage usage)
	{
		const size_t index = static_cast<size_t>(usage);
		return index < std::size(kUsageNames) ? kUsageNames[index] : "color";
	}

	const char* TextureCompressionName(TextureCompression compression)
	{
		const size_t index = static_cast<size_t>(compression);
		return index < std::size(kCompressionNames) ? kCompressionNames[index] : "auto";
	}

	const char* TextureWrapName(TextureWrap wrap)
	{
		const size_t index = static_cast<size_t>(wrap);
		return index < std::size(kWrapNames) ? kWrapNames[index] : "repeat";
	}

	const char* TextureFilterName(TextureFilter filter)
	{
		const size_t index = static_cast<size_t>(filter);
		return index < std::size(kFilterNames) ? kFilterNames[index] : "trilinear";
	}

	const char* TextureMipFilterName(TextureMipFilter filter)
	{
		const size_t index = static_cast<size_t>(filter);
		return index < std::size(kMipFilterNames) ? kMipFilterNames[index] : "gamma-correct";
	}

	bool TextureImportSettings::Parse(const std::string& text, TextureImportSettings& out,
		std::string& error)
	{
		out = TextureImportSettings {};
		error.clear();
		if (text.find_first_not_of(" \t\r\n") == std::string::npos)
			return true;   // 空文件 = 全默认

		YAML::Node root;
		try
		{
			root = YAML::Load(text);
		}
		catch (const std::exception& exception)
		{
			error = std::string("yaml: ") + exception.what();
			return false;
		}
		if (!root.IsMap())
		{
			error = "root must be a map of texture settings";
			return false;
		}
		for (const auto& entry : root)
		{
			if (!entry.first.IsScalar())
			{
				error = "field names must be scalars";
				return false;
			}
			const std::string key = Lower(entry.first.as<std::string>());
			if (!KnownFields().count(key))
			{
				error = "unknown field '" + key + "' (see docs/dev/texture-import.md)";
				return false;
			}
			const YAML::Node& value = entry.second;
			if (!value.IsScalar())
			{
				// 结构性错误要在"字段类型转换抛异常"之前拦下来:坏 .wtex 不能崩 cook。
				error = key + ": expected a scalar value";
				return false;
			}
			if (key == "source")
			{
				out.Source = value.as<std::string>();
				if (out.Source.empty())
				{
					error = "source: must not be empty (omit the field to use the sibling image)";
					return false;
				}
			}
			else if (key == "usage")
			{
				if (!EnumFromName(kUsageNames, std::size(kUsageNames), value.as<std::string>(),
						out.Usage, "usage", error))
					return false;
			}
			else if (key == "srgb")
			{
				if (!ReadBool(value, "srgb", out.Srgb, error))
					return false;
				out.SrgbExplicit = true;
			}
			else if (key == "compression")
			{
				if (!EnumFromName(kCompressionNames, std::size(kCompressionNames),
						value.as<std::string>(), out.Compression, "compression", error))
					return false;
			}
			else if (key == "mipmaps")
			{
				if (!ReadBool(value, "mipmaps", out.Mipmaps, error))
					return false;
				out.MipmapsExplicit = true;
			}
			else if (key == "mip_filter")
			{
				if (!EnumFromName(kMipFilterNames, std::size(kMipFilterNames), value.as<std::string>(),
						out.MipFilter, "mip_filter", error))
					return false;
			}
			else if (key == "max_size")
			{
				if (!ReadUint(value, "max_size", 0, 16384, out.MaxSize, error))
					return false;
				if (out.MaxSize != 0 && out.MaxSize < 4)
				{
					error = "max_size: must be 0 (unlimited) or >= 4";
					return false;
				}
			}
			else if (key == "wrap")
			{
				if (!EnumFromName(kWrapNames, std::size(kWrapNames), value.as<std::string>(),
						out.Wrap, "wrap", error))
					return false;
			}
			else if (key == "filter")
			{
				if (!EnumFromName(kFilterNames, std::size(kFilterNames), value.as<std::string>(),
						out.Filter, "filter", error))
					return false;
			}
			else if (key == "anisotropy")
			{
				if (!ReadUint(value, "anisotropy", 1, 16, out.Anisotropy, error))
					return false;
			}
			else if (key == "premultiply_alpha")
			{
				if (!ReadBool(value, "premultiply_alpha", out.PremultiplyAlpha, error))
					return false;
			}
			else if (key == "flip_y")
			{
				if (!ReadBool(value, "flip_y", out.FlipY, error))
					return false;
			}
		}
		return true;
	}

	std::string TextureImportSettings::Serialize() const
	{
		std::ostringstream out;
		out << "# Texture asset (see docs/dev/texture-import.md): import settings + source image.\n";
		out << "# Re-editable at any time: edit here or use the editor's Texture Settings panel,\n";
		out << "# then re-bake (cook / live preview). Delete this file to fall back to defaults.\n";
		if (!Source.empty())
			out << "# source: content-root relative path of the image (default = sibling with same stem).\n";
		if (!Source.empty())
			out << "source: \"" << Source << "\"\n";
		out << "usage: " << TextureUsageName(Usage) << "\n";
		if (SrgbExplicit)
			out << "srgb: " << (Srgb ? "true" : "false") << "\n";
		if (Compression != TextureCompression::Auto)
			out << "compression: " << TextureCompressionName(Compression) << "\n";
		if (MipmapsExplicit)
			out << "mipmaps: " << (Mipmaps ? "true" : "false") << "\n";
		if (MipFilter != TextureMipFilter::GammaCorrect)
			out << "mip_filter: " << TextureMipFilterName(MipFilter) << "\n";
		if (MaxSize != 0)
			out << "max_size: " << MaxSize << "\n";
		if (Wrap != TextureWrap::Repeat)
			out << "wrap: " << TextureWrapName(Wrap) << "\n";
		if (Filter != TextureFilter::Trilinear)
			out << "filter: " << TextureFilterName(Filter) << "\n";
		if (Anisotropy != 4)
			out << "anisotropy: " << Anisotropy << "\n";
		if (PremultiplyAlpha)
			out << "premultiply_alpha: true\n";
		if (FlipY)
			out << "flip_y: true\n";
		return out.str();
	}

	bool TextureImportSettings::EffectiveSrgb() const
	{
		if (SrgbExplicit)
			return Srgb;
		switch (Usage)
		{
			case TextureUsage::Color:
			case TextureUsage::Ui:
				return true;
			case TextureUsage::Normal:
			case TextureUsage::Data:
			case TextureUsage::Hdr:
			default:
				return false;
		}
	}

	bool TextureImportSettings::EffectiveMipmaps() const
	{
		if (MipmapsExplicit)
			return Mipmaps;
		return Usage != TextureUsage::Ui;
	}

	TextureCompression TextureImportSettings::EffectiveCompression() const
	{
		if (Compression != TextureCompression::Auto)
			return Compression;
		switch (Usage)
		{
			case TextureUsage::Color:
				return TextureCompression::BC7;
			case TextureUsage::Normal:
				return TextureCompression::BC5;
			case TextureUsage::Data:
				return TextureCompression::BC4;
			case TextureUsage::Hdr:
			case TextureUsage::Ui:
			default:
				return TextureCompression::None;
		}
	}

	uint64_t TextureImportSettings::Hash() const
	{
		// FNV1a64 over 生效值的规范化文本(显式但与默认等效的字段不改变键)。
		// 注意:Source **不进**键 —— 源图的字节 hash 才是缓存键的一部分(改指向另一张图 ⇒ 源 sha 变)。
		std::string canonical;
		canonical += TextureUsageName(Usage);
		canonical += EffectiveSrgb() ? "|srgb" : "|linear";
		canonical += "|";
		canonical += TextureCompressionName(EffectiveCompression());
		canonical += EffectiveMipmaps() ? "|mips" : "|nomips";
		canonical += "|";
		canonical += TextureMipFilterName(MipFilter);
		canonical += "|max" + std::to_string(MaxSize);
		canonical += "|";
		canonical += TextureWrapName(Wrap);
		canonical += "|";
		canonical += TextureFilterName(Filter);
		canonical += "|aniso" + std::to_string(Anisotropy);
		canonical += PremultiplyAlpha ? "|premul" : "";
		canonical += FlipY ? "|flipy" : "";

		uint64_t hash = 1469598103934665603ull;
		for (const char character : canonical)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	bool IsTextureAssetPath(const std::string& path)
	{
		return path.size() > 5 && path.compare(path.size() - 5, 5, ".wtex") == 0;
	}

	std::string TextureAssetPathForSource(const std::string& sourcePath)
	{
		// 只换最后一段的扩展名(textures/Icon.png → textures/Icon.wtex);
		// 没有扩展名的源(少见)直接加后缀,调用方按内容根解析。
		const size_t slash = sourcePath.find_last_of('/');
		const size_t dot = sourcePath.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return sourcePath + ".wtex";
		return sourcePath.substr(0, dot) + ".wtex";
	}

	bool LoadTextureImportSettings(const std::filesystem::path& sidecarPath,
		TextureImportSettings& out, std::string& error)
	{
		out = TextureImportSettings {};
		error.clear();
		std::error_code existsError;
		if (!std::filesystem::exists(sidecarPath, existsError))
			return true;   // 没有 sidecar = 默认设置

		std::ifstream input(sidecarPath, std::ios::binary);
		if (!input.is_open())
		{
			error = "cannot open " + sidecarPath.generic_string();
			return false;
		}
		std::ostringstream buffer;
		buffer << input.rdbuf();
		if (!TextureImportSettings::Parse(buffer.str(), out, error))
		{
			error = sidecarPath.generic_string() + ": " + error;
			return false;
		}
		return true;
	}

	bool SaveTextureImportSettings(const std::filesystem::path& sidecarPath,
		const TextureImportSettings& settings, std::string& error)
	{
		error.clear();
		std::error_code directoryError;
		if (!sidecarPath.parent_path().empty())
			std::filesystem::create_directories(sidecarPath.parent_path(), directoryError);

		const std::filesystem::path temporary = sidecarPath.string() + ".tmp-write";
		{
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			if (!output.is_open())
			{
				error = "cannot write " + temporary.generic_string();
				return false;
			}
			output << settings.Serialize();
		}
		std::error_code renameError;
		std::filesystem::rename(temporary, sidecarPath, renameError);
		if (renameError)
		{
			std::error_code removeError;
			std::filesystem::remove(sidecarPath, removeError);
			renameError.clear();
			std::filesystem::rename(temporary, sidecarPath, renameError);
		}
		if (renameError)
		{
			error = "cannot replace " + sidecarPath.generic_string() + ": " + renameError.message();
			std::error_code cleanupError;
			std::filesystem::remove(temporary, cleanupError);
			return false;
		}
		return true;
	}

	namespace
	{
		// 头/payload 分界:`---payload` 必须**独占一行**(允许行尾 \r)。
		size_t FindPayloadMarker(const std::string& text, size_t* payloadStart)
		{
			const std::string marker = kTextureAssetPayloadMarker;
			size_t offset = 0;
			while (offset <= text.size())
			{
				const size_t lineEnd = text.find('\n', offset);
				const size_t end = lineEnd == std::string::npos ? text.size() : lineEnd;
				std::string_view line(text.data() + offset, end - offset);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				if (line == marker)
				{
					if (payloadStart)
						*payloadStart = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
					return offset;
				}
				if (lineEnd == std::string::npos)
					break;
				offset = lineEnd + 1;
			}
			return std::string::npos;
		}
	}

	bool LoadTextureAssetFile(const std::filesystem::path& path, TextureAssetFile& out,
		std::string& error)
	{
		out = TextureAssetFile {};
		error.clear();
		std::vector<uint8_t> bytes;
		{
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			if (!input.is_open())
			{
				error = "cannot open " + path.generic_string();
				return false;
			}
			const std::streamoff size = input.tellg();
			input.seekg(0);
			bytes.resize(static_cast<size_t>(std::max<std::streamoff>(0, size)));
			if (!bytes.empty())
				input.read(reinterpret_cast<char*>(bytes.data()), size);
		}

		const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		size_t payloadStart = text.size();
		const size_t markerAt = FindPayloadMarker(text, &payloadStart);
		const std::string header = markerAt == std::string::npos ? text : text.substr(0, markerAt);

		if (!TextureImportSettings::Parse(header, out.Settings, error))
		{
			error = path.generic_string() + ": " + error;
			return false;
		}
		if (markerAt == std::string::npos)
		{
			// legacy:纯设置文件 + `source:` 指向外部源图。
			out.LegacySource = out.Settings.Source;
			return true;
		}
		out.Payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payloadStart), bytes.end());
		return true;
	}

	bool SaveTextureAssetFile(const std::filesystem::path& path, const TextureAssetFile& asset,
		std::string& error)
	{
		error.clear();
		if (asset.Payload.empty())
		{
			error = "texture asset payload is empty (a .wtex container must embed the source bytes)";
			return false;
		}
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
			TextureImportSettings settings = asset.Settings;
			settings.Source.clear();   // 源图已内嵌,不再写 `source:`
			output << settings.Serialize();
			output << kTextureAssetPayloadMarker << "\n";
			output.write(reinterpret_cast<const char*>(asset.Payload.data()),
				static_cast<std::streamsize>(asset.Payload.size()));
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
}
