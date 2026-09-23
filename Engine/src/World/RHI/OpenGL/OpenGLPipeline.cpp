#include "wldpch.h"
#include "OpenGLPipeline.h"
#include "OpenGLHelpers.h"

#include "World/RHI/RhiShader.h"

namespace World::Rhi::OpenGL
{
	namespace
	{
		GLuint CompileStage(GLenum type, const std::string& source)
		{
			GLuint shader = glCreateShader(type);
			const char* src = source.c_str();
			glShaderSource(shader, 1, &src, nullptr);
			glCompileShader(shader);

			GLint compiled = GL_FALSE;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
			if (compiled == GL_FALSE)
			{
				GLint length = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
				std::string log(std::max(length, 1) - 1, '\0');
				glGetShaderInfoLog(shader, length, nullptr, log.data());
				glDeleteShader(shader);
				WLD_CORE_ERROR("GL shader compile failed: {0}", log);
				return 0;
			}
			return shader;
		}

		GLenum AttribBaseType(Format format, bool& integer, uint32_t& componentCount)
		{
			integer = false;
			switch (format)
			{
				case Format::R32_SFLOAT:       componentCount = 1; return GL_FLOAT;
				case Format::R32G32_SFLOAT:    componentCount = 2; return GL_FLOAT;
				case Format::R32G32B32_SFLOAT: componentCount = 3; return GL_FLOAT;
				case Format::R32G32B32A32_SFLOAT: componentCount = 4; return GL_FLOAT;
				case Format::R8G8B8A8_UNORM:   componentCount = 4; return GL_UNSIGNED_BYTE;
				case Format::R8G8_UNORM:       componentCount = 2; return GL_UNSIGNED_BYTE;
				case Format::R8_UNORM:         componentCount = 1; return GL_UNSIGNED_BYTE;
				case Format::R16G16_UNORM:     componentCount = 2; return GL_UNSIGNED_SHORT;
				case Format::R16G16B16A16_UNORM: componentCount = 4; return GL_UNSIGNED_SHORT;
				case Format::R16G16B16A16_SFLOAT: componentCount = 4; return GL_HALF_FLOAT;
				case Format::R32_UINT:         componentCount = 1; integer = true; return GL_UNSIGNED_INT;
				case Format::R32G32_UINT:      componentCount = 2; integer = true; return GL_UNSIGNED_INT;
				case Format::R32G32B32A32_UINT: componentCount = 4; integer = true; return GL_UNSIGNED_INT;
				case Format::R8G8B8A8_UINT:    componentCount = 4; integer = true; return GL_UNSIGNED_BYTE;
				case Format::R8G8_UINT:        componentCount = 2; integer = true; return GL_UNSIGNED_BYTE;
				case Format::R8_UINT:          componentCount = 1; integer = true; return GL_UNSIGNED_BYTE;
				case Format::R32_SINT:         componentCount = 1; integer = true; return GL_INT;
				case Format::R32G32_SINT:      componentCount = 2; integer = true; return GL_INT;
				case Format::R32G32B32A32_SINT: componentCount = 4; integer = true; return GL_INT;
				case Format::R8G8B8A8_SINT:    componentCount = 4; integer = true; return GL_BYTE;
				case Format::R8G8_SINT:        componentCount = 2; integer = true; return GL_BYTE;
				case Format::R8_SINT:          componentCount = 1; integer = true; return GL_BYTE;
				default:
					componentCount = 4;
					return GL_FLOAT;
			}
		}
	}

	OpenGLPipeline::OpenGLPipeline(const PipelineDesc& desc)
		: m_Desc(desc)
	{
		WLD_CORE_ASSERT(desc.Shader, "PipelineDesc requires a shader");

		m_Program = glCreateProgram();
		std::vector<GLuint> stages;
		for (const auto& stageSource : desc.Shader->GetDesc().Stages)
		{
			const GLenum type = ToGLShaderStage(stageSource.Stage);
			if (!type || stageSource.Glsl.empty())
				continue;
			if (GLuint stage = CompileStage(type, stageSource.Glsl))
			{
				glAttachShader(m_Program, stage);
				stages.push_back(stage);
			}
		}
		WLD_CORE_ASSERT(!stages.empty(), "Pipeline has no compilable stages");

		glLinkProgram(m_Program);
		GLint linked = GL_FALSE;
		glGetProgramiv(m_Program, GL_LINK_STATUS, &linked);
		if (linked == GL_FALSE)
		{
			GLint length = 0;
			glGetProgramiv(m_Program, GL_INFO_LOG_LENGTH, &length);
			std::string log(std::max(length, 1) - 1, '\0');
			glGetProgramInfoLog(m_Program, length, nullptr, log.data());
			WLD_CORE_ERROR("GL program link failed: {0}", log);
		}
		for (GLuint stage : stages)
			glDetachShader(m_Program, stage);
		for (GLuint stage : stages)
			glDeleteShader(stage);

		// VAO:绑定格式与属性启用状态;顶点缓冲在绘制时由命令缓冲绑定。
		glCreateVertexArrays(1, &m_VertexArray);
		for (const auto& attribute : desc.VertexAttributes)
		{
			bool integer = false;
			uint32_t components = 4;
			const GLenum baseType = AttribBaseType(attribute.Format, integer, components);
			glEnableVertexArrayAttrib(m_VertexArray, attribute.Location);
			if (integer)
				glVertexArrayAttribIFormat(m_VertexArray, attribute.Location, components, baseType, attribute.Offset);
			else
				glVertexArrayAttribFormat(m_VertexArray, attribute.Location, components, baseType, GL_FALSE, attribute.Offset);
			glVertexArrayAttribBinding(m_VertexArray, attribute.Location, attribute.Binding);
		}
		for (const auto& binding : desc.VertexBindings)
			glVertexArrayBindingDivisor(m_VertexArray, binding.Binding, binding.PerInstance ? 1 : 0);
	}

