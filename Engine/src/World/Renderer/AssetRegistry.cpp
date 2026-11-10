#include "wldpch.h"

#include "World/Renderer/AssetRegistry.h"

#include "World/Renderer/MaterialLibrary.h"
#include "World/Asset/AssetCatalog.h"
#include "World/Asset/AsyncLoader.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/Texture/TextureLibrary.h"

namespace World
{
	AssetRegistry::Entry& AssetRegistry::Touch(PathId path, Kind kind)
	{
		Entry& entry = m_Entries[path];
		entry.Type = kind;
		entry.LastUse = m_Frame;
		return entry;
	}

	AssetCatalog* AssetRegistry::Catalog()
	{
		if (!m_Context)
			return nullptr;
		return m_Context->Resources().TryGet<AssetCatalog>();
	}

	PathId AssetRegistry::RemapByIdentity(const AssetRef& asset, const char* kindLabel)
	{
		// 改名/移动后的找回:只在身份可用且目录里有登记时才给新路径。
		// 注意这里**不**读取 asset.Path —— 旧路径此刻正是在 "文件已经不在了" 的状态。
		if (!asset.Identity.IsValid())
			return PathId();
		AssetCatalog* catalog = Catalog();
		if (!catalog)
			return PathId();
		const PathId found = catalog->FindPath(asset.Identity);
		if (found.IsValid())
		{
			WLD_CORE_WARN("[asset] {0} 引用按身份找回(改名/移动): {1} -> {2}", kindLabel,
				FormatAssetId(asset.Identity), StringPool::Get().PathOf(found));
			return found;
		}
		return PathId();
	}

	Ref<Mesh> AssetRegistry::ResolveMesh(const AssetRef& asset, std::string* error)
	{
		// 先按声明的路径解析(热路径);失败才动用身份目录 —— 目录只在改名/移动时才需要。
		PathId path = asset.Path;
		Ref<Mesh> mesh;
		if (path.IsValid())
		{
			Touch(path, Kind::Mesh);
			mesh = Mesh::LoadWModel(path, error);
		}
		if (!mesh)
		{
			if (const PathId remapped = RemapByIdentity(asset, "mesh"); remapped.IsValid() && remapped != path)
			{
				Touch(remapped, Kind::Mesh);
				mesh = Mesh::LoadWModel(remapped, error);
				path = remapped;
			}
		}
		if (!mesh && error && error->empty())
			*error = path.IsValid() ? "网格加载失败 '" + StringPool::Get().PathOf(path) + "'" : "path is empty";
		return mesh;
	}

	Rhi::Handle<Rhi::Texture> AssetRegistry::ResolveTexture(const AssetRef& asset, std::string* error)
	{
		// 纹理走与网格同一条"先按路径、失败再按身份找回"的顺序(改名不断链)。
		// 返回 RHI 句柄:GPU 纹理只有一份驻留(TextureLibrary),不再有 GL/RHI 两套。
		PathId path = asset.Path;
		Rhi::Handle<Rhi::Texture> texture;
		if (path.IsValid())
		{
			Touch(path, Kind::Texture);
			texture = TextureLibrary::Get().Get(path, /*srgb*/ true);
		}
		if (!texture)
		{
			if (const PathId remapped = RemapByIdentity(asset, "texture"); remapped.IsValid() && remapped != path)
			{
				Touch(remapped, Kind::Texture);
				texture = TextureLibrary::Get().Get(remapped, /*srgb*/ true);
				path = remapped;
			}
		}
		// 纹理的失败是"兜底成 1x1 白",句柄非空 ⇒ 这里只对"路径本来就无效"报错。
		if (!texture && error && error->empty())
			*error = path.IsValid() ? "纹理不可用 '" + StringPool::Get().PathOf(path) + "'" : "path is empty";
		return texture;
	}

	Ref<Material> AssetRegistry::ResolveMaterial(const AssetRef& asset, std::string* error)
	{
		PathId path = asset.Path;
		Ref<Material> material;
		if (path.IsValid())
		{
			Touch(path, Kind::Material);
			material = MaterialLibrary::Get().Load(path, error);
		}
		if (!material)
		{
			if (const PathId remapped = RemapByIdentity(asset, "material"); remapped.IsValid() && remapped != path)
			{
				Touch(remapped, Kind::Material);
				material = MaterialLibrary::Get().Load(remapped, error);
				path = remapped;
			}
		}
		if (!material && error && error->empty())
			*error = path.IsValid() ? "材质加载失败 '" + StringPool::Get().PathOf(path) + "'" : "path is empty";
		return material;
	}

