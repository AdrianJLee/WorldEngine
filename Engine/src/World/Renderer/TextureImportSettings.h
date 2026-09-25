#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace World
{
	// M4-TEX P0:`<源文件主名>.wtex` = **项目侧纹理资产**(YAML):持有该纹理的导入设置,
	// 并指向它的源图(`source:`,缺省 = 同目录同名图片)。
	//
	// 与引擎的模型管线同构(见 knowledge/traps/wimport-removed.md):
	//   源(`.png`/`.jpg`/…) → 资产(`.wtex`,项目侧唯一设置真相)→ 平台产物(`cooked/**.wtexc`)。
	// **不存在旁路设置文件**:设置只能改在资产里;任何时刻都能改(编辑器面板 / 文本 / CI),
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
		// 源图路径:**相对内容根**(与材质引用同一套写法),例如 `textures/Icon.png`。
		// 空 = 用"同目录、同主名"的图片(按 .png/.jpg/.jpeg/.tga/.bmp 依次找)。
		std::string Source;
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

	// 资产路径 = 源逻辑路径换扩展名(Icon.png → Icon.wtex;jpg/tga/bmp 同理)。
	WLD_API std::string TextureAssetPathForSource(const std::string& sourcePath);
	// 该路径是否看起来是纹理资产(`.wtex`)。
	WLD_API bool IsTextureAssetPath(const std::string& path);

	// 磁盘读写(由调用方给绝对路径:烘焙器用内容根拼,编辑器面板同理)。
	// 缺文件 = 返回默认值且 ok=true(删除 sidecar 等价于"回到自动默认")。
	WLD_API bool LoadTextureImportSettings(const std::filesystem::path& sidecarPath,
		TextureImportSettings& out, std::string& error);
	WLD_API bool SaveTextureImportSettings(const std::filesystem::path& sidecarPath,
		const TextureImportSettings& settings, std::string& error);
}
