#pragma once

#include "World/Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// M4-TEX P1:烘焙产物 `.wtexc` —— 自描述头 + 逐 mip 块数据。
	//
	// 设计口径(见 docs/dev/texture-import.md):
	//   * 与后端无关:只记"块格式 + sRGB 标志 + 尺寸 + 采样状态",运行时映射到 RHI Format;
	//   * 逐 mip 上传:每级 offset/size 直接喂 `Rhi::Texture::SetData(..., mip)`;
	//   * 可校验:头里存源 sha256 + 设置 hash ⇒ "陈旧产物"能被发现(编辑器徽标 / cook 重烘)。
	enum class TextureBlockFormat : uint16_t
	{
		Rgba8 = 0,    // 未压缩 8bpp
		Bc7 = 1,      // 16 字节 / 4x4
		Bc5 = 2,      // 16 字节 / 4x4(法线:RG + 着色器重建 Z)
		Bc4 = 3,      // 8 字节 / 4x4
		Bc1 = 4,      // 8 字节 / 4x4
		Bc3 = 5,      // 16 字节 / 4x4
		Rgba16f = 6,  // 预留(HDR 上传路径)
	};

	WLD_API const char* TextureBlockFormatName(TextureBlockFormat format);
	WLD_API bool IsBlockCompressed(TextureBlockFormat format);
	WLD_API uint32_t TextureBlockBytes(TextureBlockFormat format);     // BC1/BC4=8,其余 BC=16,未压缩=0
	WLD_API uint32_t TextureBytesPerPixel(TextureBlockFormat format);  // Rgba8=4,Rgba16f=8,块格式=0

	constexpr uint32_t kTextureArtifactVersion = 1;
	constexpr uint32_t kTextureArtifactMagic = 0x58544C57u;   // 'WLTX'
	constexpr uint32_t kTextureArtifactHeaderSize = 336;
	constexpr uint32_t kTextureArtifactMaxMips = 16;

	struct TextureArtifactMip
	{
		uint64_t Offset = 0;   // 相对数据段起点
		uint64_t Size = 0;
	};

	struct WLD_API TextureArtifactHeader
	{
		TextureBlockFormat Format = TextureBlockFormat::Rgba8;
		bool Srgb = false;
		bool Premultiplied = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t MipCount = 1;
		uint32_t Wrap = 0;         // TextureWrap 的数值
		uint32_t Filter = 0;       // TextureFilter 的数值
		uint32_t Anisotropy = 1;
		uint32_t Usage = 0;        // TextureUsage 的数值
		uint64_t SettingsHash = 0;
		uint8_t SourceSha256[32] = {};
		uint64_t DataSize = 0;
		TextureArtifactMip Mips[kTextureArtifactMaxMips] {};

		// 该 mip 的尺寸与数据量(块格式按 4x4 向上取整;未压缩按像素)。
		void MipExtent(uint32_t mip, uint32_t& outWidth, uint32_t& outHeight) const;
		uint64_t MipDataSize(uint32_t mip) const;
		// 尺寸对应的完整 mip 链长度(含 mip0)。
		static uint32_t MipCountFor(uint32_t width, uint32_t height);
	};

	// 头 + 表校验(不碰数据段)。失败填 error(人话,带期望/实际)。
	WLD_API bool ParseTextureArtifactHeader(const uint8_t* bytes, size_t size,
		TextureArtifactHeader& out, std::string& error);
	// 整段产物:头 + 数据段边界都要在 size 内,且 mip 表与 dataSize 自洽。
	WLD_API bool ParseTextureArtifact(const std::vector<uint8_t>& bytes,
		TextureArtifactHeader& out, std::string& error);
	// 组装产物(调用方负责让 payload 与 header.Mips 一致;函数内复核并报错)。
	WLD_API bool BuildTextureArtifact(const TextureArtifactHeader& header,
		const std::vector<uint8_t>& payload, std::vector<uint8_t>& outBytes, std::string& error);
}
