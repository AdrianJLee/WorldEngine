#include "World/Utils/Paths.h"
#include "TextureAtlas.h"
#include "World/Core/Log.h"

#include <filesystem>



#include <yaml-cpp/yaml.h>
#include <fstream>
#include <sstream>
#include <mutex>

namespace World
{
	namespace
	{
		std::filesystem::path ResolvePhysicalPath(const std::string& filepath)
		{
			if (filepath.empty())
				return {};
			std::error_code ec;
			std::filesystem::path p(filepath);
			if (std::filesystem::exists(p, ec))
				return p;
			std::filesystem::path fromProj = Paths::ProjectDir() / p;
			if (std::filesystem::exists(fromProj, ec))
				return fromProj;
			std::filesystem::path fromAsset = Paths::AssetRoot() / p;
			if (std::filesystem::exists(fromAsset, ec))
				return fromAsset;
			if (filepath.rfind("assets/", 0) == 0 || filepath.rfind("assets\\", 0) == 0)
			{
				std::filesystem::path stripped = Paths::AssetRoot() / filepath.substr(7);
				if (std::filesystem::exists(stripped, ec))
					return stripped;
			}
			return p;
		}
	}
	static std::unordered_map<std::string, std::shared_ptr<TextureAtlas>> s_AtlasCache;
	static std::mutex s_AtlasCacheMutex;

	SubTexture2D::SubTexture2D(std::string name, const glm::vec4& pixelRect, uint32_t atlasW, uint32_t atlasH,
		const glm::vec2& pivot, const glm::vec4& border)
		: Name(std::move(name)), PixelRect(pixelRect), Pivot(pivot), Border(border), OriginalSize(pixelRect.z, pixelRect.w)
	{
		const float invW = atlasW > 0 ? 1.0f / static_cast<float>(atlasW) : 1.0f;
		const float invH = atlasH > 0 ? 1.0f / static_cast<float>(atlasH) : 1.0f;

		const float u0 = pixelRect.x * invW;
		const float u1 = (pixelRect.x + pixelRect.z) * invW;
		const float v0 = (static_cast<float>(atlasH) - (pixelRect.y + pixelRect.w)) * invH;
		const float v1 = (static_cast<float>(atlasH) - pixelRect.y) * invH;

		TexCoords[0] = { u0, v0 }; // Bottom-Left
		TexCoords[1] = { u1, v0 }; // Bottom-Right
		TexCoords[2] = { u1, v1 }; // Top-Right
		TexCoords[3] = { u0, v1 }; // Top-Left
	}

	TextureAtlas::TextureAtlas(std::string texturePath, uint32_t width, uint32_t height)
		: m_TexturePath(std::move(texturePath)), m_Width(width > 0 ? width : 1), m_Height(height > 0 ? height : 1)
	{
	}

	void TextureAtlas::SetDimensions(uint32_t width, uint32_t height)
	{
		m_Width = width > 0 ? width : 1;
		m_Height = height > 0 ? height : 1;
		// Recompute UVs for all subtextures
		for (auto& sub : m_SubTextures)
		{
			sub = SubTexture2D(sub.Name, sub.PixelRect, m_Width, m_Height, sub.Pivot, sub.Border);
		}
	}

	void TextureAtlas::AddSubTexture(const SubTexture2D& subTexture)
	{
		const size_t index = m_SubTextures.size();
		m_NameToIndex[subTexture.Name] = index;
		const NameId nid = StringPool::Get().InternName(subTexture.Name);
		if (nid.IsValid())
		{
			m_NameIdToIndex[nid.Value] = index;
		}
		m_SubTextures.push_back(subTexture);
	}

	const SubTexture2D* TextureAtlas::FindSubTexture(const std::string& name) const
	{
		auto it = m_NameToIndex.find(name);
		if (it != m_NameToIndex.end() && it->second < m_SubTextures.size())
			return &m_SubTextures[it->second];
		return nullptr;
	}

	const SubTexture2D* TextureAtlas::FindSubTexture(NameId nameId) const
	{
		if (!nameId.IsValid()) return nullptr;
		auto it = m_NameIdToIndex.find(nameId.Value);
		if (it != m_NameIdToIndex.end() && it->second < m_SubTextures.size())
			return &m_SubTextures[it->second];
		return nullptr;
	}

	const SubTexture2D* TextureAtlas::GetSubTexture(size_t index) const
	{
		if (index < m_SubTextures.size())
			return &m_SubTextures[index];
		return nullptr;
	}

