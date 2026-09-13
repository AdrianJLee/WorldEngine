#pragma once
#include "World/Core/Export.h"
#include "World/RHI/Rhi.h"
#include "World/Renderer/SubTexture2D.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Components.h"

#include <vector>

namespace World
{
	class OrthographicCamera;
	class Shader;
	class VertexArray;
	class Texture2D;
	class Renderer2D
	{
	public:
		static void Init();
		static void BeginScene(const Camera& camera, const glm::mat4& transform, Rhi::Handle<Rhi::CommandBuffer> commandBuffer);

		static void EndScene();
		static void StartBatch();
		static void Flush();

		static void DrawQuadCore(const glm::mat4& transform, const Ref<Texture2D>& texture,
			const glm::vec4& color = glm::vec4(1.0f), const glm::vec2* texCoords = nullptr, float tilingFactor = 1.0f, int entityID = -1);
		static void DrawCircleCore(const glm::mat4& transform, const glm::vec4& color, float thickness = 1.0f, float fade = 0.005f, int entityID = -1);
		static void DrawLineCore(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID = -1);
		static void DrawRectCore(const glm::mat4& transform, const glm::vec4& color, int entityID = -1);


		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color);
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color);
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor = 1.0f);
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor = 1.0f);
		static void DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f);
		static void DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f);
		static void DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));


		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color);
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color);
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));
		static void DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor = 1.0f, const glm::vec4& tintColor = glm::vec4(1.0f));

		static void DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, float rotation = 0);

		// 统计信息
		struct Statistics
		{
			uint32_t DrawCalls = 0;
			uint32_t QuadCount = 0;
			uint32_t CircleCount = 0;
			uint32_t LineCount = 0;
			uint32_t GetTotalVertexCount() const { return QuadCount * 4 + CircleCount * 4 + LineCount * 2; }
			uint32_t GetTotalIndexCount() const { return QuadCount * 6 + CircleCount * 6 + LineCount * 2; }
		};
		static Statistics GetStats();
		static void ResetStats();

	private:

		static void NextBatch();

	private:
		static WLD_API Rhi::Handle<Rhi::CommandBuffer> s_CurrentCommandBuffer;
	};
}
