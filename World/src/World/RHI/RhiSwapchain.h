#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	class Semaphore;

	enum class PresentMode : uint8_t { Immediate = 0, Mailbox, Fifo };
	enum class ColorSpace : uint8_t { SrgbNonlinear = 0, ExtendedSrgbLinear };

	struct SwapchainDesc
	{
		void* NativeWindow = nullptr;      // 平台原生窗口句柄
		Format Format = Format::B8G8R8A8_UNORM;
		uint32_t ImageCount = 3;           // 0 = 后端默认
		bool VSync = true;
		PresentMode Present = PresentMode::Fifo;
		ColorSpace Space = ColorSpace::SrgbNonlinear;
		std::string DebugName;
	};

	struct AcquireResult
	{
		uint32_t ImageIndex = 0;
		Handle<Texture> Image;             // 当前可渲染图像(Present 布局前需 barrier)
		bool OutOfDate = false;            // 尺寸/表面变化,需重建交换链
	};

	class WLD_API Swapchain
	{
	public:
		virtual ~Swapchain() = default;
		virtual AcquireResult AcquireNext(const Handle<Semaphore>& signalWhenReady = nullptr) = 0;
		virtual void Present(const Handle<Semaphore>& waitBeforePresent = nullptr) = 0;
		virtual Extent2D GetExtent() const = 0;
		// 交换链图像数量:呈现信号量需按图像配对(不按帧槽位)。
		virtual uint32_t GetImageCount() const = 0;
		virtual void Resize(Extent2D extent) = 0;
	};
}
