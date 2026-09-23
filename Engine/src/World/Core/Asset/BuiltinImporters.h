#pragma once

#include "World/Core/Asset/AssetImporter.h"

#include <memory>
#include <vector>

namespace World::Asset
{
	// 内置导入器:PassThrough(纹理/字体/图标原样)、Scene(.wd 原样,P2 规范化)、
	// Script(.lua/.luau → W7-2 起产出 WSL1 字节码容器,v2)、
	// Model(D5b:.gltf/.glb → .wmodel v2 + .wmat + 贴图的多产物导入,v1)、
	// MaterialShader(M4-S2 起的材质表面函数原样复制;Slang-B1 起 **v2**:
	// 规范扩展名 = .slang,legacy .hlsl 兼容读取 + 迁移提示;cook 指纹 = 源内容哈希
	// 复合导入器名/版本 —— 改了注解/代码或升版就重烘)。
	// 默认注册顺序:Scene/Script/Model/MaterialShader 在前,PassThrough 兜底。
	WLD_API std::vector<std::shared_ptr<IAssetImporter>> DefaultImporters();
}
