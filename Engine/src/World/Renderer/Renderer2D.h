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
		static void Shutdown();
		static void BeginScene(const Camera& camera, const glm::mat4& transform, Rhi::Handle<Rhi::CommandBuffer> commandBuffer);

		static void EndScene();
		static void StartBatch();
		static void Flush();

		static void DrawQuadCore(const glm::mat4& transform, const Ref<Texture2D>& texture,
			const glm::vec4& color = glm::vec4(1.0f), const glm::vec2* texCoords = nullptr, float tilingFactor = 1.0f, int entityID = -1);
		static void DrawCircleCore(const glm::mat4& transform, const glm::vec4& color, float thickness = 1.0f, float fade = 0.005f, int entityID = -1);

		// ---- B3 并行几何预处理 ----
		// 把"每实体独立的顶点变换"从批次状态机里拆出来:这两个 Compute 只读共享常量、
		// 只写调用方提供的数组,可在工作线程安全执行(SceneRenderer 用 JobSystem 并行调用);
		// 随后的 Draw*Positions 仍在主线程按原顺序写入批次缓冲,保证绘制顺序与结果不变。
		static void ComputeQuadPositions(const glm::mat4& transform, glm::vec3 outPositions[4]);
		static void DrawQuadPositions(const glm::vec3 positions[4], const Ref<Texture2D>& texture,
			const glm::vec4& color, const glm::vec2* texCoords, float tilingFactor, int entityID);
		static void ComputeCirclePositions(const glm::mat4& transform, glm::vec3 outPositions[4]);
		static void DrawCirclePositions(const glm::vec3 positions[4], const glm::vec4& color,
			float thickness, float fade, int entityID);

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
			uint32_t ParallelPreparedQuads = 0;    // 本帧由工作线程完成变换的四边形数
			uint32_t ParallelPreparedCircles = 0;  // 本帧由工作线程完成变换的圆形数
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
