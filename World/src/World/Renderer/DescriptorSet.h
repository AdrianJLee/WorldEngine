#pragma once
#include "UniformBufferSet.h"
#include "Texture.h"

#include <map>

namespace World
{
	/*
•	Pass / Global
•	b0 ~ b7
•	Material
•	b8 ~ b15
•	Object
•	b16 ~ b23
纹理槽同理：
•	Global textures
•	t0 ~ t7
•	Material textures
•	t8 ~ t23
*/
	namespace DescriptorBindings
	{
		// ==========================================
		// UBO 绑定槽位 (对应 Shader 中的 layout(binding = x) uniform xxx)
		// ==========================================
		namespace UniformBuffers
		{
			// 属于 Pass / Global 的 UBO (b0 ~ b7)
			namespace Pass
			{
				static constexpr uint32_t Camera = 0;           // b0: Camera (View/Projection)
				static constexpr uint32_t TextureIndices = 1;   // b1: Texture Indices (针对 2D batching 等)
				static constexpr uint32_t GlobalLighting = 2;   // b2: Global Lights / Environment Params
			}

			// 属于 Material 的 UBO (b8 ~ b15)
			namespace Material
			{
				static constexpr uint32_t Properties = 8;       // b8: 材质参数(AlbedoColor, Roughness 等)
			}

			// 属于 Object 的 UBO (b16 ~ b23)
			namespace Object
			{
				static constexpr uint32_t Transform = 16;       // b16: 模型矩阵
				static constexpr uint32_t EntityData = 17;      // b17: EntityID等
			}
		}

		// ==========================================
		// Texture / 纹理采样器 绑定槽位 (对应 Shader 中的 layout(binding = x) uniform sampler2D xxx)
		// ==========================================
		namespace Textures
		{
			// 全局使用的纹理资源 (t0 ~ t7)
			namespace Global
			{
				static constexpr uint32_t Environment = 0;      // t0: 静态环境贴图/Skybox
				static constexpr uint32_t ShadowMap = 1;        // t1: 阴影深度贴图
			}

			// 材质特有贴图部分 (t8 ~ t23)
			namespace Material
			{
				static constexpr uint32_t Albedo = 8;           // t8: 漫反射/基色贴图
				static constexpr uint32_t Normal = 9;           // t9: 法线贴图
				static constexpr uint32_t MetallicRoughness = 10; // t10: 金属性/粗糙度贴图
			}

			// 针对 2D Renderer 的批处理专属定义
			namespace Batching2D
			{
				static constexpr uint32_t BaseSlot = 0;         // 2D 渲染器专属的纹理数组起点(占用 t0 ~ t31)
			}
		}
	}

	struct u_TextureData
	{
		uint32_t Index;
		uint32_t Padding[3]; // 填充12个字节凑齐16字节
	};

	class DescriptorSet
	{
	public:
		void AddUniformBufferSet(uint32_t binding, uint32_t size, uint32_t maxFrames = 3);

		Ref<UniformBufferSet> GetUniformBufferSet(uint32_t binding);

		void SetTexture(uint32_t binding, Ref<Texture2D> texture);
		void SetTextures(uint32_t binding, const std::array<Ref<Texture2D>, 32>& textures);

		void Bind(uint32_t frameIndex);
	private:
		// <binding, UniformBufferSet>
		std::map<uint32_t, Ref<UniformBufferSet>> m_UniformBuffers;

		std::map<uint32_t, std::vector<Ref<Texture2D>>> m_Textures;
	};
}