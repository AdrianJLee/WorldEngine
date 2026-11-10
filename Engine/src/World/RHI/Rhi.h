#pragma once

#include "World/RHI/RhiDevice.h"

namespace World::Rhi
{
	enum class Backend : uint8_t { OpenGL = 0, Vulkan, Auto };

	// 工厂:根据请求创建后端设备;Auto 按能力与可用性降级(请求 Vulkan 不可用 →
	// 记录日志并回退 OpenGL)。返回 nullptr 表示没有任何可用后端。
	WLD_API Handle<Device> CreateDevice(Backend backend, const DeviceDesc& desc, std::string* error);
	WLD_API Backend ResolveBackend(Backend requested, std::string* chosenName);
}