	uint64_t AssetRegistry::BeginFrame()
	{
		return ++m_Frame;
	}

	std::size_t AssetRegistry::CollectGarbage()
	{
		if (m_Frame == 0)
			return 0;

		std::size_t freed = 0;
		// 收集候选后再删除:Evict 会改 m_Entries(不能边遍历边擦除)。
		std::vector<std::pair<PathId, Kind>> victims;
		for (const auto& [path, entry] : m_Entries)
		{
			if (m_Pinned.count(path))
				continue;
			// kResidentIdleFrames == 0 ⇒ 只要不是本帧用过的就回收(测试口径)。
			const uint64_t idle = m_Frame - entry.LastUse;
			if (idle > kResidentIdleFrames || (kResidentIdleFrames == 0 && idle > 0))
				victims.emplace_back(path, entry.Type);
		}

		for (const auto& [path, kind] : victims)
		{
			if (Evict(path, kind))
				++freed;
			m_Entries.erase(path);
		}
		return freed;
	}

	bool AssetRegistry::Evict(PathId path, Kind kind)
	{
		switch (kind)
		{
			case Kind::Mesh:
				// 真正让出:同时清掉 GPU 侧条目(延迟释放),否则顶点缓冲仍被强引用。
				return Mesh::EvictWModel(path);
			case Kind::Texture:
				TextureLibrary::Get().Invalidate(path);
				return true;
			case Kind::Material:
				// 边界:材质归编辑器文档生命周期,这里只清记账(见头文件说明)。
				return false;
		}
		return false;
	}

	void AssetRegistry::Pin(PathId path)
	{
		if (path.IsValid())
			m_Pinned.insert(path);
	}

	void AssetRegistry::Unpin(PathId path)
	{
		m_Pinned.erase(path);
	}

	bool AssetRegistry::IsPinned(PathId path) const
	{
		return m_Pinned.count(path) != 0;
	}

	void AssetRegistry::Invalidate(PathId path)
	{
		Mesh::EvictWModel(path);
		TextureLibrary::Get().Invalidate(path);
		m_Entries.erase(path);
	}

	void AssetRegistry::Clear()
	{
		for (const auto& [path, entry] : m_Entries)
			Evict(path, entry.Type);
		m_Entries.clear();
		m_Pinned.clear();
	}

	std::vector<std::string> AssetRegistry::Describe() const
	{
		std::vector<std::string> lines;
		lines.reserve(m_Entries.size());
		for (const auto& [path, entry] : m_Entries)
		{
			const char* type = entry.Type == Kind::Mesh ? "mesh"
				: (entry.Type == Kind::Texture ? "texture" : "material");
			lines.push_back(std::string(type) + " " + std::string(StringPool::Get().PathOf(path))
				+ " idle=" + std::to_string(m_Frame - entry.LastUse)
				+ (m_Pinned.count(path) ? " pinned" : ""));
		}
		return lines;
	}

	bool AssetRegistry::PrefetchMesh(const AssetRef& asset)
	{
		if (!m_Context || !asset.HasPath())
			return false;
		if (!m_Context->Resources().Has<AsyncLoader>())
			m_Context->Resources().Emplace<AsyncLoader>();
		AsyncLoader& loader = m_Context->Resources().Get<AsyncLoader>();
		// 已驻留的不用再排(下次 Resolve 直接命中);其余交给加载器(幂等)。
		loader.RequestMesh(asset.Path);
		return true;
	}

	std::size_t AssetRegistry::PendingLoadCount() const
	{
		if (!m_Context)
			return 0;
		const AsyncLoader* loader = m_Context->Resources().TryGet<AsyncLoader>();
		return loader ? loader->PendingCount() : 0;
	}

	std::string AssetRegistry::DescribeLoads() const
	{
		if (!m_Context)
			return {};
		const AsyncLoader* loader = m_Context->Resources().TryGet<AsyncLoader>();
		return loader ? loader->Describe() : std::string();
	}
}
