#include "wldpch.h"
#include "OpenGLPipeline.h"
#include "OpenGLDevice.h"
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

		// GL_SPIRV 摄入(GL_ARB_gl_spirv / GL 4.6 core):把 Slang 产出的 SPIR-V 模块
		// 交给 glShaderBinary,再用 glSpecializeShader 选定入口。
		//  - 入口名固定 "main":Slang 不带 -fvk-use-entrypoint-name 时写入的就是 "main",
		//    GL 的链接入口也是 "main"(ARB_gl_spirv 的 SpecializeShader 语义);
		//  - 失败(格式/校验不通过)返回 0,调用方按过渡期规则退回 GLSL 文本并留 ERROR 日志。
		GLuint CompileSpirVModule(GLenum type, const std::vector<uint8_t>& spirv,
			const std::string& debugName, GLuint program)
		{
			if (spirv.empty() || (spirv.size() % 4) != 0)
			{
				WLD_CORE_ERROR("[gl-spirv] {0}: SPIR-V blob is empty or not 32-bit aligned ({1} bytes)",
					debugName, spirv.size());
				return 0;
			}

			const GLuint shader = glCreateShader(type);
			if (!shader)
				return 0;

			while (glGetError() != GL_NO_ERROR) {}
			glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.data(),
				static_cast<GLsizei>(spirv.size()));
			const GLenum binaryError = glGetError();
			if (binaryError != GL_NO_ERROR)
			{
				WLD_CORE_ERROR("[gl-spirv] {0}: glShaderBinary failed (GL error 0x{1:x}, {2} bytes)",
					debugName, static_cast<unsigned>(binaryError), spirv.size());
				glDeleteShader(shader);
				return 0;
			}

			static constexpr const char* kEntryPoint = "main";
			glSpecializeShader(shader, kEntryPoint, 0, nullptr, nullptr);
			GLint specialized = GL_FALSE;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &specialized);
			if (specialized == GL_FALSE)
			{
				GLint length = 0;
				glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
				std::string log(std::max(length, 1) - 1, '\0');
				glGetShaderInfoLog(shader, length, nullptr, log.data());
				glDeleteShader(shader);
				WLD_CORE_ERROR("[gl-spirv] {0}: glSpecializeShader(\"{1}\") failed: {2}",
					debugName, kEntryPoint, log);
				return 0;
			}

			WLD_CORE_INFO("[gl-spirv] {0}: stage ingested via glShaderBinary + glSpecializeShader "
				"(entry \"{1}\", {2} bytes, program={3})",
				debugName, kEntryPoint, spirv.size(), program);
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
		const std::string debugName = desc.Shader ? desc.Shader->GetDesc().DebugName : std::string();
		for (const auto& stageSource : desc.Shader->GetDesc().Stages)
		{
			const GLenum type = ToGLShaderStage(stageSource.Stage);
			if (!type)
				continue;

			// SPIR-V 优先(能力具备且阶段确实带字节码时);否则走过渡期的 GLSL 文本分支。
			// GL 4.6 + GL_ARB_gl_spirv 是 T1 起 GL 的着色器摄入标准(T6 删除 GLSL 分支)。
			GLuint stage = 0;
			if (SupportsSpirVShaderModules() && !stageSource.SpirV.empty())
				stage = CompileSpirVModule(type, stageSource.SpirV,
					debugName + " (" + stageSource.EntryPoint + ")", m_Program);
			if (!stage && !stageSource.Glsl.empty())
				stage = CompileStage(type, stageSource.Glsl);
			if (stage)
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
