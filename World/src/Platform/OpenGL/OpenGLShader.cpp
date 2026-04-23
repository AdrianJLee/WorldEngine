#include "wldpch.h"
#include "OpenGLShader.h"

#include "World/Renderer/ShaderUtils.h"

#include <glad/glad.h>

namespace World
{

	// Get the shader type from the file path
	static GLenum ShaderTypeFromString(const std::string& type)
	{
		if (type == ".vert")
			return GL_VERTEX_SHADER;
		if (type == ".frag")
			return GL_FRAGMENT_SHADER;
		WLD_CORE_ASSERT(false, "Unknown shader type!");
		return 0;
	}

	OpenGLShader::~OpenGLShader()
	{
		WLD_PROFILE_FUNCTION();

		glDeleteProgram(m_RendererID);
	}
	void OpenGLShader::Bind() const
	{
		WLD_PROFILE_FUNCTION();

		glUseProgram(m_RendererID);
	}
	void OpenGLShader::Unbind() const
	{
		glUseProgram(0);
	}

	void OpenGLShader::AddShader(const std::string& path, ShaderType type)
	{
		m_Name = path;
		switch (type)
		{
			case ShaderType::Vertex:
			{
				const auto& vertexBuffer = ShaderCompiler::CompileOrLoad(path, "VSMain", "vs_6_0");
				shaderSources[GL_VERTEX_SHADER] = std::string(vertexBuffer.begin(), vertexBuffer.end());

				break;
			}

			case ShaderType::Fragment:
			{
				const auto& fragBuffer = ShaderCompiler::CompileOrLoad(path, "PSMain", "ps_6_0");
				shaderSources[GL_FRAGMENT_SHADER] = std::string(fragBuffer.begin(), fragBuffer.end());

				break;
			}

		}
		WLD_CORE_ASSERT(false, "We only support vertex and fragment shaders for now!");

	}

	void OpenGLShader::Compile()
	{
		WLD_PROFILE_FUNCTION();
		Compile(shaderSources);
	}

	void OpenGLShader::Compile(const std::unordered_map<GLenum, std::string>& shaderSources)
	{
		WLD_PROFILE_FUNCTION();

		uint32_t rendererID = glCreateProgram();

		WLD_ASSERT(shaderSources.size() <= 2, "We only support 2 shaders for now (vertex and fragment)");
		// Compile each shader
		std::array<GLuint, 2> shaderIDs;
		int index = 0;
		for (auto& kv : shaderSources)
		{
			GLenum type = kv.first;
			const std::string& source = kv.second;
			GLuint Shader = glCreateShader(type);

			// Send the shader source code to GL
			const GLchar* sourceCStr = source.c_str();
			glShaderSource(Shader, 1, &sourceCStr, 0);
			glCompileShader(Shader);

			GLint isCompiled = 0;
			glGetShaderiv(Shader, GL_COMPILE_STATUS, &isCompiled);
			if (isCompiled == GL_FALSE)
			{
				GLint maxLength = 0;
				glGetShaderiv(Shader, GL_INFO_LOG_LENGTH, &maxLength);

				std::vector<GLchar> infoLog(maxLength);
				glGetShaderInfoLog(Shader, maxLength, &maxLength, &infoLog[0]);

				glDeleteShader(Shader);

				WLD_CORE_ERROR("{0}", infoLog.data());
				WLD_CORE_ASSERT(false, "Shader compilation failure!");

				return;
			}

			glAttachShader(rendererID, Shader);
			shaderIDs[index++] = Shader;
		}

		// Link our program
		glLinkProgram(rendererID);

		// Note the different functions here: glGetProgram* instead of glGetShader*.
		GLint isLinked = 0;
		glGetProgramiv(rendererID, GL_LINK_STATUS, (int*)&isLinked);
		if (isLinked == GL_FALSE)
		{
			GLint maxLength = 0;
			glGetProgramiv(rendererID, GL_INFO_LOG_LENGTH, &maxLength);

			// The maxLength includes the NULL character
			std::vector<GLchar> infoLog(maxLength);
			glGetProgramInfoLog(rendererID, maxLength, &maxLength, &infoLog[0]);

			// We don't need the program anymore.
			glDeleteProgram(rendererID);

			// Don't leak shaders either.
			for (auto id : shaderIDs)
				glDeleteShader(id);


			WLD_CORE_ERROR("{0}", infoLog.data());
			WLD_CORE_ASSERT(false, "Shader link failure!");
			return;
		}

		// Always detach shaders after a successful link.
		for (auto id : shaderIDs)
			glDetachShader(rendererID, id);

		m_RendererID = rendererID;


		// 解决名字不规范问题：提取并自动给 sampler2D 类型的 uniform 分配对应的纹理槽号（0-31）
		glUseProgram(m_RendererID);
		GLint numUniforms = 0;
		glGetProgramiv(m_RendererID, GL_ACTIVE_UNIFORMS, &numUniforms);

		for (int i = 0; i < numUniforms; ++i)
		{
			GLenum type;
			GLint size;
			char name[128];
			glGetActiveUniform(m_RendererID, i, sizeof(name), nullptr, &size, &type, name);

			// 找到所有的2D采样器，给它们指派纹理槽 (0,1,2...,size-1)
			if (type == GL_SAMPLER_2D)
			{
				GLint location = glGetUniformLocation(m_RendererID, name);

				// 因为着色器中限定且通常最多 32 个纹理槽位
				int samplers[32];
				for (int j = 0; j < 32; j++)
				{
					samplers[j] = j;
				}

				// 给这个不规范名字的 uniform 数组，按槽位依次赋值 [0, 1, 2, ... ]
				glUniform1iv(location, size, samplers);
			}
		}
	}

}