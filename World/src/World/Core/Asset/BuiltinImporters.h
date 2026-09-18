#pragma once

#include "World/Core/Asset/AssetImporter.h"

#include <memory>
#include <vector>

namespace World::Asset
{
	// 内置导入器:PassThrough(纹理/字体/图标原样)、Scene(.wd 原样,P2 规范化)、
	// Script(.lua/.luau → W7-2 起产出 WSL1 字节码容器,v2)。默认注册顺序:特定类型在前,
	// PassThrough 兜底。
	WLD_API std::vector<std::shared_ptr<IAssetImporter>> DefaultImporters();
}
