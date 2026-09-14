#pragma once

#include "World/Core/Core.h"

namespace World
{
	class WorldContext;
}

namespace World::Asset
{
	// 按项目清单挂载内容(VFS):目录 provider(开发形态)+ 内容包(发行形态),
	// 并注册运行时服务(着色器烘焙产物解析)。
	//
	// 必须在 Renderer::Init 之前调用:渲染器初始化即创建管线、解析着色器,
	// 发行形态需要此时就能从内容包里读到 cooked 产物。
	WLD_API void MountProjectContent(World::WorldContext& context);
}
