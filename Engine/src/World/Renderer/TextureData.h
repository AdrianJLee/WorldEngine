#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/TextureArtifact.h"

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// 纹理 CPU 数据加载的**唯一入口**:VFS 优先 → 磁盘回退 → 1x1 白色兜底。
	// D3 把这段逻辑从 OpenGLTexture 抽出来,让 RHI 原生上传(3D 材质贴图)与
	// GL 纹理(2D/WUI)共用同一份查找与兜底策略,行为不会两边漂移。
	struct WLD_API TextureData
	{
		std::vector<uint8_t> Pixels;   // RGBA8,行序:第 0 行 = 图片顶部(未翻转)
		uint32_t Width = 1;
		uint32_t Height = 1;
		int Channels = 4;              // 原图通道数(1/2/3/4),供 GL 侧选择内部格式
		bool Valid = true;             // false = 文件缺失/损坏,内容为白色兜底并已打印警告
	};

	// flipVertically:GL 侧历史行为是 stbi_set_flip_vertically_on_load(1),
	// 即"上传后 UV (0,0) 在左下";RHI 原生的 3D 贴图按 Vulkan/GL 共同的左上原点约定,
	// 由调用方显式选择,避免依赖全局 stb 状态。
	WLD_API TextureData LoadTextureData(const std::string& path, bool flipVertically = false);

	// 与 LoadTextureData 同一套解码口径,输入是内存字节(单文件 `.wtex` 容器的内嵌图像)。
	WLD_API TextureData LoadTextureDataFromMemory(const std::vector<uint8_t>& bytes,
		bool flipVertically = false);

	// M4-TEX P6c:`<path>` 可以写源图(`textures/Icon.png`)或纹理资产(`textures/Icon.wtex`)。
	// 本函数把**资产引用**解析成它的源图路径(资产里的 `source:`,缺省 = 同目录同主名图片);
	// 源图引用原样返回。给"产物命中但建纹理失败 / 别处只想解码源图"的调用点用 ——
	// 直接拿 `.wtex` 去 stb 解码只会拿到 1x1 白兜底(实测)。
	WLD_API std::string ResolveTextureSourcePath(const std::string& path);

	// M4-TEX P2:产物优先的纹理加载结果(契约见 docs/dev/texture-import.md §7)。
	//
	// 解析口径与 LoadTextureData 同一套:逻辑路径 `<path>` → 先找 `<path>.wtexc`
	// (VFS 优先、磁盘回退),命中且头/逐 mip 表自洽 ⇒ FromArtifact=true,数据由
	// Bytes + Header.Mips 描述(每级数据 = Bytes[headerSize + offset, +size))。
	// 没有产物 / 产物坏 / 头不合法 ⇒ FromArtifact=false,走 Source(stb RGBA8 回退,
	// 行为与 P2 之前逐字节一致),坏产物只打一条警告,不抛异常。
	struct WLD_API TextureAsset
	{
		bool FromArtifact = false;
		TextureArtifactHeader Header {};   // FromArtifact 时有效
		std::vector<uint8_t> Bytes;        // 完整产物(头 + 数据段);FromArtifact 时非空
		TextureData Source;                // 回退路径的 stb 结果(FromArtifact=false 时有效)

		// 第 mip 级的数据指针/字节数(越界或未命中返回 nullptr / 0)。
		const uint8_t* MipData(uint32_t mip) const;
		uint64_t MipSize(uint32_t mip) const;
	};

	// 产物优先:命中 <path>.wtexc 用产物(逐 mip 块数据 + 采样状态),否则回退源图。
	// flipVertically 只作用于回退的 stb 路径 —— 产物在烘焙期已按设置定好朝向(块数据无法事后翻转)。
	WLD_API TextureAsset LoadTextureAsset(const std::string& path, bool flipVertically = false);
}
