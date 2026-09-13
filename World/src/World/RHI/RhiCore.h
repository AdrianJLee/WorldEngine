#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// RHI 合同核心类型。POD 描述符默认值即"未设置"状态;接口只面向合同,后端在
// World/src/World/RHI/<Backend> 实现。跨 DLL 的版本号见 RHI_ABI_VERSION。

#define WORLD_RHI_ABI_VERSION 1u

namespace World::Rhi
{
	template <typename T>
	using Handle = Ref<T>;

	enum class Format : uint16_t
	{
		Undefined = 0,
		R8_UNORM,
		R8G8_UNORM,
		R8G8B8A8_UNORM,
		B8G8R8A8_UNORM,
		R8G8B8A8_SRGB,
		B8G8R8A8_SRGB,
		R16_UNORM,
		R16G16_UNORM,
		R16G16B16A16_UNORM,
		R16_SFLOAT,
		R16G16_SFLOAT,
		R16G16B16A16_SFLOAT,
		R32_SFLOAT,
		R32G32_SFLOAT,
		R32G32B32_SFLOAT,
		R32G32B32A32_SFLOAT,
		R10G10B10A2_UNORM,
		R11G11B10_SFLOAT,
		D16_UNORM,
		D32_SFLOAT,
		D24_UNORM_S8_UINT,
		D32_SFLOAT_S8_UINT,
		Count,
	};

	enum class TextureType : uint8_t { Texture1D = 0, Texture2D, Texture3D, Cube };
	enum class SampleCount : uint8_t { Count1 = 1, Count2 = 2, Count4 = 4, Count8 = 8 };

	enum BufferUsageBits : uint32_t
	{
		BufferUsageNone = 0,
		BufferUsageVertex = 1u << 0,
		BufferUsageIndex = 1u << 1,
		BufferUsageUniform = 1u << 2,
		BufferUsageStorage = 1u << 3,
		BufferUsageIndirect = 1u << 4,
		BufferUsageTransferSrc = 1u << 5,
		BufferUsageTransferDst = 1u << 6,
	};

	enum TextureUsageBits : uint32_t
	{
		TextureUsageNone = 0,
		TextureUsageSampled = 1u << 0,
		TextureUsageColorAttachment = 1u << 1,
		TextureUsageDepthStencilAttachment = 1u << 2,
		TextureUsageStorage = 1u << 3,
		TextureUsageTransferSrc = 1u << 4,
		TextureUsageTransferDst = 1u << 5,
	};

	enum class MemoryHint : uint8_t { DeviceLocal = 0, HostVisible, HostCoherent };

	enum class ShaderStage : uint8_t { Vertex = 0, Fragment, Geometry, Compute, Count };
	using ShaderStageFlags = uint32_t;
	inline constexpr ShaderStageFlags ShaderStageFlag(ShaderStage stage)
	{
		return 1u << static_cast<uint32_t>(stage);
	}

	enum class PrimitiveTopology : uint8_t
	{
		TriangleList = 0, TriangleStrip, LineList, LineStrip, PointList,
	};
	enum class PolygonMode : uint8_t { Fill = 0, Line, Point };
	enum class CullMode : uint8_t { None = 0, Front, Back };
	enum class FrontFace : uint8_t { CounterClockwise = 0, Clockwise };
	enum class IndexType : uint8_t { UInt16 = 0, UInt32 };
	enum class CompareOp : uint8_t { Never = 0, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
	enum class Filter : uint8_t { Nearest = 0, Linear };
	enum class SamplerMipmapMode : uint8_t { Nearest = 0, Linear };
	enum class SamplerAddressMode : uint8_t { Repeat = 0, MirroredRepeat, ClampToEdge, ClampToBorder };
	enum class BlendFactor : uint8_t { Zero = 0, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantAlpha, OneMinusConstantAlpha };
	enum class BlendOp : uint8_t { Add = 0, Subtract, ReverseSubtract, Min, Max };
	enum class LoadOp : uint8_t { Load = 0, Clear, DontCare };
	enum class StoreOp : uint8_t { Store = 0, DontCare };
	enum class AttachmentLayout : uint8_t { Undefined = 0, ColorAttachment, DepthStencilAttachment, Present, ShaderReadOnly, TransferDst };
	enum class DescriptorType : uint8_t { Sampler = 0, CombinedImageSampler, SampledImage, StorageImage, UniformBuffer, StorageBuffer, InputAttachment };
	enum class ResourceState : uint8_t
	{
		Undefined = 0, General, ColorAttachment, DepthStencilAttachment, Present, ShaderReadOnly,
		CopySrc, CopyDst, VertexBuffer, IndexBuffer, UniformBuffer,
	};

	struct Extent2D { uint32_t Width = 0; uint32_t Height = 0; };
	struct Extent3D { uint32_t Width = 0; uint32_t Height = 0; uint32_t Depth = 1; };
	struct Offset3D { int32_t X = 0; int32_t Y = 0; int32_t Z = 0; };
	struct Viewport { float X = 0; float Y = 0; float Width = 0; float Height = 0; float MinDepth = 0; float MaxDepth = 1; };
	struct Scissor { int32_t X = 0; int32_t Y = 0; uint32_t Width = 0; uint32_t Height = 0; };
	struct ClearColor { float R = 0; float G = 0; float B = 0; float A = 1; };
	struct ClearDepthStencil { float Depth = 1.0f; uint32_t Stencil = 0; };
	struct ClearValue
	{
		ClearColor Color;
		ClearDepthStencil DepthStencil;
	};

	// 资源屏障:GL 后端可降级为 no-op,合同语义仍要求正确排序。
	struct ResourceBarrier
	{
		Handle<class Texture> Texture;
		Handle<class Buffer> Buffer;
		ResourceState Before = ResourceState::Undefined;
		ResourceState After = ResourceState::Undefined;
		uint32_t BaseMipLevel = 0;
		uint32_t MipLevelCount = 1;
		uint32_t BaseArrayLayer = 0;
		uint32_t ArrayLayerCount = 1;
	};
}
