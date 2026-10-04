#include "wldpch.h"

#include "World/Renderer/Texture/TextureLibrary.h"

namespace World
{
	TextureLibrary& TextureLibrary::Get()
	{
		// 进程内唯一;符号随 WorldRuntime.dll 导出(先例:MaterialLibrary::Get())。
		static TextureLibrary instance;
		return instance;
	}

	void TextureLibrary::Shutdown()
	{
		Get().Clear();
	}

	Ref<Texture2D> TextureLibrary::Load(PathId path, std::string* error)
	{
		if (error) error->clear();
		if (!path.IsValid())
			return nullptr;

		if (const auto cached = m_Cache.find(path); cached != m_Cache.end())
			return cached->second;
		if (m_Failed.count(path))
			return nullptr;

		const std::string& logical = StringPool::Get().PathOf(path);
		if (logical.empty())
			return nullptr;

		Ref<Texture2D> texture = Texture2D::Create(logical);
		if (!texture || !texture.get())
		{
			// 失败不逐帧重试;可读原因给调用方一次警告用。
			m_Failed.insert(path);
			if (error) *error = "纹理创建失败 '" + logical + "'";
			return nullptr;
		}

		m_Cache.emplace(path, texture);
		return texture;
	}

	void TextureLibrary::Invalidate(PathId path)
	{
		m_Failed.erase(path);
		m_Cache.erase(path);
	}

	void TextureLibrary::Clear()
	{
		m_Cache.clear();
		m_Failed.clear();
	}

	std::size_t TextureLibrary::ResidentCount() const
	{
		return m_Cache.size();
	}
}