	OpenGLPipeline::~OpenGLPipeline()
	{
		if (m_Program)
			glDeleteProgram(m_Program);
		if (m_VertexArray)
			glDeleteVertexArrays(1, &m_VertexArray);
	}

	void OpenGLPipeline::Bind() const
	{
		glUseProgram(m_Program);
		glBindVertexArray(m_VertexArray);

		// 深度/模板
		if (m_Desc.DepthStencil.DepthTest) glEnable(GL_DEPTH_TEST);
		else glDisable(GL_DEPTH_TEST);
		glDepthMask(m_Desc.DepthStencil.DepthWrite ? GL_TRUE : GL_FALSE);
		glDepthFunc(ToGLCompare(m_Desc.DepthStencil.DepthCompare));
		glDepthRange(0.0f, 1.0f);

		if (m_Desc.DepthStencil.StencilTest)
		{
			glEnable(GL_STENCIL_TEST);
			const auto& front = m_Desc.DepthStencil.Front;
			const auto& back = m_Desc.DepthStencil.Back;
			glStencilFuncSeparate(GL_FRONT, ToGLCompare(front.Compare), front.Reference, front.CompareMask);
			glStencilOpSeparate(GL_FRONT, ToGLStencilOp(front.Fail), ToGLStencilOp(front.DepthFail), ToGLStencilOp(front.Pass));
			glStencilMaskSeparate(GL_FRONT, front.WriteMask);
			glStencilFuncSeparate(GL_BACK, ToGLCompare(back.Compare), back.Reference, back.CompareMask);
			glStencilOpSeparate(GL_BACK, ToGLStencilOp(back.Fail), ToGLStencilOp(back.DepthFail), ToGLStencilOp(back.Pass));
			glStencilMaskSeparate(GL_BACK, back.WriteMask);
		}
		else
			glDisable(GL_STENCIL_TEST);

		// 光栅化
		if (m_Desc.Cull == CullMode::None)
			glDisable(GL_CULL_FACE);
		else
		{
			glEnable(GL_CULL_FACE);
			glCullFace(ToGLCull(m_Desc.Cull));
		}
		glFrontFace(ToGLFrontFace(m_Desc.Front));
		glPolygonMode(GL_FRONT_AND_BACK, ToGLPolygon(m_Desc.Polygon));
		glLineWidth(m_Desc.LineWidth);
		if (m_Desc.DepthBiasConstant != 0.0f || m_Desc.DepthBiasSlope != 0.0f)
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(m_Desc.DepthBiasSlope, m_Desc.DepthBiasConstant);
		}
		else
			glDisable(GL_POLYGON_OFFSET_FILL);
		if (m_Desc.DepthClamp) glEnable(GL_DEPTH_CLAMP);
		else glDisable(GL_DEPTH_CLAMP);
		if (m_Desc.PrimitiveRestart) glEnable(GL_PRIMITIVE_RESTART);
		else glDisable(GL_PRIMITIVE_RESTART);
		if (m_Desc.RasterizerDiscard) glEnable(GL_RASTERIZER_DISCARD);
		else glDisable(GL_RASTERIZER_DISCARD);
		if (m_Desc.AlphaToCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		glSampleMaski(0, m_Desc.SampleMask);

		// 混合(逐附件)
		if (m_Desc.Blends.empty())
		{
			glDisablei(GL_BLEND, 0);
			glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		}
		for (size_t i = 0; i < m_Desc.Blends.size(); i++)
		{
			const auto& blend = m_Desc.Blends[i];
			if (blend.BlendEnable)
			{
				glEnablei(GL_BLEND, static_cast<GLuint>(i));
				glBlendEquationSeparatei(static_cast<GLuint>(i), ToGLBlendOp(blend.ColorOp), ToGLBlendOp(blend.AlphaOp));
				glBlendFuncSeparatei(static_cast<GLuint>(i),
					ToGLBlendFactor(blend.SrcColor), ToGLBlendFactor(blend.DstColor),
					ToGLBlendFactor(blend.SrcAlpha), ToGLBlendFactor(blend.DstAlpha));
			}
			else
				glDisablei(GL_BLEND, static_cast<GLuint>(i));
			glColorMaski(static_cast<GLuint>(i),
				(blend.ColorWriteMask & 0x1) ? GL_TRUE : GL_FALSE,
				(blend.ColorWriteMask & 0x2) ? GL_TRUE : GL_FALSE,
				(blend.ColorWriteMask & 0x4) ? GL_TRUE : GL_FALSE,
				(blend.ColorWriteMask & 0x8) ? GL_TRUE : GL_FALSE);
		}
	}
}
