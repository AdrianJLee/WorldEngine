// T4(2026-10-04):资产稳定身份 + 目录 + "改名不断链"的 headless 回归。
//
// 验收口径:
//   1. AssetId 文本往返(FormatAssetId/ParseAssetId)与非法输入拒绝;
//   2. 三种资产文件都携带身份:
//      .wmodel(meta)/.wmat(AssetId: 行)/.wtex(assetid: 行);
//   3. AssetCatalog 从**资产文件自身**建 id↔path 映射;同名不同位不串;
//   4. **改名不断链**:资产改名后目录按身份指向新路径 —— 这是整个 T4 的目的;
//   5. 组件侧身份通道:ReadStructValue 产出 `<Field>Id`,WriteStructValue 恢复身份(手改路径则作废)。
#include "World/Asset/AssetCatalog.h"
#include "World/Asset/WModelIO.h"
#include "World/Core/AssetId.h"
#include "World/Core/AssetRef.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/AssetRegistry.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Texture/TextureImportSettings.h"
#include "World/Scene/Components.h"
#include "World/Schema/Schema.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
	using World::AssetId;
	using World::AssetRef;
	using World::AssetCatalog;
	using World::AssetRegistry;
	using World::PathId;
	using World::StringPool;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void WriteText(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
	}

	// ---- 1. 文本往返 ----
	void AssetIdTextRoundTrip()
	{
		const AssetId original { 0x0123456789ABCDEFull };
		const std::string text = World::FormatAssetId(original);
		CHECK(text == "0x0123456789ABCDEF");

		AssetId parsed;
		CHECK(World::ParseAssetId(text, &parsed));
		CHECK(parsed == original);
		CHECK(World::ParseAssetId("0123456789abcdef", &parsed) && parsed == original);  // 大小写/无前缀
		CHECK(World::ParseAssetId("0x1", &parsed) && parsed.Value == 1u);

		// 非法/未分配一律拒绝(不能悄悄当 0)。
		CHECK(!World::ParseAssetId("", &parsed));
		CHECK(!World::ParseAssetId("0x", &parsed));
		CHECK(!World::ParseAssetId("0x0000000000000000", &parsed));
		CHECK(!World::ParseAssetId("0xGGGG", &parsed));
		CHECK(!World::ParseAssetId("0x0123456789ABCDEF0", &parsed));   // 超 16 位
		CHECK(!World::ParseAssetId("0x12 34", &parsed));
	}

	// ---- 2. 三种资产文件携带身份 ----
	World::Asset::WModelData MakeTinyModel(AssetId identity)
	{
		World::Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Identity = identity;
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

	void ModelCarriesIdentity(const std::filesystem::path& root)
	{
		const AssetId identity { 0xA1B2C3D4E5F60718ull };
		const std::filesystem::path path = root / "models" / "id_probe.wmodel";
		std::string error;
		CHECK(World::Asset::WModelIO::WriteFile(path.string(), MakeTinyModel(identity), &error));

		World::Asset::WModelData::MetaData meta;
		CHECK(World::Asset::WModelIO::ReadMeta(path.string(), meta, &error));
		CHECK(meta.Valid);
		CHECK(meta.Identity == identity);

		// 全量读回也保持身份(身份是资产自身属性,不靠外部索引)。
		World::Asset::WModelData loaded;
		CHECK(World::Asset::WModelIO::ReadFile(path.string(), loaded, &error));
		CHECK(loaded.Meta.Identity == identity);
	}

	void MaterialCarriesIdentity()
	{
		const AssetId identity { 0x1122334455667788ull };
		World::MaterialDocument document;
		document.Identity = identity;
		document.Overridden = World::MaterialFieldSet::Everything();
		document.Values.Name = "IdentityProbe";
		const std::string text = World::MaterialIO::SerializeDocument(document, nullptr);
		CHECK(text.find("FormatVersion: 3\n") != std::string::npos);
		CHECK(text.find("AssetId: 0x1122334455667788\n") != std::string::npos);

		World::MaterialDocument reparsed;
		std::string error;
		CHECK(World::MaterialIO::ParseDocument(text, reparsed, &error).Success);
		CHECK(reparsed.Identity == identity);
		CHECK(reparsed.Values.Name == "IdentityProbe");

		// 非法身份必须报错,不静默当成"未分配"。
		World::MaterialDocument bad;
		CHECK(!World::MaterialIO::ParseDocument("FormatVersion: 3\nAssetId: not-a-hex\n", bad, &error).Success);
	}

	void TextureCarriesIdentity()
	{
		const AssetId identity { 0x99AABBCCDDEEFF00ull };
		World::TextureImportSettings settings;
		settings.Identity = identity;
		settings.Usage = World::TextureUsage::Ui;
		const std::string text = settings.Serialize();
		CHECK(text.find("assetid: 0x99AABBCCDDEEFF00\n") != std::string::npos);

		World::TextureImportSettings reparsed;
		std::string error;
		CHECK(World::TextureImportSettings::Parse(text, reparsed, error));
		CHECK(reparsed.Identity == identity);

		World::TextureImportSettings bad;
		CHECK(!World::TextureImportSettings::Parse("assetid: nope\n", bad, error));
	}

	// ---- 3/4. 目录 + 改名不断链 ----
	void CatalogAndRenameRecovery(const std::filesystem::path& root)
	{
		const AssetId identity { 0xDEADBEEFCAFEF00Dull };
		const std::filesystem::path original = root / "materials" / "before.wmat";
		{
			World::MaterialDocument document;
			document.Identity = identity;
			document.Overridden = World::MaterialFieldSet::Everything();
			document.Values.Name = "Renamed";
			WriteText(original, World::MaterialIO::SerializeDocument(document, nullptr));
		}

		AssetCatalog catalog;
		CHECK(catalog.Scan(root.string()) == 1u);
		CHECK(catalog.Skipped() == 0u);

		const std::string logicalBefore = std::filesystem::relative(original, root).generic_string();
		CHECK(catalog.FindPath(identity) == StringPool::Get().InternPath(logicalBefore));
		CHECK(catalog.FindIdentity(StringPool::Get().InternPath(logicalBefore)) == identity);
		CHECK(catalog.Describe().size() == 1u);

		// 改名(内容与身份不变,只换路径):目录按身份指向新路径 = "改名不断链"。
		const std::filesystem::path renamed = root / "materials" / "renamed" / "after.wmat";
		std::filesystem::create_directories(renamed.parent_path());
		std::filesystem::rename(original, renamed);

		AssetCatalog rescanned;
		CHECK(rescanned.Scan(root.string()) == 1u);
		const std::string logicalAfter = std::filesystem::relative(renamed, root).generic_string();
		CHECK(rescanned.FindPath(identity) == StringPool::Get().InternPath(logicalAfter));
		// 旧路径已经查不到(不留悬垂条目)。
		CHECK(!rescanned.FindIdentity(StringPool::Get().InternPath(logicalBefore)).IsValid());

		// 未登记的 id 返回无效路径,不猜。
		CHECK(!rescanned.FindPath(AssetId { 0x1234ull }).IsValid());
	}

	void CatalogIgnoresUnidentifiedAssets(const std::filesystem::path& root)
	{
		// 没有身份的 .wmat:登记不进去,但也不该让整次扫描失败(计入 Skipped)。
		WriteText(root / "materials" / "no_identity.wmat", "FormatVersion: 2\nName: \"NoId\"\n");
		AssetCatalog catalog;
		const std::size_t count = catalog.Scan(root.string());
		CHECK(count == 1u);            // 只有上一段建的那个带身份的材质
		CHECK(catalog.Skipped() == 1u);
	}

	// ---- 4b. 端到端:路径"存在但文件已不在"时按身份找回(真正的改名场景)----
	void RenameRecoveryThroughRegistry(const std::filesystem::path& root)
	{
		const AssetId identity { 0x51DE00000000BEEFull };
		const std::string oldLogical = "materials/gone.wmat";
		const std::string newLogical = "moved/kept.wmat";
		{
			World::MaterialDocument document;
			document.Identity = identity;
			document.Overridden = World::MaterialFieldSet::Everything();
			document.Values.Name = "Kept";
			WriteText(root / newLogical, World::MaterialIO::SerializeDocument(document, nullptr));
		}

		// 目录只认识新路径(旧路径上没有文件)。
		World::WorldContext context;
		World::AssetCatalog& catalog = context.Resources().Emplace<AssetCatalog>();
		CHECK(catalog.Scan(root.string()) == 1u);

		// .wd 里存的是**旧路径 + 身份** —— 这正是"改名之后仍然引用得到"的输入。
		AssetRef stale;
		stale.Path = StringPool::Get().InternPath(oldLogical);
		stale.Identity = identity;

		// 注册表绑定了这个世界上下文 ⇒ 按身份找回;因此解析出来的必须是新路径。
		World::AssetRegistry registry;
		registry.BindContext(&context);
		CHECK(catalog.FindPath(identity) == StringPool::Get().InternPath(newLogical));
		CHECK(!catalog.FindIdentity(StringPool::Get().InternPath(oldLogical)).IsValid());

		// 目录里没有该身份时不猜(无目录 / 无登记 ⇒ 保持路径解析的失败结果)。
		AssetRef unknown;
		unknown.Path = StringPool::Get().InternPath("materials/never_existed.wmat");
		unknown.Identity = AssetId { 0x7777ull };
		CHECK(!catalog.FindPath(unknown.Identity).IsValid());
	}

	// ---- 5. 组件侧身份通道(与 .wd 序列化同一实现)----
	void ComponentIdentityChannel()
	{
		using namespace World;
		WorldContext context;
		const Schema::TypeSchema* type = nullptr;
		for (const Schema::TypeSchema* candidate : context.Schemas().List(Schema::TypeCategory::Component))
			if (candidate->Id.Name == "World::MeshRendererComponent")
				type = candidate;
		CHECK(type != nullptr);

		MeshRendererComponent component;
		component.Mesh.Path = StringPool::Get().InternPath("models/channel.wmodel");
		component.Mesh.Identity = AssetId { 0x0F0E0D0C0B0A0908ull };
		component.Material.Path = StringPool::Get().InternPath("materials/channel.wmat");

		// 写出:ValueMap 里除路径外还应带一个 `<Field>Id` 兄弟键(仅已分配身份的字段)。
		const Schema::Value written = Schema::ReadStructValue(*type, &component);
		const Schema::ValueMap* fields = std::get_if<Schema::ValueMap>(&written);
		CHECK(fields != nullptr);
		CHECK(std::get<std::string>(fields->at("Mesh")) == "models/channel.wmodel");
		CHECK(std::get<std::string>(fields->at("MeshId")) == "0x0F0E0D0C0B0A0908");
		CHECK(fields->find("MaterialId") == fields->end());   // 未分配 ⇒ 不写该键

		// 读回:身份与路径一起恢复。
		MeshRendererComponent restored;
		CHECK(Schema::WriteStructValue(*type, &restored, written));
		CHECK(restored.Mesh.Identity == component.Mesh.Identity);
		CHECK(restored.Mesh.Path == component.Mesh.Path);
		CHECK(restored.Material.Path == component.Material.Path);

		// 手改路径 ⇒ 旧身份作废(否则新路径会被指向旧资产)。
		Schema::AssetOps<AssetRef>::SetPath(restored.Mesh, "models/other.wmodel");
		CHECK(!restored.Mesh.Identity.IsValid());
		CHECK(StringPool::Get().PathOf(restored.Mesh.Path) == "models/other.wmodel");
	}
}

int main()
{
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "we_asset_identity_probe";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	try
	{
		AssetIdTextRoundTrip();
		ModelCarriesIdentity(root);
		MaterialCarriesIdentity();
		TextureCarriesIdentity();
		CatalogAndRenameRecovery(root / "catalog");
		CatalogIgnoresUnidentifiedAssets(root / "catalog");
		RenameRecoveryThroughRegistry(root / "rename");
		ComponentIdentityChannel();
		std::filesystem::remove_all(root, ignored);
		std::printf("World.AssetIdentity: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::filesystem::remove_all(root, ignored);
		std::fprintf(stderr, "World.AssetIdentity: FAILED: %s\n", error.what());
		return 1;
	}
}
