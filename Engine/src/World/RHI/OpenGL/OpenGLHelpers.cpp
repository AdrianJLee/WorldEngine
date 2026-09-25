#include "wldpch.h"
#include "OpenGLHelpers.h"

// S3TC 为扩展枚举;glad 未声明,值来自 EXT_texture_compression_s3tc。
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM 0x8E8D
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace World::Rhi::OpenGL
{
	GLenum ToGLInternalFormat(Format format)
	{
		switch (format)
		{
			case Format::R8_UNORM:              return GL_R8;
			case Format::R8G8_UNORM:            return GL_RG8;
			case Format::R8G8B8A8_UNORM:        return GL_RGBA8;
			case Format::B8G8R8A8_UNORM:        return GL_RGBA8;
			case Format::R8G8B8A8_SRGB:         return GL_SRGB8_ALPHA8;
			case Format::B8G8R8A8_SRGB:         return GL_SRGB8_ALPHA8;
			case Format::R8_SNORM:              return GL_R8_SNORM;
			case Format::R8G8_SNORM:            return GL_RG8_SNORM;
			case Format::R8G8B8A8_SNORM:        return GL_RGBA8_SNORM;
			case Format::R16_SNORM:             return GL_R16_SNORM;
			case Format::R16G16_SNORM:          return GL_RG16_SNORM;
			case Format::R16G16B16A16_SNORM:    return GL_RGBA16_SNORM;
			case Format::R8_UINT:               return GL_R8UI;
			case Format::R16_UINT:              return GL_R16UI;
			case Format::R32_UINT:              return GL_R32UI;
			case Format::R8G8_UINT:             return GL_RG8UI;
			case Format::R16G16_UINT:           return GL_RG16UI;
			case Format::R32G32_UINT:           return GL_RG32UI;
			case Format::R8G8B8A8_UINT:         return GL_RGBA8UI;
			case Format::R16G16B16A16_UINT:     return GL_RGBA16UI;
			case Format::R32G32B32A32_UINT:     return GL_RGBA32UI;
			case Format::R8_SINT:               return GL_R8I;
			case Format::R16_SINT:              return GL_R16I;
			case Format::R32_SINT:              return GL_R32I;
			case Format::R8G8_SINT:             return GL_RG8I;
			case Format::R16G16_SINT:           return GL_RG16I;
			case Format::R32G32_SINT:           return GL_RG32I;
			case Format::R8G8B8A8_SINT:         return GL_RGBA8I;
			case Format::R16G16B16A16_SINT:     return GL_RGBA16I;
			case Format::R32G32B32A32_SINT:     return GL_RGBA32I;
			case Format::R16_UNORM:             return GL_R16;
			case Format::R16G16_UNORM:          return GL_RG16;
			case Format::R16G16B16A16_UNORM:    return GL_RGBA16;
			case Format::R16_SFLOAT:            return GL_R16F;
			case Format::R16G16_SFLOAT:         return GL_RG16F;
			case Format::R16G16B16A16_SFLOAT:   return GL_RGBA16F;
			case Format::R32_SFLOAT:            return GL_R32F;
			case Format::R32G32_SFLOAT:         return GL_RG32F;
			case Format::R32G32B32_SFLOAT:      return GL_RGB32F;
			case Format::R32G32B32A32_SFLOAT:   return GL_RGBA32F;
			case Format::R10G10B10A2_UNORM:     return GL_RGB10_A2;
			case Format::R11G11B10_SFLOAT:      return GL_R11F_G11F_B10F;
			case Format::D16_UNORM:             return GL_DEPTH_COMPONENT16;
			case Format::D32_SFLOAT:            return GL_DEPTH_COMPONENT32F;
			case Format::D24_UNORM_S8_UINT:     return GL_DEPTH24_STENCIL8;
			case Format::D32_SFLOAT_S8_UINT:    return GL_DEPTH32F_STENCIL8;
			case Format::BC1_UNORM:             return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
			case Format::BC2_UNORM:             return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
			case Format::BC3_UNORM:             return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			case Format::BC4_UNORM:             return GL_COMPRESSED_RED_RGTC1;
			case Format::BC5_UNORM:             return GL_COMPRESSED_RG_RGTC2;
			case Format::BC7_UNORM:             return GL_COMPRESSED_RGBA_BPTC_UNORM;
			// M4-TEX:块格式 sRGB 变体(采样时硬件解码到线性)。
			case Format::BC1_UNORM_SRGB:        return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
			case Format::BC3_UNORM_SRGB:        return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
			case Format::BC7_UNORM_SRGB:        return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
			default:                            return 0;
		}
	}

	GLenum ToGLDataFormat(Format format)
	{
		switch (format)
		{
			case Format::R8_UNORM:
			case Format::R8_SNORM:
			case Format::R16_UNORM:
			case Format::R16_SNORM:
			case Format::R16_SFLOAT:
			case Format::R32_SFLOAT:
			case Format::D16_UNORM:
			case Format::D32_SFLOAT:
				return GL_RED;
			// 整数纹理必须配 *_INTEGER 格式:glTextureSubImage*/glGetTextureImage 用
			// GL_RED + GL_INT 这类组合是非法枚举(读回会直接失败)。
			// entity-id 附件(R32_SINT)的拾取读回就依赖这一条。
			case Format::R8_UINT:
			case Format::R8_SINT:
			case Format::R16_UINT:
			case Format::R16_SINT:
			case Format::R32_UINT:
			case Format::R32_SINT:
				return GL_RED_INTEGER;
			case Format::R8G8_UNORM:
			case Format::R8G8_SNORM:
			case Format::R16G16_UNORM:
			case Format::R16G16_SNORM:
			case Format::R16G16_SFLOAT:
			case Format::R32G32_SFLOAT:
				return GL_RG;
			case Format::R8G8_UINT:
			case Format::R8G8_SINT:
			case Format::R16G16_UINT:
			case Format::R16G16_SINT:
			case Format::R32G32_UINT:
			case Format::R32G32_SINT:
				return GL_RG_INTEGER;
			case Format::R32G32B32_SFLOAT:
			case Format::R11G11B10_SFLOAT:
				return GL_RGB;
			case Format::B8G8R8A8_UNORM:
			case Format::B8G8R8A8_SRGB:
				return GL_BGRA;
			case Format::R8G8B8A8_UNORM:
			case Format::R8G8B8A8_SRGB:
			case Format::R8G8B8A8_SNORM:
			case Format::R16G16B16A16_UNORM:
			case Format::R16G16B16A16_SNORM:
			case Format::R16G16B16A16_SFLOAT:
			case Format::R32G32B32A32_SFLOAT:
			case Format::R10G10B10A2_UNORM:
				return GL_RGBA;
			case Format::R8G8B8A8_UINT:
			case Format::R8G8B8A8_SINT:
			case Format::R16G16B16A16_UINT:
			case Format::R16G16B16A16_SINT:
			case Format::R32G32B32A32_UINT:
			case Format::R32G32B32A32_SINT:
				return GL_RGBA_INTEGER;
			case Format::D24_UNORM_S8_UINT:
			case Format::D32_SFLOAT_S8_UINT:
				return GL_DEPTH_STENCIL;
			default:
				return 0;
		}
	}

	GLenum ToGLDataType(Format format)
	{
		switch (format)
		{
			case Format::R8_UNORM:
			case Format::R8G8_UNORM:
			case Format::R8G8B8A8_UNORM:
			case Format::B8G8R8A8_UNORM:
			case Format::R8G8B8A8_SRGB:
			case Format::B8G8R8A8_SRGB:
			case Format::R8_SNORM:
			case Format::R8G8_SNORM:
			case Format::R8G8B8A8_SNORM:
			case Format::R8_UINT:
			case Format::R8G8_UINT:
			case Format::R8G8B8A8_UINT:
			case Format::R8_SINT:
			case Format::R8G8_SINT:
			case Format::R8G8B8A8_SINT:
				return GL_UNSIGNED_BYTE;
			case Format::R16_UNORM:
			case Format::R16G16_UNORM:
			case Format::R16G16B16A16_UNORM:
			case Format::R16_SNORM:
			case Format::R16G16_SNORM:
			case Format::R16G16B16A16_SNORM:
			case Format::R16_UINT:
			case Format::R16G16_UINT:
			case Format::R16G16B16A16_UINT:
			case Format::R16_SINT:
			case Format::R16G16_SINT:
			case Format::R16G16B16A16_SINT:
				return GL_UNSIGNED_SHORT;
			case Format::R16_SFLOAT:
			case Format::R16G16_SFLOAT:
			case Format::R16G16B16A16_SFLOAT:
			case Format::R32_SFLOAT:
			case Format::R32G32_SFLOAT:
			case Format::R32G32B32_SFLOAT:
			case Format::R32G32B32A32_SFLOAT:
				return GL_FLOAT;
			case Format::R32_UINT:
			case Format::R32G32_UINT:
			case Format::R32G32B32A32_UINT:
			case Format::D24_UNORM_S8_UINT:
			case Format::D32_SFLOAT_S8_UINT:
			case Format::R10G10B10A2_UNORM:
				return GL_UNSIGNED_INT;
			case Format::R32_SINT:
			case Format::R32G32_SINT:
			case Format::R32G32B32A32_SINT:
				return GL_INT;
			case Format::R11G11B10_SFLOAT:
				return GL_UNSIGNED_INT_10F_11F_11F_REV;
			case Format::D16_UNORM:
				return GL_UNSIGNED_SHORT;
			case Format::D32_SFLOAT:
				return GL_FLOAT;
			default:
				return 0;
		}
	}

	GLenum ToGLShaderStage(ShaderStage stage)
	{
		switch (stage)
		{
			case ShaderStage::Vertex:     return GL_VERTEX_SHADER;
			case ShaderStage::Fragment:   return GL_FRAGMENT_SHADER;
			case ShaderStage::Geometry:   return GL_GEOMETRY_SHADER;
			case ShaderStage::Compute:    return GL_COMPUTE_SHADER;
			default:                      return 0;
		}
	}

	GLenum ToGLCompare(CompareOp op)
	{
		switch (op)
		{
			case CompareOp::Never:          return GL_NEVER;
			case CompareOp::Less:           return GL_LESS;
			case CompareOp::Equal:          return GL_EQUAL;
			case CompareOp::LessOrEqual:    return GL_LEQUAL;
			case CompareOp::Greater:        return GL_GREATER;
			case CompareOp::NotEqual:       return GL_NOTEQUAL;
			case CompareOp::GreaterOrEqual: return GL_GEQUAL;
			case CompareOp::Always:         return GL_ALWAYS;
			default:                        return 0;
		}
	}

	GLenum ToGLStencilOp(StencilOp op)
	{
		switch (op)
		{
			case StencilOp::Keep:           return GL_KEEP;
			case StencilOp::Zero:           return GL_ZERO;
			case StencilOp::Replace:        return GL_REPLACE;
			case StencilOp::IncrementClamp: return GL_INCR;
			case StencilOp::DecrementClamp: return GL_DECR;
			case StencilOp::Invert:         return GL_INVERT;
			case StencilOp::IncrementWrap:  return GL_INCR_WRAP;
			case StencilOp::DecrementWrap:  return GL_DECR_WRAP;
			default:                        return 0;
		}
	}

	GLenum ToGLBlendFactor(BlendFactor factor)
	{
		switch (factor)
		{
			case BlendFactor::Zero:                  return GL_ZERO;
			case BlendFactor::One:                   return GL_ONE;
			case BlendFactor::SrcColor:              return GL_SRC_COLOR;
			case BlendFactor::OneMinusSrcColor:      return GL_ONE_MINUS_SRC_COLOR;
			case BlendFactor::SrcAlpha:              return GL_SRC_ALPHA;
			case BlendFactor::OneMinusSrcAlpha:      return GL_ONE_MINUS_SRC_ALPHA;
			case BlendFactor::DstAlpha:              return GL_DST_ALPHA;
			case BlendFactor::OneMinusDstAlpha:      return GL_ONE_MINUS_DST_ALPHA;
			case BlendFactor::ConstantAlpha:         return GL_CONSTANT_ALPHA;
			case BlendFactor::OneMinusConstantAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
			default:                                 return 0;
		}
	}

	GLenum ToGLBlendOp(BlendOp op)
	{
		switch (op)
		{
			case BlendOp::Add:             return GL_FUNC_ADD;
			case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
			case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
			case BlendOp::Min:             return GL_MIN;
			case BlendOp::Max:             return GL_MAX;
			default:                       return 0;
		}
	}

	GLenum ToGLFilter(Filter filter)
	{
		return filter == Filter::Nearest ? GL_NEAREST : GL_LINEAR;
	}

	GLenum ToGLMipFilter(Filter filter, SamplerMipmapMode mode)
	{
		const bool linear = filter == Filter::Linear;
		const bool linearMip = mode == SamplerMipmapMode::Linear;
		if (linear && linearMip) return GL_LINEAR_MIPMAP_LINEAR;
		if (linear) return GL_LINEAR_MIPMAP_NEAREST;
		if (linearMip) return GL_NEAREST_MIPMAP_LINEAR;
		return GL_NEAREST_MIPMAP_NEAREST;
	}

	GLenum ToGLWrap(SamplerAddressMode mode)
	{
		switch (mode)
		{
			case SamplerAddressMode::Repeat:         return GL_REPEAT;
			case SamplerAddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
			case SamplerAddressMode::ClampToEdge:    return GL_CLAMP_TO_EDGE;
			case SamplerAddressMode::ClampToBorder:  return GL_CLAMP_TO_BORDER;
			default:                                 return 0;
		}
	}

	GLenum ToGLTopology(PrimitiveTopology topology)
	{
		switch (topology)
		{
			case PrimitiveTopology::TriangleList: return GL_TRIANGLES;
			case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
			case PrimitiveTopology::LineList:     return GL_LINES;
			case PrimitiveTopology::LineStrip:    return GL_LINE_STRIP;
			case PrimitiveTopology::PointList:    return GL_POINTS;
			default:                              return 0;
		}
	}

	GLenum ToGLPolygon(PolygonMode mode)
	{
		switch (mode)
		{
			case PolygonMode::Fill:  return GL_FILL;
			case PolygonMode::Line:  return GL_LINE;
			case PolygonMode::Point: return GL_POINT;
			default:                 return 0;
		}
	}

	GLenum ToGLCull(CullMode mode)
	{
		switch (mode)
		{
			case CullMode::None:  return GL_NONE;
			case CullMode::Front: return GL_FRONT;
			case CullMode::Back:  return GL_BACK;
			default:              return 0;
		}
	}

	GLenum ToGLFrontFace(FrontFace face)
	{
		return face == FrontFace::CounterClockwise ? GL_CCW : GL_CW;
	}

	GLenum ToGLTextureTarget(TextureType type)
	{
		switch (type)
		{
			case TextureType::Texture1D: return GL_TEXTURE_1D;
			case TextureType::Texture2D: return GL_TEXTURE_2D;
			case TextureType::Texture3D: return GL_TEXTURE_3D;
			case TextureType::Cube:      return GL_TEXTURE_CUBE_MAP;
			default:                     return 0;
		}
	}
}
