#pragma once
#include "World/Renderer/Buffer.h"
#include "World/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

namespace World
{
	class Shader
	{
	public:
		enum class ShaderType
		{
			None = 0,
			Vertex = 1,
			Fragment = 2,
		};
	public:
		static Ref<Shader> Create();

	public:
		virtual ~Shader() = default;
		virtual void AddShader(const std::string& path, ShaderType type) = 0;
		virtual void Compile() = 0;
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual const std::string& GetName() const = 0;

	};

}

