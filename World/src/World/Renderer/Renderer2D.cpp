#include "wldpch.h"
#include "Renderer2D.h"

#include "World/Renderer/VertexArray.h"
#include "World/Renderer/Buffer.h"
#include "World/Renderer/Shader.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/Texture.h"
#include "World/Renderer/PipelineStateObject.h"

#include <glm/gtc/matrix_transform.hpp>

namespace World
{
	Ref<CommandBuffer> Renderer2D::s_CurrentCommandBuffer = nullptr;
	struct QuadVertex
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };

		// 默认颜色为白色
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };

		glm::vec2 TexCoord = { 0.0f, 0.0f };

		// 默认纹理索引为0，表示使用白色纹理
		float TexIndex = 0.0f;

		float TilingFactor = 1.0f;

		int EntityID = -1; // 用于场景编辑器中选择实体，默认为-1表示没有实体
	};
	struct Renderer2DQuadData
	{
		Ref<PipelineStateObject> QuadPipeline;

		// 单次渲染调用中最多可以渲染的四边形数量
		static const uint32_t MaxQuads = 20000;

		// 每个四边形有4个顶点数据
		static const uint32_t MaxVertices = MaxQuads * 4;

		// 每个四边形有6个顶点索引
		static const uint32_t MaxIndices = MaxQuads * 6;

		// 当前渲染调用中已经提交的四边形数量
		uint32_t QuadIndexCount = 0;

		Ref<VertexArray> QuadVertexArray;

		Ref<VertexBuffer> QuadVertexBuffer;

		// 四边形顶点数据的缓冲区
		QuadVertex* QuadVertexBufferBase = nullptr;
		// 四边形顶点数据的当前指针
		QuadVertex* QuadVertexBufferPtr = nullptr;

	};
	struct CircleVertex
	{
		glm::vec3 WorldPosition = { 0.0f, 0.0f, 0.0f };

		glm::vec3 LocalPosition = { 0.0f, 0.0f, 0.0f };

		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };

		float Thickness = 1.0f;

		float Fade = 0.005f;

		int EntityID = -1;
	};

	struct Renderer2DCircleData
	{
		Ref<PipelineStateObject> CirclePipeline;

		// 单次渲染调用中最多可以渲染的四边形数量
		static const uint32_t MaxCircles = 20000;

		// 每个四边形有4个顶点数据
		static const uint32_t MaxVertices = MaxCircles * 4;

		// 每个四边形有6个顶点索引
		static const uint32_t MaxIndices = MaxCircles * 6;

		uint32_t CircleIndexCount = 0;

		Ref<VertexArray> CircleVertexArray;
		Ref<VertexBuffer> CircleVertexBuffer;
		CircleVertex* CircleVertexBufferBase = nullptr;
		CircleVertex* CircleVertexBufferPtr = nullptr;

	};

	struct LineVertex
	{
		glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		int EntityID = -1;
	};
	struct Renderer2DLineData
	{
		Ref<PipelineStateObject> LinePipeline;

		static const uint32_t MaxLines = 10000;
		static const uint32_t MaxVertices = MaxLines * 2;
		static const uint32_t MaxIndices = MaxLines * 2;
		uint32_t LineIndexCount = 0;

		Ref<VertexArray> LineVertexArray;
		Ref<VertexBuffer> LineVertexBuffer;
		LineVertex* LineVertexBufferBase = nullptr;
		LineVertex* LineVertexBufferPtr = nullptr;

		const float Thickness = 2.0f;
	};

	struct Renderer2DData
	{
		Renderer2DQuadData QuadData;
		Renderer2DCircleData CircleData;
		Renderer2DLineData LineData;

		Ref<Texture2D> WhiteTexture;
		Renderer2D::Statistics Stats;
		static const uint32_t MaxTextureSlots = 32; // 32是OpenGL至少支持的最大纹理单元数量
		// 纹理槽数组，存储当前渲染调用中使用的纹理对象
		std::array<Ref<Texture2D>, MaxTextureSlots> TextureSlots;
		uint32_t TextureSlotIndex = 1; // 0 预留给白色纹理
		glm::vec4 VertexPositions[4] =
		{
			{-0.5f,-0.5f,0.0f,1.0f},
			{ 0.5f,-0.5f,0.0f,1.0f},
			{ 0.5f, 0.5f,0.0f,1.0f},
			{-0.5f, 0.5f,0.0f,1.0f}
		};

		Ref<DescriptorSet> TextureDescriptorSet;
	};

	static Renderer2DData s_Data;
	void Renderer2D::Init()
	{
		WLD_PROFILE_FUNCTION();
		{
			s_Data.TextureDescriptorSet = CreateRef<DescriptorSet>();
		}
		// Quad渲染数据初始化
		{
			//Vertex Array
			s_Data.QuadData.QuadVertexArray = VertexArray::Create();

			// Shader
			Ref<Shader>  quadShader = Shader::Create();
			quadShader->AddShader("assets/shaders/Renderer2D_Quad.hlsl", Shader::ShaderType::Vertex);
			quadShader->AddShader("assets/shaders/Renderer2D_Quad.hlsl", Shader::ShaderType::Fragment);
			quadShader->Compile();


			//Vertex Buffer
			PipelineSpecification quadPipelineSpec;
			quadPipelineSpec.Shader = quadShader;
			quadPipelineSpec.Layout = {
				{ ShaderDataType::Float3, "a_Position",0},
				{ ShaderDataType::Float4, "a_Color",1 },
				{ ShaderDataType::Float2, "a_TexCoord",2 },
				{ ShaderDataType::Float, "a_TexIndex",3 },
				{ ShaderDataType::Float, "a_TilingFactor",4 },
				{ ShaderDataType::Int, "a_EntityID",5 }
			};
			s_Data.QuadData.QuadPipeline = PipelineStateObject::Create(quadPipelineSpec);

			s_Data.QuadData.QuadVertexBuffer = VertexBuffer::Create(s_Data.QuadData.MaxVertices * sizeof(QuadVertex));
			s_Data.QuadData.QuadVertexBuffer->SetLayout(s_Data.QuadData.QuadPipeline->GetSpecification().Layout);
			s_Data.QuadData.QuadVertexArray->AddVertexBuffer(s_Data.QuadData.QuadVertexBuffer,
				s_Data.QuadData.QuadPipeline->GetSpecification().Shader);

			// 为四边形顶点数据结构分配内存
			s_Data.QuadData.QuadVertexBufferBase = new QuadVertex[s_Data.QuadData.MaxVertices];

			//Index Buffer
			// 构建四边形索引数据，每个四边形由两个三角形组成，共6个顶点索引
			uint32_t* quadIndices = new uint32_t[s_Data.QuadData.MaxIndices];
			uint32_t offset = 0;
			for (uint32_t i = 0; i < s_Data.QuadData.MaxIndices; i += 6)
			{
				// {0,1,2,2,3,0}
				quadIndices[i + 0] = offset + 0;
				quadIndices[i + 1] = offset + 1;
				quadIndices[i + 2] = offset + 2;

				quadIndices[i + 3] = offset + 2;
				quadIndices[i + 4] = offset + 3;
				quadIndices[i + 5] = offset + 0;

				// 下一个四边形的顶点索引从当前偏移量开始，偏移量每次增加4（每个四边形有4个顶点）
				offset += 4;
			}

			Ref<IndexBuffer> quadIB = IndexBuffer::Create(quadIndices, s_Data.QuadData.MaxIndices);
			s_Data.QuadData.QuadVertexArray->SetIndexBuffer(quadIB);
			delete[] quadIndices;
		}

		// Circle渲染数据初始化
		{
			// Vertex Array
			s_Data.CircleData.CircleVertexArray = VertexArray::Create();

			// Shader
			Ref<Shader> circleShader = Shader::Create();
			circleShader->AddShader("assets/shaders/Renderer2D_Circle.hlsl", Shader::ShaderType::Vertex);
			circleShader->AddShader("assets/shaders/Renderer2D_Circle.hlsl", Shader::ShaderType::Fragment);
			circleShader->Compile();

			// CircleVertexBuffer
			PipelineSpecification circlePipelineSpec;
			circlePipelineSpec.Shader = circleShader;
			circlePipelineSpec.Layout = {
				{ ShaderDataType::Float3, "WorldPosition",0},
				{ ShaderDataType::Float3, "LocalPosition",1 },
				{ ShaderDataType::Float4, "Color",2 },
				{ ShaderDataType::Float, "Thickness",3 },
				{ ShaderDataType::Float, "Fade",4 },
				{ ShaderDataType::Int, "EntityID",5 }
			};
			s_Data.CircleData.CirclePipeline = PipelineStateObject::Create(circlePipelineSpec);

			s_Data.CircleData.CircleVertexBuffer = VertexBuffer::Create(s_Data.CircleData.MaxVertices * sizeof(CircleVertex));
			s_Data.CircleData.CircleVertexBuffer->SetLayout(s_Data.CircleData.CirclePipeline->GetSpecification().Layout);
			s_Data.CircleData.CircleVertexArray->AddVertexBuffer(s_Data.CircleData.CircleVertexBuffer, s_Data.CircleData.CirclePipeline->GetSpecification().Shader);
			// 为圆形顶点数据结构分配内存
			s_Data.CircleData.CircleVertexBufferBase = new CircleVertex[s_Data.CircleData.MaxVertices];

			// Index Buffer
			uint32_t* circleIndices = new uint32_t[s_Data.CircleData.MaxIndices];
			uint32_t offset = 0;
			for (uint32_t i = 0; i < s_Data.CircleData.MaxIndices; i += 6)
			{
				circleIndices[i + 0] = offset + 0;
				circleIndices[i + 1] = offset + 1;
				circleIndices[i + 2] = offset + 2;
				circleIndices[i + 3] = offset + 2;
				circleIndices[i + 4] = offset + 3;
				circleIndices[i + 5] = offset + 0;
				offset += 4;
			}
			Ref<IndexBuffer> circleIB = IndexBuffer::Create(circleIndices, s_Data.CircleData.MaxIndices);
			s_Data.CircleData.CircleVertexArray->SetIndexBuffer(circleIB);
			delete[] circleIndices;
		}

		// Line渲染数据初始化
		{
			RenderCommand::SetLineWidth(s_Data.LineData.Thickness);

			// Vertex Array
			s_Data.LineData.LineVertexArray = VertexArray::Create();

			// Shader
			Ref<Shader> lineShader = Shader::Create();
			lineShader->AddShader("assets/shaders/Renderer2D_Line.hlsl", Shader::ShaderType::Vertex);
			lineShader->AddShader("assets/shaders/Renderer2D_Line.hlsl", Shader::ShaderType::Fragment);
			lineShader->Compile();

			// LineVertexBuffer
			PipelineSpecification linePipelineSpec;
			linePipelineSpec.Shader = lineShader;
			linePipelineSpec.Layout = {
				{ ShaderDataType::Float3, "a_Position",0},
				{ ShaderDataType::Float4, "a_Color",1 },
				{ ShaderDataType::Int, "a_EntityID",2 }
			};
			s_Data.LineData.LinePipeline = PipelineStateObject::Create(linePipelineSpec);

			s_Data.LineData.LineVertexBuffer = VertexBuffer::Create(s_Data.LineData.MaxVertices * sizeof(LineVertex));
			s_Data.LineData.LineVertexBuffer->SetLayout(s_Data.LineData.LinePipeline->GetSpecification().Layout);
			s_Data.LineData.LineVertexArray->AddVertexBuffer(s_Data.LineData.LineVertexBuffer, s_Data.LineData.LinePipeline->GetSpecification().Shader);
			s_Data.LineData.LineVertexBufferBase = new LineVertex[s_Data.LineData.MaxVertices];

			// Index Buffer
			uint32_t* lineIndices = new uint32_t[s_Data.LineData.MaxIndices];
			uint32_t lineOffset = 0;
			for (uint32_t i = 0; i < s_Data.LineData.MaxIndices; i += 2)
			{
				lineIndices[i + 0] = lineOffset + 0;
				lineIndices[i + 1] = lineOffset + 1;
				lineOffset += 2;
			}
			Ref<IndexBuffer> lineIB = IndexBuffer::Create(lineIndices, s_Data.LineData.MaxIndices);
			s_Data.LineData.LineVertexArray->SetIndexBuffer(lineIB);
			delete[] lineIndices;
		}


		// 创建一个1x1的白色纹理，作为默认纹理使用
		s_Data.WhiteTexture = Texture2D::Create(1, 1);
		uint32_t whiteTextureData = 0xffffffff;

		s_Data.WhiteTexture->SetData(&whiteTextureData, sizeof(uint32_t));
		// 将白色纹理绑定到纹理槽0，确保在渲染调用中默认使用白色纹理
		s_Data.TextureSlots[0] = s_Data.WhiteTexture;

	}

	void Renderer2D::BeginScene(const Camera& camera, const glm::mat4& transform, Ref<CommandBuffer> commandBuffer)
	{
		WLD_PROFILE_FUNCTION();
		s_CurrentCommandBuffer = commandBuffer;
		s_CurrentCommandBuffer->BindPipeline(s_Data.QuadData.QuadPipeline);

		glm::mat4 viewProjection = camera.GetProjectionMatrix() * glm::inverse(transform);

	}

	void Renderer2D::EndScene()
	{
		WLD_PROFILE_FUNCTION();
		Flush();

		s_CurrentCommandBuffer = nullptr;
	}

	void Renderer2D::StartBatch()
	{
		// 重置纹理槽索引，保留槽0给白色纹理
		s_Data.TextureSlotIndex = 1;

		// Quad
		{
			// 重置四边形索引计数
			s_Data.QuadData.QuadIndexCount = 0;
			// 重置四边形顶点数据的当前指针
			s_Data.QuadData.QuadVertexBufferPtr = s_Data.QuadData.QuadVertexBufferBase;
		}

		// Circle
		{

			s_Data.CircleData.CircleIndexCount = 0;
			s_Data.CircleData.CircleVertexBufferPtr = s_Data.CircleData.CircleVertexBufferBase;
		}

		// Line
		{
			s_Data.LineData.LineIndexCount = 0;
			s_Data.LineData.LineVertexBufferPtr = s_Data.LineData.LineVertexBufferBase;
		}

	}

	void Renderer2D::Flush()
	{
		if (s_Data.LineData.LineVertexBufferPtr != s_Data.LineData.LineVertexBufferBase && s_Data.LineData.LineVertexBufferPtr != nullptr)
		{
			uint32_t lineDataSize = (uint32_t)((uint8_t*)s_Data.LineData.LineVertexBufferPtr - (uint8_t*)s_Data.LineData.LineVertexBufferBase);
			s_CurrentCommandBuffer->SetBufferData(s_Data.LineData.LineVertexBuffer, s_Data.LineData.LineVertexBufferBase, lineDataSize);

			s_CurrentCommandBuffer->BindPipeline(s_Data.LineData.LinePipeline);
			s_CurrentCommandBuffer->DrawLines(s_Data.LineData.LineVertexArray, s_Data.LineData.LineIndexCount);

			s_Data.Stats.DrawCalls++;

		}
		if (s_Data.QuadData.QuadIndexCount > 0)
		{
			// 计算已经提交的四边形顶点数据的大小，并将数据上传到GPU
			// 当前指针减去基地址得到已经提交的顶点数据的字节大小
			uint32_t dataSize = (uint32_t)((uint8_t*)s_Data.QuadData.QuadVertexBufferPtr - (uint8_t*)s_Data.QuadData.QuadVertexBufferBase);
			s_CurrentCommandBuffer->SetBufferData(s_Data.QuadData.QuadVertexBuffer, s_Data.QuadData.QuadVertexBufferBase, dataSize);

			s_Data.TextureDescriptorSet->SetTextures(DescriptorBindings::Textures::Batching2D::BaseSlot, s_Data.TextureSlots);

			s_CurrentCommandBuffer->BindDescriptorSet(s_Data.TextureDescriptorSet);

			s_CurrentCommandBuffer->BindPipeline(s_Data.QuadData.QuadPipeline);
			s_CurrentCommandBuffer->DrawIndexed(s_Data.QuadData.QuadVertexArray, s_Data.QuadData.QuadIndexCount);

			s_Data.Stats.DrawCalls++;
		}

		if (s_Data.CircleData.CircleVertexBufferPtr != s_Data.CircleData.CircleVertexBufferBase && s_Data.CircleData.CircleVertexBufferPtr != nullptr)
		{
			uint32_t circleDataSize = (uint32_t)((uint8_t*)s_Data.CircleData.CircleVertexBufferPtr - (uint8_t*)s_Data.CircleData.CircleVertexBufferBase);
			s_CurrentCommandBuffer->SetBufferData(s_Data.CircleData.CircleVertexBuffer, s_Data.CircleData.CircleVertexBufferBase, circleDataSize);

			s_CurrentCommandBuffer->BindPipeline(s_Data.CircleData.CirclePipeline);
			s_CurrentCommandBuffer->DrawIndexed(s_Data.CircleData.CircleVertexArray, s_Data.CircleData.CircleIndexCount);

			s_Data.Stats.DrawCalls++;
		}

	}
	void Renderer2D::DrawQuadCore(const glm::mat4& transform, const Ref<Texture2D>& texture, const glm::vec4& color, const glm::vec2* texCoords, float tilingFactor, int entityID)
	{
		WLD_PROFILE_FUNCTION();

		// 如果超出了批次允许的最大顶点索引数，直接开启下一批次
		if (s_Data.QuadData.QuadIndexCount >= s_Data.QuadData.MaxIndices)
		{
			NextBatch();
		}

		// 处理 UV
		constexpr glm::vec2 defaultTexCoords[] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
		const glm::vec2* actualTexCoords = texCoords != nullptr ? texCoords : defaultTexCoords;

		// 处理 纹理
		float textureIndex = 0.0f; // 默认使用 0 号槽位，即白纹理 (WhiteTexture)

		if (texture != nullptr)
		{
			// 检查纹理是否已经绑定在当前批次的某个槽位中
			for (uint32_t i = 1; i < s_Data.TextureSlotIndex; i++)
			{
				if (*s_Data.TextureSlots[i] == *texture)
				{
					textureIndex = (float)i;
					break;
				}
			}

			// 如果纹理尚未在此批次中绑定，则进行分配
			if (textureIndex == 0.0f)
			{

				if (s_Data.TextureSlotIndex >= s_Data.MaxTextureSlots)
				{
					NextBatch();
				}
				if (s_Data.TextureSlotIndex < s_Data.MaxTextureSlots)
				{
					textureIndex = (float)s_Data.TextureSlotIndex;
					s_Data.TextureSlots[s_Data.TextureSlotIndex] = texture;
					s_Data.TextureSlotIndex++;
				}
			}
		}

		// 组装并上传顶点数据
		constexpr uint32_t quadVertexCount = 4;
		for (uint32_t i = 0; i < quadVertexCount; i++)
		{
			s_Data.QuadData.QuadVertexBufferPtr->Position = transform * s_Data.VertexPositions[i];
			s_Data.QuadData.QuadVertexBufferPtr->Color = color;
			s_Data.QuadData.QuadVertexBufferPtr->TexCoord = actualTexCoords[i];
			s_Data.QuadData.QuadVertexBufferPtr->TexIndex = textureIndex;
			s_Data.QuadData.QuadVertexBufferPtr->TilingFactor = tilingFactor;
			s_Data.QuadData.QuadVertexBufferPtr->EntityID = entityID;
			s_Data.QuadData.QuadVertexBufferPtr++;
		}

		// 更新统计数据
		s_Data.QuadData.QuadIndexCount += 6;
		s_Data.Stats.QuadCount++;
	}

	void Renderer2D::DrawCircleCore(const glm::mat4& transform, const glm::vec4& color, float thickness, float fade, int entityID)
	{
		WLD_PROFILE_FUNCTION();

		if (s_Data.CircleData.CircleIndexCount >= s_Data.CircleData.MaxIndices)
		{
			NextBatch();
		}

		constexpr uint32_t circleVertexCount = 4;
		for (uint32_t i = 0; i < circleVertexCount; i++)
		{
			s_Data.CircleData.CircleVertexBufferPtr->WorldPosition = transform * s_Data.VertexPositions[i];
			s_Data.CircleData.CircleVertexBufferPtr->LocalPosition = s_Data.VertexPositions[i] * 2.0f;
			s_Data.CircleData.CircleVertexBufferPtr->Color = color;
			s_Data.CircleData.CircleVertexBufferPtr->Thickness = thickness;
			s_Data.CircleData.CircleVertexBufferPtr->Fade = fade;
			s_Data.CircleData.CircleVertexBufferPtr->EntityID = entityID;
			s_Data.CircleData.CircleVertexBufferPtr++;
		}

		// 更新统计数据
		s_Data.CircleData.CircleIndexCount += 6;
		s_Data.Stats.CircleCount++;

	}

	void Renderer2D::DrawLineCore(const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& color, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		if (s_Data.LineData.LineIndexCount >= s_Data.LineData.MaxIndices)
		{
			NextBatch();
		}

		s_Data.LineData.LineVertexBufferPtr->Position = p0;
		s_Data.LineData.LineVertexBufferPtr->Color = color;
		s_Data.LineData.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineData.LineVertexBufferPtr++;
		s_Data.LineData.LineVertexBufferPtr->Position = p1;
		s_Data.LineData.LineVertexBufferPtr->Color = color;
		s_Data.LineData.LineVertexBufferPtr->EntityID = entityID;
		s_Data.LineData.LineVertexBufferPtr++;

		s_Data.LineData.LineIndexCount += 2;
		s_Data.Stats.LineCount++;
	}

	void Renderer2D::DrawRectCore(const glm::mat4& transform, const glm::vec4& color, int entityID)
	{
		WLD_PROFILE_FUNCTION();
		glm::vec3 vertices[4];
		for (uint32_t i = 0; i < 4; i++)
		{
			vertices[i] = transform * s_Data.VertexPositions[i];
		}
		DrawLineCore(vertices[0], vertices[1], color, entityID);
		DrawLineCore(vertices[1], vertices[2], color, entityID);
		DrawLineCore(vertices[2], vertices[3], color, entityID);
		DrawLineCore(vertices[3], vertices[0], color, entityID);
	}

	void Renderer2D::NextBatch()
	{
		WLD_CORE_ERROR("NextBatch don't work.Please fix it!");
		EndScene();

		StartBatch();
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuad(glm::vec3(position, 0.0f), size, color);
	}
	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color)
	{
		DrawQuadCore(glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f }), nullptr, color, nullptr, 1.0f, -1);
	}
	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor)
	{
		DrawQuad(glm::vec3(position, 0.0f), size, texture, tilingFactor);
	}
	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<Texture2D>& texture, float tilingFactor)
	{
		DrawQuadCore(glm::translate(glm::mat4(1.0f), position) * glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f }), texture, glm::vec4(1.0f), nullptr, tilingFactor, -1);
	}

	void Renderer2D::DrawQuad(const glm::vec2& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor)
	{
		DrawQuad(glm::vec3(position, 0.0f), size, subtexture, tilingFactor);
	}
	void Renderer2D::DrawQuad(const glm::vec3& position, const glm::vec2& size, const Ref<SubTexture2D>& subtexture, float tilingFactor)
	{
		WLD_PROFILE_FUNCTION();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuadCore(transform, subtexture->GetTexture(), { 1.0f, 1.0f, 1.0f, 1.0f }, subtexture->GetTexCoords(), tilingFactor, -1);
	}

	void Renderer2D::DrawQuad(const glm::mat4& transform, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawQuadCore(transform, texture, tintColor, nullptr, tilingFactor, -1);
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, color);
	}
	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const glm::vec4& color)
	{
		WLD_PROFILE_FUNCTION();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuadCore(transform, nullptr, color);
	}
	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, texture, tilingFactor, tintColor);
	}
	void Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<Texture2D>& texture, float tilingFactor, const glm::vec4& tintColor)
	{
		WLD_PROFILE_FUNCTION();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });
		DrawQuadCore(transform, texture, tintColor, nullptr, tilingFactor, -1);
	}

	Renderer2D::Statistics Renderer2D::GetStats()
	{
		return s_Data.Stats;
	}
	void Renderer2D::ResetStats()
	{
		memset(&s_Data.Stats, 0, sizeof(Statistics));
	}

	void Renderer2D::DrawRotatedQuad(const glm::vec2& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		DrawRotatedQuad(glm::vec3(position, 0.0f), size, rotation, subtexture, tilingFactor, tintColor);
	}
	void  Renderer2D::DrawRotatedQuad(const glm::vec3& position, const glm::vec2& size, float rotation, const Ref<SubTexture2D>& subtexture, float tilingFactor, const glm::vec4& tintColor)
	{
		WLD_PROFILE_FUNCTION();
		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawQuadCore(transform, subtexture->GetTexture(), tintColor, subtexture->GetTexCoords(), tilingFactor, -1);
	}
	void Renderer2D::DrawRect(const glm::vec3& position, const glm::vec2& size, const glm::vec4& color, float rotation)
	{
		WLD_PROFILE_FUNCTION();

		glm::mat4 transform = glm::translate(glm::mat4(1.0f), position)
			* glm::rotate(glm::mat4(1.0f), rotation, { 0.0f, 0.0f, 1.0f })
			* glm::scale(glm::mat4(1.0f), { size.x, size.y, 1.0f });

		DrawRectCore(transform, color, -1);
	}

}

