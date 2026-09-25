#include "wldpch.h"

#include "World/Renderer/TextureArtifact.h"

#include <algorithm>
#include <cstring>

namespace World
{
	namespace
	{
		void WriteU16(uint8_t*& cursor, uint16_t value)
		{
			*cursor++ = static_cast<uint8_t>(value & 0xFF);
			*cursor++ = static_cast<uint8_t>((value >> 8) & 0xFF);
		}

		void WriteU32(uint8_t*& cursor, uint32_t value)
		{
			for (int shift = 0; shift < 32; shift += 8)
				*cursor++ = static_cast<uint8_t>((value >> shift) & 0xFF);
		}

		void WriteU64(uint8_t*& cursor, uint64_t value)
		{
			for (int shift = 0; shift < 64; shift += 8)
				*cursor++ = static_cast<uint8_t>((value >> shift) & 0xFF);
		}

		uint16_t ReadU16(const uint8_t*& cursor)
		{
			const uint16_t value = static_cast<uint16_t>(cursor[0] | (cursor[1] << 8));
			cursor += 2;
			return value;
		}

		uint32_t ReadU32(const uint8_t*& cursor)
		{
			uint32_t value = 0;
			for (int shift = 0; shift < 32; shift += 8)
				value |= static_cast<uint32_t>(*cursor++) << shift;
			return value;
		}

		uint64_t ReadU64(const uint8_t*& cursor)
		{
			uint64_t value = 0;
			for (int shift = 0; shift < 64; shift += 8)
				value |= static_cast<uint64_t>(*cursor++) << shift;
			return value;
		}

		uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
		{
			return (value + divisor - 1) / divisor;
		}
	}

	const char* TextureBlockFormatName(TextureBlockFormat format)
	{
		switch (format)
		{
			case TextureBlockFormat::Rgba8: return "rgba8";
			case TextureBlockFormat::Bc7: return "bc7";
			case TextureBlockFormat::Bc5: return "bc5";
			case TextureBlockFormat::Bc4: return "bc4";
			case TextureBlockFormat::Bc1: return "bc1";
			case TextureBlockFormat::Bc3: return "bc3";
			case TextureBlockFormat::Rgba16f: return "rgba16f";
			default: return "unknown";
		}
	}

	bool IsBlockCompressed(TextureBlockFormat format)
	{
		return format != TextureBlockFormat::Rgba8 && format != TextureBlockFormat::Rgba16f;
	}

	uint32_t TextureBlockBytes(TextureBlockFormat format)
	{
		switch (format)
		{
			case TextureBlockFormat::Bc1:
			case TextureBlockFormat::Bc4:
				return 8;
			case TextureBlockFormat::Bc3:
			case TextureBlockFormat::Bc5:
			case TextureBlockFormat::Bc7:
				return 16;
			default:
				return 0;
		}
	}

	uint32_t TextureBytesPerPixel(TextureBlockFormat format)
	{
		switch (format)
		{
			case TextureBlockFormat::Rgba8: return 4;
			case TextureBlockFormat::Rgba16f: return 8;
			default: return 0;
		}
	}

	void TextureArtifactHeader::MipExtent(uint32_t mip, uint32_t& outWidth,
		uint32_t& outHeight) const
	{
		outWidth = std::max(1u, Width >> mip);
		outHeight = std::max(1u, Height >> mip);
	}

	uint64_t TextureArtifactHeader::MipDataSize(uint32_t mip) const
	{
		uint32_t width = 0;
		uint32_t height = 0;
		MipExtent(mip, width, height);
		if (IsBlockCompressed(Format))
			return static_cast<uint64_t>(DivideRoundUp(width, 4)) * DivideRoundUp(height, 4)
				* TextureBlockBytes(Format);
		return static_cast<uint64_t>(width) * height * TextureBytesPerPixel(Format);
	}

	uint32_t TextureArtifactHeader::MipCountFor(uint32_t width, uint32_t height)
	{
		uint32_t levels = 1;
		uint32_t size = std::max(width, height);
		while (size > 1 && levels < kTextureArtifactMaxMips)
		{
			size >>= 1;
			++levels;
		}
		return levels;
	}

	bool ParseTextureArtifactHeader(const uint8_t* bytes, size_t size,
		TextureArtifactHeader& out, std::string& error)
	{
		error.clear();
		if (!bytes || size < kTextureArtifactHeaderSize)
		{
			error = "artifact too small for header (" + std::to_string(size) + " bytes)";
			return false;
		}
		const uint8_t* cursor = bytes;
		const uint32_t magic = ReadU32(cursor);
		if (magic != kTextureArtifactMagic)
		{
			error = "bad magic (not a .wtexc artifact)";
			return false;
		}
		const uint32_t version = ReadU32(cursor);
		if (version != kTextureArtifactVersion)
		{
			error = "unsupported artifact version " + std::to_string(version)
				+ " (expected " + std::to_string(kTextureArtifactVersion) + ")";
			return false;
		}
		const uint32_t headerSize = ReadU32(cursor);
		if (headerSize != kTextureArtifactHeaderSize)
		{
			error = "unsupported header size " + std::to_string(headerSize);
			return false;
		}

		TextureArtifactHeader header;
		header.Format = static_cast<TextureBlockFormat>(ReadU16(cursor));
		const uint16_t flags = ReadU16(cursor);
		header.Srgb = (flags & 0x1u) != 0;
		header.Premultiplied = (flags & 0x2u) != 0;
		header.Width = ReadU16(cursor);
		header.Height = ReadU16(cursor);
		header.MipCount = cursor[0];
		header.Wrap = cursor[1];
		header.Filter = cursor[2];
		header.Anisotropy = cursor[3];
		header.Usage = cursor[4];
		cursor += 5;
		cursor += 3;   // reserved
		header.SettingsHash = ReadU64(cursor);
		std::memcpy(header.SourceSha256, cursor, 32);
		cursor += 32;
		header.DataSize = ReadU64(cursor);
		for (uint32_t mip = 0; mip < kTextureArtifactMaxMips; ++mip)
			header.Mips[mip].Offset = ReadU64(cursor);
		for (uint32_t mip = 0; mip < kTextureArtifactMaxMips; ++mip)
			header.Mips[mip].Size = ReadU64(cursor);

		if (header.Width == 0 || header.Height == 0)
		{
			error = "artifact has zero extent";
			return false;
		}
		if (header.MipCount == 0 || header.MipCount > kTextureArtifactMaxMips)
		{
			error = "bad mip count " + std::to_string(header.MipCount);
			return false;
		}
		if (header.Format > TextureBlockFormat::Rgba16f)
		{
			error = "unknown block format " + std::to_string(static_cast<int>(header.Format));
			return false;
		}
		out = header;
		return true;
	}

