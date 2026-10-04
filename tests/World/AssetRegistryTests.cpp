// 资产驻留层与显式资源表(World/Core/ResourceTable.h + World/Renderer/AssetRegistry.h)headless 单测。
//
// 验收口径:
//   1. ResourceTable:Emplace/TryGet/Get/Has/Remove/Clear 语义;Get 缺失必须**抛**而不是静默造默认值;
//      同类型重复 Emplace = 替换旧实例;键按类型名而不是 type_info 地址(跨 DLL 身份一致的前提)。
//   2. AssetRegistry:解析记账按帧推进;同路径命中同一实例;空 id 直接返回 nullptr 且不记账;
//      Pin 阻止回收、Unpin 后恢复可回收;超期后 CollectGarbage 真的清掉驻留表;
//      Describe() 能列出 "type path idle=N [pinned]"。
//   3. 归属:AssetRegistry 是 WorldContext::Resources() 里的**世界级服务**(不是进程单例)。
#include "World/Core/AssetRef.h"
#include "World/Core/ResourceTable.h"
#include "World/Asset/AssetCatalog.h"
#include "World/Renderer/AssetRegistry.h"
#include "World/Core/WorldContext.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using World::AssetId;
	using World::AssetRef;
	using World::AssetRegistry;
	using World::PathId;
	using World::Ref;
	using World::ResourceTable;
	using World::StringPool;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// ---- ResourceTable ----
	// 用存活计数证明替换/移除真的析构了旧实例(不能断言地址不同:分配器会复用地址)。
	struct ProbeService
	{
		static int Alive;
		int Value = 0;
		ProbeService() { ++Alive; }
		~ProbeService() { --Alive; }
	};
	int ProbeService::Alive = 0;
	struct OtherService
	{
		std::string Name;
	};

	void ResourceTableBasics()
	{
		ResourceTable table;
		CHECK(table.Size() == 0u);
		CHECK(table.TryGet<ProbeService>() == nullptr);
		CHECK(!table.Has<ProbeService>());

		ProbeService& first = table.Emplace<ProbeService>();
		first.Value = 7;
		CHECK(table.Has<ProbeService>());
		CHECK(table.Get<ProbeService>().Value == 7);
		CHECK(table.TryGet<ProbeService>() == &first);

		// 同类型重复登记 = 替换旧实例(旧实例必须被析构,而不是泄漏)。
		ProbeService& second = table.Emplace<ProbeService>();
		CHECK(ProbeService::Alive == 1);   // 旧实例已析构,只剩新的这一个
		second.Value = 3;
		CHECK(table.Get<ProbeService>().Value == 3);
		CHECK(table.Size() == 1u);

		// 不同类型各自一格。
		table.Emplace<OtherService>().Name = "x";
		CHECK(table.Size() == 2u);
		CHECK(table.Has<ProbeService>() && table.Has<OtherService>());
		CHECK(table.Get<OtherService>().Name == "x");

		CHECK(table.TypeNames().size() == 2u);

		CHECK(table.Remove<ProbeService>());
		CHECK(ProbeService::Alive == 0);   // 移除 = 析构
		CHECK(!table.Has<ProbeService>());
		CHECK(!table.Remove<ProbeService>());

		table.Clear();
		CHECK(table.Size() == 0u);
	}

	void ResourceTableMissingThrows()
	{
		ResourceTable table;
		bool threw = false;
		try
		{
			(void)table.Get<ProbeService>();
		}
		catch (const std::logic_error&)
		{
			threw = true;
		}
		CHECK(threw);   // 缺失必须抛:不静默造默认值
	}

	// ---- AssetRegistry ----
	void ResolveAccounting()
	{
		AssetRegistry registry;
		CHECK(registry.Frame() == 0u);

		// 空 id:nullptr,且不产生驻留条目。
		CHECK(registry.ResolveMesh(AssetRef {}) == nullptr);
		CHECK(registry.ResolveTexture(AssetRef {}) == nullptr);
		CHECK(registry.ResidentCount() == 0u);

		// 不存在的路径:加载失败(nullptr),但使用记账仍要落表(否则会逐帧重试打爆磁盘)。
		const PathId missing = StringPool::Get().InternPath("__assetregistry_probe__/missing.wmodel");
		CHECK(registry.ResolveMesh(AssetRef { missing, AssetId() }) == nullptr);
		CHECK(registry.ResidentCount() == 1u);
		CHECK(registry.Describe().size() == 1u);

		const uint64_t frame = registry.BeginFrame();
		CHECK(frame == 1u);
		CHECK(registry.Frame() == 1u);
	}

	void PinBlocksCollection()
	{
		AssetRegistry registry;
		const PathId path = StringPool::Get().InternPath("__assetregistry_probe__/pinned.wmodel");
		registry.ResolveMesh(AssetRef { path, AssetId() });
		registry.Pin(path);
		CHECK(registry.IsPinned(path));

		// 推进到远超空闲阈值:Pin 住的条目必须还在。
		for (int i = 0; i < static_cast<int>(AssetRegistry::kResidentIdleFrames) + 8; ++i)
			registry.BeginFrame();
		registry.CollectGarbage();
		CHECK(registry.ResidentCount() == 1u);
		CHECK(registry.Describe()[0].find("pinned") != std::string::npos);

		// Unpin 之后再推进一帧,即可回收。
		registry.Unpin(path);
		registry.BeginFrame();
		registry.CollectGarbage();
		CHECK(registry.ResidentCount() == 0u);
	}

	void ExpiredEntriesAreCollected()
	{
		AssetRegistry registry;
		const PathId a = StringPool::Get().InternPath("__assetregistry_probe__/a.wmodel");
		const PathId b = StringPool::Get().InternPath("__assetregistry_probe__/b.wmodel");
		registry.ResolveMesh(AssetRef { a, AssetId() });
		registry.BeginFrame();
		registry.ResolveMesh(AssetRef { b, AssetId() });
		CHECK(registry.ResidentCount() == 2u);

		// 只推进到"a 超期、b 还新鲜"的位置:回收后 b 必须留着。
		for (uint64_t i = 0; i < AssetRegistry::kResidentIdleFrames; ++i)
			registry.BeginFrame();
		registry.ResolveMesh(AssetRef { b, AssetId() });   // 刷新 b 的使用时间
		registry.BeginFrame();
		registry.CollectGarbage();
		CHECK(registry.ResidentCount() == 1u);
		CHECK(registry.Describe()[0].find("b.wmodel") != std::string::npos);
	}

	void InvalidateDropsEntry()
	{
		AssetRegistry registry;
		const PathId path = StringPool::Get().InternPath("__assetregistry_probe__/inv.wmodel");
		registry.ResolveMesh(AssetRef { path, AssetId() });
		CHECK(registry.ResidentCount() == 1u);
		registry.Invalidate(path);
		CHECK(registry.ResidentCount() == 0u);
	}

	// ---- 归属:世界资源表里的世界级服务 ----
	void WorldResourceOwnership()
	{
		World::WorldContext context;
		CHECK(!context.Resources().Has<AssetRegistry>());
		AssetRegistry& registry = context.Resources().Emplace<AssetRegistry>();
		CHECK(context.Resources().Has<AssetRegistry>());
		CHECK(context.Resources().TryGet<AssetRegistry>() == &registry);
		// Resources 是**显式成员**,不会因为场景数据拷贝而复制(ResourceTable 非拷贝)。
		CHECK(context.Resources().Size() == 1u);
	}
}

int main()
{
	try
	{
		ResourceTableBasics();
		ResourceTableMissingThrows();
		ResolveAccounting();
		PinBlocksCollection();
		ExpiredEntriesAreCollected();
		InvalidateDropsEntry();
		WorldResourceOwnership();
		std::printf("World.AssetRegistry: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.AssetRegistry: FAILED: %s\n", error.what());
		return 1;
	}
}