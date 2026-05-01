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
		namespace Pass
		{
			static constexpr uint32_t Camera = 0;
			static constexpr uint32_t TextureIndices = 1;
		}
		namespace Material
		{
			static constexpr uint32_t Albedo = 8;
			static constexpr uint32_t Normal = 9;
		}

		namespace Object
		{
			static constexpr uint32_t Transform = 16;
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

		void Bind(uint32_t frameIndex);
	private:
		// <binding, UniformBufferSet>
		std::map<uint32_t, Ref<UniformBufferSet>> m_UniformBuffers;

		std::map<uint32_t, Ref<Texture2D>> m_Textures;
	};
}