	bool ParseTextureArtifact(const std::vector<uint8_t>& bytes, TextureArtifactHeader& out,
		std::string& error)
	{
		TextureArtifactHeader header;
		if (!ParseTextureArtifactHeader(bytes.data(), bytes.size(), header, error))
			return false;
		if (kTextureArtifactHeaderSize + header.DataSize > bytes.size())
		{
			error = "artifact truncated: header says " + std::to_string(header.DataSize)
				+ " data bytes, file has " + std::to_string(bytes.size() - kTextureArtifactHeaderSize);
			return false;
		}
		for (uint32_t mip = 0; mip < header.MipCount; ++mip)
		{
			const TextureArtifactMip& entry = header.Mips[mip];
			const uint64_t expected = header.MipDataSize(mip);
			if (entry.Size != expected)
			{
				error = "mip " + std::to_string(mip) + " size mismatch: got "
					+ std::to_string(entry.Size) + ", expected " + std::to_string(expected);
				return false;
			}
			if (entry.Offset + entry.Size > header.DataSize)
			{
				error = "mip " + std::to_string(mip) + " runs past the data section";
				return false;
			}
		}
		out = header;
		return true;
	}

	bool BuildTextureArtifact(const TextureArtifactHeader& header,
		const std::vector<uint8_t>& payload, std::vector<uint8_t>& outBytes, std::string& error)
	{
		error.clear();
		if (header.Width == 0 || header.Height == 0)
		{
			error = "artifact has zero extent";
			return false;
		}
		if (header.MipCount == 0 || header.MipCount > kTextureArtifactMaxMips)
		{
			error = "bad mip count " + std::to_string(header.MipCount);
			return false;
		}
		uint64_t expectedOffset = 0;
		for (uint32_t mip = 0; mip < header.MipCount; ++mip)
		{
			const uint64_t expectedSize = header.MipDataSize(mip);
			if (header.Mips[mip].Size != expectedSize || header.Mips[mip].Offset != expectedOffset)
			{
				error = "mip " + std::to_string(mip) + " table is inconsistent with the header";
				return false;
			}
			expectedOffset += expectedSize;
		}
		if (payload.size() != expectedOffset)
		{
			error = "payload size " + std::to_string(payload.size()) + " != expected "
				+ std::to_string(expectedOffset);
			return false;
		}

		TextureArtifactHeader written = header;
		written.DataSize = payload.size();
		outBytes.assign(kTextureArtifactHeaderSize, 0);
		uint8_t* cursor = outBytes.data();
		WriteU32(cursor, kTextureArtifactMagic);
		WriteU32(cursor, kTextureArtifactVersion);
		WriteU32(cursor, kTextureArtifactHeaderSize);
		WriteU16(cursor, static_cast<uint16_t>(written.Format));
		uint16_t flags = 0;
		if (written.Srgb) flags |= 0x1u;
		if (written.Premultiplied) flags |= 0x2u;
		WriteU16(cursor, flags);
		WriteU16(cursor, static_cast<uint16_t>(written.Width));
		WriteU16(cursor, static_cast<uint16_t>(written.Height));
		*cursor++ = static_cast<uint8_t>(written.MipCount);
		*cursor++ = static_cast<uint8_t>(written.Wrap);
		*cursor++ = static_cast<uint8_t>(written.Filter);
		*cursor++ = static_cast<uint8_t>(written.Anisotropy);
		*cursor++ = static_cast<uint8_t>(written.Usage);
		cursor += 3;   // reserved
		WriteU64(cursor, written.SettingsHash);
		std::memcpy(cursor, written.SourceSha256, 32);
		cursor += 32;
		WriteU64(cursor, written.DataSize);
		for (uint32_t mip = 0; mip < kTextureArtifactMaxMips; ++mip)
			WriteU64(cursor, written.Mips[mip].Offset);
		for (uint32_t mip = 0; mip < kTextureArtifactMaxMips; ++mip)
			WriteU64(cursor, written.Mips[mip].Size);
		// 头固定 336 字节:上面写到 332,末尾 4 字节是对齐填充(保持 16 字节对齐,
		// 未来加字段从这里长,不移动数据段起点)。assert 只防"写表写到一半"的改动错误。
		WLD_CORE_ASSERT(static_cast<size_t>(cursor - outBytes.data()) <= kTextureArtifactHeaderSize,
			"texture artifact header write overflowed the fixed header size");
		outBytes.insert(outBytes.end(), payload.begin(), payload.end());
		return true;
	}
}
