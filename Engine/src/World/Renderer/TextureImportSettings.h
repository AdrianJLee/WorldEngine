#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World
{
	// M4-TEX P0:纹理**源文件**的持久导入设置(sidecar `<源完整名>.wtex`,YAML)。
	//
	// 它不是一次性的"导入向导选项":任何时刻都能改(编辑器面板 / 文本 / CI 脚本),
	// 改完重烘(clamp 见 docs/dev/texture-import.md)。源图本身永远不动。
	enum class TextureUsage : uint8_t
	{
		Color = 0,   // 反照率/颜色:sRGB + BC7
		Normal = 1,  // 切线空间法线:R/G 通道,BC5 + 着色器重建 Z
		Data = 2,    // AO/粗糙度/金属度等数据:线性 + BC4/BC7
		Hdr = 3,     // 光照图/环境:v1 不压(无 RGBA16F 上传路径前按 Data 处理)
		Ui = 4,      // 图标/图集:RGBA8 不压、不生成 mip
	};

	enum class TextureCompression : uint8_t
	{
		Auto = 0,   // 按 usage 展开(推荐)
		None,       // RGBA8(不压)
		BC7,        // 高质量 8bpp,颜色/HDR 之外都可用
		BC5,        // 二通道 RGTC,法线
		BC4,        // 单通道 RGTC,数据
		BC1,        // 备用(低质量 4bpp)
		BC3,        // 备用(带 alpha,8bpp)
	};

	enum class TextureWrap : uint8_t { Repeat = 0, Clamp, Mirror };
	enum class TextureFilter : uint8_t { Point = 0, Bilinear, Trilinear };
	enum class TextureMipFilter : uint8_t
	{
		Box = 0,          // 直接盒式平均(线性数据用)
		GammaCorrect,     // 先转线性再平均(sRGB 贴图用;避免缩小后变暗)
	};

	struct WLD_API TextureImportSettings
	{
		TextureUsage Usage = TextureUsage::Color;
		// Srgb 只在显式写进 .wtex 时覆盖 usage 派生值(见 SrgbExplicit)。
		bool Srgb = true;
		bool SrgbExplicit = false;
		TextureCompression Compression = TextureCompression::Auto;
		bool Mipmaps = true;
		bool MipmapsExplicit = false;
		TextureMipFilter MipFilter = TextureMipFilter::GammaCorrect;
		uint32_t MaxSize = 0;                 // 0 = 不限制(>0 时按最长边等比缩到该值)
		TextureWrap Wrap = TextureWrap::Repeat;
		TextureFilter Filter = TextureFilter::Trilinear;
		uint32_t Anisotropy = 4;              // 1..16(运行时再按设备上限 clamp)
		bool PremultiplyAlpha = false;
		bool FlipY = false;                   // 内容贴图默认不翻转(UV 原点左上)

		// 解析 YAML(JSON 是 YAML 子集)。未知字段 = 错误(不静默丢设置);空文本 = 默认值。
		static bool Parse(const std::string& text, TextureImportSettings& out, std::string& error);
		// 只写**显式**字段(默认值不落盘),保持 sidecar 小而可读。
		std::string Serialize() const;

		// 生效值(usage 派生 + 显式覆盖)。烘焙与缓存键一律用这三个,不用裸字段。
		bool EffectiveSrgb() const;
		bool EffectiveMipmaps() const;
		TextureCompression EffectiveCompression() const;
		// 缓存键的一部分:对**生效值**做 FNV1a64(确定性;不是安全哈希)。
		uint64_t Hash() const;
	};

	WLD_API const char* TextureUsageName(TextureUsage usage);
	WLD_API const char* TextureCompressionName(TextureCompression compression);
	WLD_API const char* TextureWrapName(TextureWrap wrap);
	WLD_API const char* TextureFilterName(TextureFilter filter);
	WLD_API const char* TextureMipFilterName(TextureMipFilter filter);

	// sidecar 路径 = 源逻辑路径 + ".wtex"(Icon.png → Icon.png.wtex;jpg/tga 同理)。
	WLD_API std::string TextureSettingsPath(const std::string& sourcePath);

	// 磁盘读写(由调用方给绝对路径:烘焙器用内容根拼,编辑器面板同理)。
	// 缺文件 = 返回默认值且 ok=true(删除 sidecar 等价于"回到自动默认")。
	WLD_API bool LoadTextureImportSettings(const std::filesystem::path& sidecarPath,
		TextureImportSettings& out, std::string& error);
	WLD_API bool SaveTextureImportSettings(const std::filesystem::path& sidecarPath,
		const TextureImportSettings& settings, std::string& error);
}
