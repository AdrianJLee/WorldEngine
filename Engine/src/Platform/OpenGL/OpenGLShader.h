#pragma once
#include "World/Renderer/Shader.h"
#include <string>
#include <unordered_map>

typedef unsigned int GLenum;

namespace World
{
	class OpenGLShader :public Shader
	{
	public:
		OpenGLShader() = default;

		virtual ~OpenGLShader();
		virtual void Bind() const override;
		virtual void Unbind() const override;
		virtual const std::string& GetName() const override { return m_Name; }

		virtual void AddShader(const std::string& path, ShaderType type) override;
		virtual void Compile() override;

	private:

		void Compile(const std::unordered_map<GLenum, std::string>& shaderSources);
	private:
		uint32_t m_RendererID;
		std::string m_Name;
		std::unordered_map<GLenum, std::string> shaderSources;

	};

}

