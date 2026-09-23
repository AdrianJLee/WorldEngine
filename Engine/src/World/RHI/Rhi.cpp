#include "wldpch.h"
#include "World/RHI/Rhi.h"

#include "World/RHI/OpenGL/OpenGLDevice.h"
#include "World/RHI/Vulkan/VulkanDevice.h"

#include "World/Core/Log.h"

namespace World::Rhi
{
	Backend ResolveBackend(Backend requested, std::string* chosenName)
	{
		// W5:仅 OpenGL 后端接线;Vulkan 后端完成前选择 Vulkan 自动降级并告警。
		switch (requested)
		{
			case Backend::Vulkan:
				if (chosenName) *chosenName = "vulkan";
				return Backend::Vulkan;
			case Backend::Auto:
			case Backend::OpenGL:
			default:
				if (chosenName) *chosenName = "opengl";
				return Backend::OpenGL;
		}
	}

	Handle<Device> CreateDevice(Backend backend, const DeviceDesc& desc, std::string* error)
	{
		if (error)
			error->clear();
		if (backend == Backend::Vulkan)
			return Vulkan::VulkanDevice::Create(desc, error);
		const Backend chosen = ResolveBackend(backend, nullptr);
		if (chosen == Backend::OpenGL)
		{
			try
			{
				return CreateRef<OpenGL::OpenGLDevice>(desc);
			}
			catch (const std::exception& exception)
			{
				if (error) *error = exception.what();
				return nullptr;
			}
		}

		if (error) *error = "No RHI backend available for the requested API";
		return nullptr;
	}
}
