#include "wldpch.h"
#include "SubTexture2D.h"

namespace World
{
	Ref<SubTexture2D> SubTexture2D::CreateFromCoords(const Ref<Texture2D>& texture, const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize)
	{
		glm::vec2 min = { (coords.x * cellSize.x) / texture->GetWidth(), (coords.y * cellSize.y) / texture->GetHeight() };
		glm::vec2 max = { ((coords.x + spriteSize.x) * cellSize.x) / texture->GetWidth(), ((coords.y + spriteSize.y) * cellSize.y) / texture->GetHeight() };
		return CreateRef<SubTexture2D>(texture, min, max);
	}
	SubTexture2D::SubTexture2D(const Ref<Texture2D>& texture, const glm::vec2& min, const glm::vec2& max)
		:m_Texture(texture), m_Min(min), m_Max(max)
	{
		m_TexCoords[0] = { m_Min.x, m_Min.y };
		m_TexCoords[1] = { m_Max.x, m_Min.y };
		m_TexCoords[2] = { m_Max.x, m_Max.y };
		m_TexCoords[3] = { m_Min.x, m_Max.y };
	}
	void SubTexture2D::Reset(const glm::vec2& coords, const glm::vec2& cellSize, const glm::vec2& spriteSize)
	{
		m_Min = { (coords.x * cellSize.x) / m_Texture->GetWidth(), (coords.y * cellSize.y) / m_Texture->GetHeight() };
		m_Max = { ((coords.x + spriteSize.x) * cellSize.x) / m_Texture->GetWidth(), ((coords.y + spriteSize.y) * cellSize.y) / m_Texture->GetHeight() };
		m_TexCoords[0] = { m_Min.x, m_Min.y };
		m_TexCoords[1] = { m_Max.x, m_Min.y };
		m_TexCoords[2] = { m_Max.x, m_Max.y };
		m_TexCoords[3] = { m_Min.x, m_Max.y };
	}

}