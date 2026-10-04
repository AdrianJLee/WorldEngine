#pragma once

// 资产字段操作的引擎内建特化:边界是逻辑路径字符串,内存里是驻留 POD 标识。每个可序列化资产类型提供一个 AssetOps<T> 特化,
// 以路径字符串作为边界值。
//
// 2026-10-04:组件里的资产定位符改为驻留 POD 标识。`AssetOps<PathId>` 让 schema 层
// 完全无感 —— 生成器对 `Kind == Asset` 的字段一律发 `AssetOps<decltype(字段)>::GetPath/
// SetPath`,换成员类型(字符串 → 4B id)不需要改生成器;`.wd` 上仍然写人类可读的逻辑路径,
// 只在内存里换成 id。

#include "World/Core/Core.h"
#include "World/Core/AssetRef.h"
#include "World/Core/StringPool.h"
#include "World/Schema/Schema.h"

namespace World::Schema
{
	// 组件资产引用:边界(序列化/属性面板/Luau)是**逻辑路径字符串**(人读、脚本用);
	// 内存里是 16B POD(`AssetRef` = 驻留 PathId + 稳定 AssetId)。两个附加钩子把身份交给
	// schema 驱动序列化器(见 Core/AssetRef.h 与 Schema.h 的 GetAssetIdentity/SetAssetIdentity)。
	template <>
	struct AssetOps<World::AssetRef>
	{
		static std::string GetPath(const World::AssetRef& asset)
		{
			return std::string(World::StringPool::Get().PathOf(asset.Path));
		}

		static void SetPath(World::AssetRef& asset, const std::string& path)
		{
			const World::PathId next = path.empty()
				? World::PathId() : World::StringPool::Get().InternPath(path);
			// 手改路径 = "重新按路径解析":旧身份不再适用(否则会把新路径指向旧资产)。
			if (next != asset.Path)
				asset.Identity = World::AssetId {};
			asset.Path = next;
		}

		static uint64_t GetIdentity(const World::AssetRef& asset) { return asset.Identity.Value; }
		static void SetIdentity(World::AssetRef& asset, uint64_t identity)
		{
			asset.Identity = World::AssetId { identity };
		}
	};

	// 名字字段:边界(序列化/面板/Luau)是字符串,内存里是 4B 驻留 NameId。
	// 名字**不是身份**(同名不要求唯一),所以没有 AssetRef 那样的身份通道。
	template <>
	struct NameOps<World::NameId>
	{
		static std::string GetName(World::NameId id)
		{
			return std::string(World::StringPool::Get().NameOf(id));
		}

		static void SetName(World::NameId& id, const std::string& name)
		{
			id = name.empty() ? World::NameId() : World::StringPool::Get().InternName(name);
		}
	};

	template <>
	struct AssetOps<World::PathId>
	{
		static std::string GetPath(const World::PathId& id)
		{
			return std::string(World::StringPool::Get().PathOf(id));
		}

		static void SetPath(World::PathId& id, const std::string& path)
		{
			id = path.empty() ? World::PathId() : World::StringPool::Get().InternPath(path);
		}
	};
}
