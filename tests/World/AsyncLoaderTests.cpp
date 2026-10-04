// T5c(2026-10-04):异步加载器 —— 工作线程解析 + 主线程提交的两阶段。
//
// 验收口径:
//   1. 异步路径产出的 Mesh 与**同步** LoadWModel 逐字节相同(共用 BuildFromWModel,一致性是结构性的);
//   2. RequestMesh 幂等:第二次不重排任务,状态稳定;
//   3. Pending → PumpCompletions → Mesh 缓存命中;提交后异步层不再记账;
//   4. 内联回退(WLD_NO_PARALLEL / JobSystem 未运行)结果与异步路径逐字节相同;
//   5. 坏文件 ⇒ Failed + 可读原因,且不会"逐帧重试";
//   6. Cancel 后工作线程的结果被丢弃(不复活条目),PumpCompletions 不得把它塞进缓存;
//   7. ScenePrefetch:走查组件登记请求,幂等计数正确。
#include "World/Asset/AsyncLoader.h"
#include "World/Asset/ScenePrefetch.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
	using World::AssetId;
	using World::AssetRef;
	using World::AsyncLoader;
	using World::PathId;
	using World::Ref;
	using World::StringPool;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 与 ModelIOTests 同形的极小模型(3 顶点 1 三角形),确定性字节。
	World::Asset::WModelData MakeTinyModel()
	{
		World::Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Identity = AssetId { 0x7117ull };
		model.Vertices = {
			{ { 0, 0, 0 }, { 0, 1, 0 }, { 0, 0 } },
			{ { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0 } },
			{ { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1 } },
		};
		model.Indices = { 0, 1, 2 };
		model.Bounds.Min = { 0, 0, 0 };
		model.Bounds.Max = { 1, 1, 0 };
		World::Asset::WModelSubmesh submesh;
		submesh.IndexOffset = 0;
		submesh.IndexCount = 3;
		model.Submeshes.push_back(submesh);
		World::Asset::WModelMeshRange range;
		range.FirstSubmesh = 0;
		range.SubmeshCount = 1;
		model.Meshes.push_back(range);
		return model;
	}

	void WriteModel(const std::filesystem::path& path)
	{
		std::filesystem::create_directories(path.parent_path());
		std::string error;
		CHECK(World::Asset::WModelIO::WriteFile(path.string(), MakeTinyModel(), &error));
		CHECK(error.empty());
	}

	// 逐字节比较两个 Mesh 的 CPU 侧内容(顶点/索引/包围盒/子网格/节点/材质槽)。
	bool SameMeshBytes(const World::Mesh& a, const World::Mesh& b)
	{
		const World::MeshDesc& da = a.GetDesc();
		const World::MeshDesc& db = b.GetDesc();
		if (da.VertexLayoutId != db.VertexLayoutId) return false;
		if (da.VertexData != db.VertexData) return false;
		if (da.Indices != db.Indices) return false;
		const World::MeshBounds& ba = a.GetBounds();
		const World::MeshBounds& bb = b.GetBounds();
		if (ba.Min != bb.Min || ba.Max != bb.Max) return false;
		if (a.GetSubmeshes().size() != b.GetSubmeshes().size()) return false;
		for (std::size_t i = 0; i < a.GetSubmeshes().size(); ++i)
		{
			const World::MeshSubmesh& sa = a.GetSubmeshes()[i];
			const World::MeshSubmesh& sb = b.GetSubmeshes()[i];
			if (sa.IndexOffset != sb.IndexOffset || sa.IndexCount != sb.IndexCount
				|| sa.MaterialSlot != sb.MaterialSlot)
				return false;
		}
		return a.GetMaterialSlots() == b.GetMaterialSlots();
	}

	// ---- 1/3/4. 异步 == 同步(逐字节),且提交后缓存命中 ----
	void AsyncMatchesSync(const std::filesystem::path& root)
	{
		const std::filesystem::path file = root / "models" / "tiny.wmodel";
		WriteModel(file);
		const std::string logical = file.string();

		// 同步基准(先清缓存,避免相互污染)。
		World::Mesh::ClearWModelCache();
		std::string syncError;
		Ref<World::Mesh> syncMesh = World::Mesh::LoadWModel(logical, &syncError);
		CHECK(syncMesh != nullptr);
		CHECK(syncError.empty());
		World::Mesh::ClearWModelCache();

		AsyncLoader loader;
		const PathId path = StringPool::Get().InternPath(logical);
		// 第一次:登记(内联或异步,取决于 JobSystem 是否在跑)。
		const AsyncLoader::Status first = loader.RequestMesh(path);
		CHECK(first.State == AsyncLoader::Phase::Pending || first.State == AsyncLoader::Phase::Ready);

		// 幂等:第二次请求不该改变状态(仍是在飞或已就绪)。
		const AsyncLoader::Status second = loader.RequestMesh(path);
		CHECK(second.State == first.State);

		const std::size_t committed = loader.PumpCompletions();
		CHECK(committed <= 1u);

		// 提交之后:异步层记账清空,权威状态在 Mesh 的缓存里。
		CHECK(loader.PendingCount() == 0u);
		CHECK(loader.ReadyCount() == 0u);

		std::string asyncError;
		Ref<World::Mesh> fromCache = World::Mesh::LoadWModel(path, &asyncError);
		CHECK(fromCache != nullptr);
		CHECK(fromCache == syncMesh || SameMeshBytes(*fromCache, *syncMesh));
		World::Mesh::ClearWModelCache();
	}

	// ---- 5. 坏文件 ⇒ Failed + 原因,不重排 ----
	void BrokenFileFailsOnce(const std::filesystem::path& root)
	{
		const std::filesystem::path file = root / "models" / "broken.wmodel";
		std::filesystem::create_directories(file.parent_path());
		{
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out << "not a wmodel";
		}
		const PathId path = StringPool::Get().InternPath(file.string());

		AsyncLoader loader;
		loader.RequestMesh(path);
		loader.PumpCompletions();
		const AsyncLoader::Status status = loader.Query(path);
		CHECK(status.State == AsyncLoader::Phase::Failed);
		CHECK(!status.Error.empty());
		CHECK(loader.FailedCount() == 1u);
		// 失败不重试:再请求仍是 Failed(不会重新排任务)。
		CHECK(loader.RequestMesh(path).State == AsyncLoader::Phase::Failed);

		// 空路径直接拒绝,不记账。
		CHECK(loader.RequestMesh(PathId()).State == AsyncLoader::Phase::Failed);
	}

	// ---- 6. Cancel 丢弃在飞结果 ----
	void CancelDropsInFlight(const std::filesystem::path& root)
	{
		const std::filesystem::path file = root / "models" / "cancel.wmodel";
		WriteModel(file);
		const PathId path = StringPool::Get().InternPath(file.string());

		World::Mesh::ClearWModelCache();
		AsyncLoader loader;
		loader.RequestMesh(path);
		loader.Cancel(path);
		CHECK(loader.Query(path).State == AsyncLoader::Phase::None);
		// 即使工作线程已经算完,提交点也不得把它塞进缓存。
		loader.PumpCompletions();
		CHECK(loader.PendingCount() == 0u);
		// EvictWModel 返回"该键是否存在" ⇒ 用它探测缓存里确实**没有**被塞进来的条目。
		CHECK(!World::Mesh::EvictWModel(path));
		// 而同步路径仍然可用(取消的是"这次异步加载",不是这个资产)。
		CHECK(World::Mesh::LoadWModel(path, nullptr) != nullptr);
		World::Mesh::ClearWModelCache();
	}

	// ---- 7. 场景预取 ----
	void ScenePrefetchCounts(const std::filesystem::path& root)
	{
		const std::filesystem::path file = root / "models" / "scene_probe.wmodel";
		WriteModel(file);

		World::WorldContext context;
		Ref<World::Scene> scene = World::CreateRef<World::Scene>(context);
		const entt::entity a = scene->GetRegistry().create();
		World::MeshRendererComponent renderer;
		renderer.Mesh.Path = StringPool::Get().InternPath(file.string());
		scene->GetRegistry().emplace<World::MeshRendererComponent>(a, renderer);
		// 第二个实体引用同一路径:预取应计为"已知",不重复请求。
		const entt::entity b = scene->GetRegistry().create();
		scene->GetRegistry().emplace<World::MeshRendererComponent>(b, renderer);
		// 一个空的渲染组件:没有路径 ⇒ 不请求。
		const entt::entity c = scene->GetRegistry().create();
		scene->GetRegistry().emplace<World::MeshRendererComponent>(c, World::MeshRendererComponent {});

		const World::ScenePrefetchResult result = World::PrefetchSceneAssets(context, *scene);
		CHECK(result.Requested == 1u);
		CHECK(result.AlreadyKnown == 1u);
		CHECK(context.Resources().Has<AsyncLoader>());
		World::Mesh::ClearWModelCache();
	}
}

int main()
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "we_async_loader_probe";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	try
	{
		AsyncMatchesSync(root / "sync");
		BrokenFileFailsOnce(root / "broken");
		CancelDropsInFlight(root / "cancel");
		ScenePrefetchCounts(root / "prefetch");
		std::filesystem::remove_all(root, ignored);
		std::printf("World.AsyncLoader: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::filesystem::remove_all(root, ignored);
		std::fprintf(stderr, "World.AsyncLoader: FAILED: %s\n", error.what());
		return 1;
	}
}
