#pragma once
#include "World/Core/Export.h"
#include "World/Renderer/RendererAPI.h"

namespace World
{
	class Renderer
	{
	public:
		static void Init();
		static void OnWindowResize(uint32_t width, uint32_t height);

		static void Submit(const Ref<class Shader>& shader, const Ref<class VertexArray>& vertexArray, const  glm::mat4& transform = glm::mat4(1.0));

		inline static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static WLD_API SceneData* m_SceneData;
	};
}