	std::string TextureAtlas::SerializeYaml() const
	{
		YAML::Emitter out;
		out << YAML::BeginMap;
		out << YAML::Key << "TexturePath" << YAML::Value << m_TexturePath;
		out << YAML::Key << "Width" << YAML::Value << m_Width;
		out << YAML::Key << "Height" << YAML::Value << m_Height;

		out << YAML::Key << "Sprites" << YAML::Value << YAML::BeginSeq;
		for (const auto& sub : m_SubTextures)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "Name" << YAML::Value << sub.Name;
			out << YAML::Key << "Rect" << YAML::Flow << YAML::BeginSeq
				<< sub.PixelRect.x << sub.PixelRect.y << sub.PixelRect.z << sub.PixelRect.w
				<< YAML::EndSeq;
			out << YAML::Key << "Pivot" << YAML::Flow << YAML::BeginSeq
				<< sub.Pivot.x << sub.Pivot.y
				<< YAML::EndSeq;
			out << YAML::Key << "Border" << YAML::Flow << YAML::BeginSeq
				<< sub.Border.x << sub.Border.y << sub.Border.z << sub.Border.w
				<< YAML::EndSeq;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;
		return std::string(out.c_str());
	}

	bool TextureAtlas::DeserializeYaml(const std::string& yamlStr)
	{
		try
		{
			YAML::Node data = YAML::Load(yamlStr);
			if (!data || !data.IsMap()) return false;

			if (data["TexturePath"]) m_TexturePath = data["TexturePath"].as<std::string>();
			if (data["Width"]) m_Width = data["Width"].as<uint32_t>();
			if (data["Height"]) m_Height = data["Height"].as<uint32_t>();
			if (m_Width == 0) m_Width = 1;
			if (m_Height == 0) m_Height = 1;

			m_SubTextures.clear();
			m_NameToIndex.clear();
			m_NameIdToIndex.clear();

			auto sprites = data["Sprites"];
			if (sprites && sprites.IsSequence())
			{
				for (const auto& item : sprites)
				{
					std::string name = item["Name"] ? item["Name"].as<std::string>() : "";
					glm::vec4 rect(0.0f);
					if (item["Rect"] && item["Rect"].IsSequence() && item["Rect"].size() == 4)
					{
						rect.x = item["Rect"][0].as<float>();
						rect.y = item["Rect"][1].as<float>();
						rect.z = item["Rect"][2].as<float>();
						rect.w = item["Rect"][3].as<float>();
					}
					glm::vec2 pivot(0.5f, 0.5f);
					if (item["Pivot"] && item["Pivot"].IsSequence() && item["Pivot"].size() == 2)
					{
						pivot.x = item["Pivot"][0].as<float>();
						pivot.y = item["Pivot"][1].as<float>();
					}
					glm::vec4 border(0.0f);
					if (item["Border"] && item["Border"].IsSequence() && item["Border"].size() == 4)
					{
						border.x = item["Border"][0].as<float>();
						border.y = item["Border"][1].as<float>();
						border.z = item["Border"][2].as<float>();
						border.w = item["Border"][3].as<float>();
					}

					SubTexture2D sub(name, rect, m_Width, m_Height, pivot, border);
					AddSubTexture(sub);
				}
			}
			return true;
		}
		catch (const std::exception& e)
		{
			WLD_CORE_WARN("[TextureAtlas] Failed to deserialize YAML: {}", e.what());
			return false;
		}
	}

	std::shared_ptr<TextureAtlas> TextureAtlas::LoadFromFile(const std::string& filepath)
	{
		const auto resolved = ResolvePhysicalPath(filepath);
		std::ifstream file(resolved.string());
		if (!file.is_open())
		{
			WLD_CORE_WARN("[TextureAtlas] Failed to open file: {}", filepath);
			return nullptr;
		}
		std::stringstream ss;
		ss << file.rdbuf();
		auto atlas = std::make_shared<TextureAtlas>();
		if (!atlas->DeserializeYaml(ss.str()))
		{
			return nullptr;
		}
		return atlas;
	}

	std::shared_ptr<TextureAtlas> TextureAtlas::GetOrLoad(const std::string& filepath)
	{
		std::lock_guard<std::mutex> lock(s_AtlasCacheMutex);
		auto it = s_AtlasCache.find(filepath);
		if (it != s_AtlasCache.end())
			return it->second;

		auto atlas = LoadFromFile(filepath);
		if (atlas)
		{
			s_AtlasCache[filepath] = atlas;
		}
		return atlas;
	}

	void TextureAtlas::ClearCache()
	{
		std::lock_guard<std::mutex> lock(s_AtlasCacheMutex);
		s_AtlasCache.clear();
	}

	bool TextureAtlas::SaveToFile(const std::string& filepath) const
	{
		std::ofstream file(filepath);
		if (!file.is_open())
		{
			WLD_CORE_WARN("[TextureAtlas] Failed to write file: {}", filepath);
			return false;
		}
		file << SerializeYaml();
		return true;
	}
}

