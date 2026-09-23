#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// RHI 合同核心类型。POD 描述符默认值即"未设置"状态;接口只面向合同,后端在
// Engine/src/World/RHI/<Backend> 实现。跨 DLL 的版本号见 RHI_ABI_VERSION。

#define WORLD_RHI_ABI_VERSION 2u

// 前置声明(全限定):RHI 头会与旧渲染器的 World::Texture 同处一个翻译单元,
// 裸名声明会解析到父命名空间的同名类,导致成员类型悄悄变成旧类型。
namespace World::Rhi
{
	class Texture;
	class Buffer;
}

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
		R8_SNORM,
		R8G8_SNORM,
		R8G8B8A8_SNORM,
		R16_SNORM,
		R16G16_SNORM,
		R16G16B16A16_SNORM,
		R8_UINT,
		R16_UINT,
		R32_UINT,
		R8G8_UINT,
		R16G16_UINT,
		R32G32_UINT,
		R8G8B8A8_UINT,
		R16G16B16A16_UINT,
		R32G32B32A32_UINT,
		R8_SINT,
		R16_SINT,
		R32_SINT,
		R8G8_SINT,
		R16G16_SINT,
		R32G32_SINT,
		R8G8B8A8_SINT,
		R16G16B16A16_SINT,
		R32G32B32A32_SINT,
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
		BC1_UNORM,
		BC2_UNORM,
		BC3_UNORM,
		BC4_UNORM,
		BC5_UNORM,
		BC7_UNORM,
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
		TextureUsageInputAttachment = 1u << 6,
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
	enum class AttachmentLayout : uint8_t { Undefined = 0, ColorAttachment, DepthStencilAttachment, Present, ShaderReadOnly, TransferSrc, TransferDst };
	enum class DescriptorType : uint8_t { Sampler = 0, CombinedImageSampler, SampledImage, StorageImage, UniformBuffer, StorageBuffer, InputAttachment };
	enum class ResourceState : uint8_t
	{
		Undefined = 0, General, ColorAttachment, DepthStencilAttachment, Present, ShaderReadOnly,
		CopySrc, CopyDst, VertexBuffer, IndexBuffer, UniformBuffer,
	};

	enum class StencilOp : uint8_t { Keep = 0, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
	enum class QueryType : uint8_t { Occlusion = 0, Timestamp };
	enum class BorderColor : uint8_t { TransparentBlack = 0, OpaqueBlack, OpaqueWhite };

	enum PipelineStageBits : uint32_t
	{
		PipelineStageNone = 0,
		PipelineStageTopOfPipe = 1u << 0,
		PipelineStageDrawIndirect = 1u << 1,
		PipelineStageVertexInput = 1u << 2,
		PipelineStageVertexShader = 1u << 3,
		PipelineStageFragmentShader = 1u << 4,
		PipelineStageEarlyFragmentTests = 1u << 5,
		PipelineStageLateFragmentTests = 1u << 6,
		PipelineStageColorAttachmentOutput = 1u << 7,
		PipelineStageTransfer = 1u << 8,
		PipelineStageComputeShader = 1u << 9,
		PipelineStageBottomOfPipe = 1u << 10,
		PipelineStageAllCommands = 1u << 11,
		PipelineStageAllGraphics = 1u << 12,
	};

	enum AccessBits : uint32_t
	{
		AccessNone = 0,
		AccessIndirectRead = 1u << 0,
		AccessIndexRead = 1u << 1,
		AccessVertexRead = 1u << 2,
		AccessUniformRead = 1u << 3,
		AccessShaderRead = 1u << 4,
		AccessShaderWrite = 1u << 5,
		AccessColorAttachmentRead = 1u << 6,
		AccessColorAttachmentWrite = 1u << 7,
		AccessDepthStencilRead = 1u << 8,
		AccessDepthStencilWrite = 1u << 9,
		AccessTransferRead = 1u << 10,
		AccessTransferWrite = 1u << 11,
		AccessHostRead = 1u << 12,
		AccessHostWrite = 1u << 13,
		AccessMemoryRead = 1u << 14,
		AccessMemoryWrite = 1u << 15,
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
		bool IsDepthStencil = false;
	};

	// 资源屏障:GL 后端可降级为 no-op,合同语义仍要求正确排序。
	struct ResourceBarrier
	{
		// 必须写全限定名:RHI 头经常与旧渲染器的 World::Texture(World/Renderer/Texture.h)
		// 同处一个翻译单元,裸 "Texture" 会被外层命名空间解析成旧类型(实测报错)。
		Handle<Rhi::Texture> Texture;
		Handle<Rhi::Buffer> Buffer;
		ResourceState Before = ResourceState::Undefined;
		ResourceState After = ResourceState::Undefined;
		uint32_t BaseMipLevel = 0;
		uint32_t MipLevelCount = 1;
		uint32_t BaseArrayLayer = 0;
		uint32_t ArrayLayerCount = 1;
	};
}
