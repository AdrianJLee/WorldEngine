#pragma once
#include "World/Renderer/Texture.h"

#include <glm/glm.hpp>

namespace World
{
	class SubTexture2D
	{
	public:
		static Ref<SubTexture2D> CreateFromCoords(const Ref<Texture2D>& texture, const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize = { 1.0f, 1.0f });
	public:
		SubTexture2D(const Ref<Texture2D>& texture, const glm::vec2& min, const glm::vec2& max);

		void Reset(const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize = { 1.0f, 1.0f });

		const Ref<Texture2D>& GetTexture() const { return m_Texture; }
		const glm::vec2* GetTexCoords() const { return m_TexCoords; }
		const glm::vec2& GetMin() const { return m_Min; }
		const glm::vec2& GetMax() const { return m_Max; }
	private:
		Ref<Texture2D> m_Texture;

		glm::vec2 m_Min;
		glm::vec2 m_Max;
		glm::vec2 m_TexCoords[4];
	};
}
