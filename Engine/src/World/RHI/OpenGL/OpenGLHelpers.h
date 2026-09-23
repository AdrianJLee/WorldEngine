#pragma once

#include "World/RHI/RhiCore.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	// 合同 → GL 常量映射。返回 0 表示该格式/枚举 GL 后端不支持。
	GLenum ToGLInternalFormat(Format format);
	GLenum ToGLDataFormat(Format format);
	GLenum ToGLDataType(Format format);
	GLenum ToGLShaderStage(ShaderStage stage);
	GLenum ToGLCompare(CompareOp op);
	GLenum ToGLStencilOp(StencilOp op);
	GLenum ToGLBlendFactor(BlendFactor factor);
	GLenum ToGLBlendOp(BlendOp op);
	GLenum ToGLFilter(Filter filter);
	GLenum ToGLMipFilter(Filter filter, SamplerMipmapMode mode);
	GLenum ToGLWrap(SamplerAddressMode mode);
	GLenum ToGLTopology(PrimitiveTopology topology);
	GLenum ToGLPolygon(PolygonMode mode);
	GLenum ToGLCull(CullMode mode);
	GLenum ToGLFrontFace(FrontFace face);
	GLenum ToGLTextureTarget(TextureType type);
}
