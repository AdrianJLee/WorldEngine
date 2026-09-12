#pragma once

// 资产字段操作的引擎内建特化。每个可序列化资产类型提供一个 AssetOps<T> 特化,
// 以路径字符串作为边界值。

#include "World/Core/Core.h"
#include "World/Renderer/Texture.h"
#include "World/Schema/Schema.h"

namespace World::Schema
{
	template <>
	struct AssetOps<Ref<Texture2D>>
	{
		static std::string GetPath(const Ref<Texture2D>& texture)
		{
			return texture && texture.get() ? texture->GetPath() : std::string();
		}

		static void SetPath(Ref<Texture2D>& texture, const std::string& path)
		{
			texture = path.empty() ? Ref<Texture2D>() : Texture2D::Create(path);
		}
	};
}
