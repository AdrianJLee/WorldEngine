#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace World
{
	struct WLD_API SubTexture2D
	{
		std::string Name;
		glm::vec2 TexCoords[4] {
			{ 0.0f, 0.0f }, // 0: Bottom-Left
			{ 1.0f, 0.0f }, // 1: Bottom-Right
			{ 1.0f, 1.0f }, // 2: Top-Right
			{ 0.0f, 1.0f }  // 3: Top-Left
		};
		glm::vec4 PixelRect { 0.0f, 0.0f, 0.0f, 0.0f }; // [x, y, width, height]
		glm::vec2 Pivot { 0.5f, 0.5f };                 // Normalized pivot point (0.5, 0.5 = center)
		glm::vec4 Border { 0.0f, 0.0f, 0.0f, 0.0f };    // [left, right, top, bottom] for 9-slice
		glm::vec2 OriginalSize { 0.0f, 0.0f };

		SubTexture2D() = default;
		SubTexture2D(std::string name, const glm::vec4& pixelRect, uint32_t atlasW, uint32_t atlasH,
			const glm::vec2& pivot = { 0.5f, 0.5f }, const glm::vec4& border = { 0.0f, 0.0f, 0.0f, 0.0f });
	};

	class WLD_API TextureAtlas
	{
	public:
		TextureAtlas() = default;
		explicit TextureAtlas(std::string texturePath, uint32_t width = 1, uint32_t height = 1);

		const std::string& GetTexturePath() const noexcept { return m_TexturePath; }
		void SetTexturePath(std::string path) { m_TexturePath = std::move(path); }

		uint32_t GetWidth() const noexcept { return m_Width; }
		uint32_t GetHeight() const noexcept { return m_Height; }
		void SetDimensions(uint32_t width, uint32_t height);

		void AddSubTexture(const SubTexture2D& subTexture);
		const SubTexture2D* FindSubTexture(const std::string& name) const;
		const SubTexture2D* FindSubTexture(NameId nameId) const;
		const SubTexture2D* GetSubTexture(size_t index) const;
		size_t GetSubTextureCount() const noexcept { return m_SubTextures.size(); }
		const std::vector<SubTexture2D>& GetAllSubTextures() const noexcept { return m_SubTextures; }

		std::string SerializeYaml() const;
		bool DeserializeYaml(const std::string& yaml);

		static std::shared_ptr<TextureAtlas> LoadFromFile(const std::string& filepath);
		static std::shared_ptr<TextureAtlas> GetOrLoad(const std::string& filepath);
		static void ClearCache();
		bool SaveToFile(const std::string& filepath) const;

	private:
		std::string m_TexturePath;
		uint32_t m_Width { 1 };
		uint32_t m_Height { 1 };
		std::vector<SubTexture2D> m_SubTextures;
		std::unordered_map<std::string, size_t> m_NameToIndex;
		std::unordered_map<uint32_t, size_t> m_NameIdToIndex;
	};
}
