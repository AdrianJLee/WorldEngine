// P1b D5:模型导入 headless 回归。
//
// 覆盖:
//   1. 标准顶点布局契约(stride 32 的 position/normal/uv,.wmodel v1 依赖它);
//   2. .wmodel 往返:写→读全字段一致 + 确定性(两次序列化逐字节相同);
//   3. 坏 magic / 未知版本 / 截断 / 越界引用 → 可读错误(硬失败,不"尽力解析");
//   4. 真实 glTF 夹具导入(2 primitive + 内嵌贴图 + 嵌套节点):节点树、submesh、
//      材质槽、贴图原样写出、缺法线按面法线补齐、Mesh::LoadWModel 读回校验 + 进程内缓存;
//   5. 不支持特性(skin/动画)硬报错;
//   6. MeshRendererComponent.MeshIndex 的 schema 往返(SceneSerializer)+ 存根出现该字段。
#include "wldpch.h"

#include "World/Core/Asset/GltfImporter.h"
#include "World/Core/Asset/WModelIO.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/ModelInstance.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;
	namespace fs = std::filesystem;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Nearly(float left, float right, float epsilon = 1e-5f)
	{
		return std::fabs(left - right) <= epsilon;
	}

	bool NearlyVec3(const glm::vec3& left, const glm::vec3& right, float epsilon = 1e-5f)
	{
		return Nearly(left.x, right.x, epsilon) && Nearly(left.y, right.y, epsilon)
			&& Nearly(left.z, right.z, epsilon);
	}

	std::vector<uint8_t> ReadBytes(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	std::string ReadText(const fs::path& path)
	{
		const std::vector<uint8_t> bytes = ReadBytes(path);
		return std::string(bytes.begin(), bytes.end());
	}

	void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
	{
		if (!path.parent_path().empty())
			fs::create_directories(path.parent_path());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	bool Contains(const std::string& text, const char* needle)
	{
		return text.find(needle) != std::string::npos;
	}

	const Schema::FieldSchema* FindField(const Schema::TypeSchema& schema, const char* name)
	{
		for (const Schema::FieldSchema& field : schema.Fields)
			if (field.Name == name)
				return &field;
		return nullptr;
	}

	fs::path TestRoot()
	{
		return fs::temp_directory_path() / "we-model-tests";
	}

	// 手工构造 .wmodel 数据(往返/坏文件用例的输入)。
	Asset::WModelData MakeSmallModel()
	{
		Asset::WModelData data;
		data.Vertices = {
			{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
			{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
			{ { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
			{ { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
		};
		data.Indices = { 0, 1, 2, 0, 2, 3 };
		data.Bounds = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
		Asset::WModelSubmesh first;
		first.IndexOffset = 0;
		first.IndexCount = 3;
		first.MaterialSlot = 0;
		first.Bounds = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
		Asset::WModelSubmesh second;
		second.IndexOffset = 3;
		second.IndexCount = 3;
		second.MaterialSlot = 1;
		second.Bounds = { { 0.0f, 0.5f, 0.0f }, { 0.5f, 1.0f, 0.0f } };
		data.Submeshes = { first, second };
		data.Meshes = { { 0, 2 } };
		Asset::WModelNode root;
		root.Name = "Root";
		root.Translation = { 1.0f, 2.0f, 3.0f };
		root.Rotation = glm::quat(0.70710678f, 0.0f, 0.70710678f, 0.0f);
		root.Scale = { 2.0f, 2.0f, 2.0f };
		Asset::WModelNode child;
		child.Parent = 0;
		child.MeshIndex = 0;
		child.Name = "Child";
		child.Translation = { 0.0f, 1.0f, 0.0f };
		data.Nodes = { root, child };
		data.MaterialSlots = { "materials/unit_a.wmat", "materials/unit_b.wmat" };
		return data;
	}

	// 1.D5 前置:标准顶点布局仍是 position/normal/uv(stride 32),.wmodel v1 依赖它。
	void StandardLayoutIsStable()
	{
		const MeshVertexLayout layout = Mesh::MakeStandardLayout();
		CHECK(layout.GetStride() == 32u);
		CHECK(layout.Attributes.size() == 3u);
		CHECK(sizeof(Asset::WModelVertex) == 32u);
	}

	// 2..wmodel 往返:写→读全字段一致;两次序列化逐字节相同(确定性)。
	void WModelRoundTripIsDeterministic()
	{
		const fs::path root = TestRoot() / "roundtrip";
		fs::remove_all(root);
		fs::create_directories(root);

		const Asset::WModelData source = MakeSmallModel();
		const std::vector<uint8_t> first = Asset::WModelIO::Serialize(source);
		const std::vector<uint8_t> second = Asset::WModelIO::Serialize(source);
		CHECK(first == second);

		const fs::path file = root / "roundtrip.wmodel";
		std::string error = "not cleared";
		CHECK(Asset::WModelIO::WriteFile(file.string(), source, &error));
		CHECK(error.empty());
		CHECK(ReadBytes(file) == first);

		Asset::WModelData loaded;
		CHECK(Asset::WModelIO::ReadFile(file.string(), loaded, &error));
		CHECK(error.empty());
		CHECK(loaded.Vertices.size() == source.Vertices.size());
		CHECK(loaded.Indices == source.Indices);
		for (size_t index = 0; index < loaded.Vertices.size(); ++index)
			CHECK(std::memcmp(&loaded.Vertices[index], &source.Vertices[index],
				sizeof(Asset::WModelVertex)) == 0);
		CHECK(NearlyVec3(loaded.Bounds.Min, source.Bounds.Min));
		CHECK(NearlyVec3(loaded.Bounds.Max, source.Bounds.Max));
		CHECK(loaded.Submeshes.size() == source.Submeshes.size());
		for (size_t index = 0; index < loaded.Submeshes.size(); ++index)
		{
			CHECK(loaded.Submeshes[index].IndexOffset == source.Submeshes[index].IndexOffset);
			CHECK(loaded.Submeshes[index].IndexCount == source.Submeshes[index].IndexCount);
			CHECK(loaded.Submeshes[index].MaterialSlot == source.Submeshes[index].MaterialSlot);
			CHECK(NearlyVec3(loaded.Submeshes[index].Bounds.Min, source.Submeshes[index].Bounds.Min));
			CHECK(NearlyVec3(loaded.Submeshes[index].Bounds.Max, source.Submeshes[index].Bounds.Max));
		}
		CHECK(loaded.Meshes.size() == source.Meshes.size());
		for (size_t index = 0; index < loaded.Meshes.size(); ++index)
		{
			CHECK(loaded.Meshes[index].FirstSubmesh == source.Meshes[index].FirstSubmesh);
			CHECK(loaded.Meshes[index].SubmeshCount == source.Meshes[index].SubmeshCount);
		}
		CHECK(loaded.Nodes.size() == source.Nodes.size());
		for (size_t index = 0; index < loaded.Nodes.size(); ++index)
		{
			const Asset::WModelNode& left = loaded.Nodes[index];
			const Asset::WModelNode& right = source.Nodes[index];
			CHECK(left.Parent == right.Parent);
			CHECK(left.MeshIndex == right.MeshIndex);
			CHECK(NearlyVec3(left.Translation, right.Translation));
			CHECK(Nearly(left.Rotation.x, right.Rotation.x));
			CHECK(Nearly(left.Rotation.y, right.Rotation.y));
			CHECK(Nearly(left.Rotation.z, right.Rotation.z));
			CHECK(Nearly(left.Rotation.w, right.Rotation.w));
			CHECK(NearlyVec3(left.Scale, right.Scale));
			CHECK(left.Name == right.Name);
		}
		CHECK(loaded.MaterialSlots == source.MaterialSlots);
		// 读回再写出必须与首次写出逐字节一致(确定性覆盖整条往返)。
		CHECK(Asset::WModelIO::Serialize(loaded) == first);
		fs::remove_all(root);
	}

	// 3.坏 magic / 未知版本 / 截断 / 越界引用各自给可读错误。
	void WModelRejectsCorruptFiles()
	{
		const fs::path root = TestRoot() / "corrupt";
		fs::remove_all(root);
		fs::create_directories(root);

		const Asset::WModelData source = MakeSmallModel();
		const std::vector<uint8_t> bytes = Asset::WModelIO::Serialize(source);
		CHECK(bytes.size() > 64u);

		const auto readFailure = [&root](const std::vector<uint8_t>& content, const char* name)
		{
			const fs::path file = root / name;
			WriteBytes(file, content);
			Asset::WModelData loaded;
			std::string error;
			const bool ok = Asset::WModelIO::ReadFile(file.string(), loaded, &error);
			CHECK(!ok);
			CHECK(!error.empty());
			return error;
		};

		{
			std::vector<uint8_t> corrupt = bytes;
			corrupt[0] = 'X';
			CHECK(Contains(readFailure(corrupt, "bad-magic.wmodel"), "magic"));
		}
		{
			std::vector<uint8_t> corrupt = bytes;
			corrupt[4] = 99;   // version = 99(小端)
			corrupt[5] = 0;
			corrupt[6] = 0;
			corrupt[7] = 0;
			CHECK(Contains(readFailure(corrupt, "bad-version.wmodel"), "version"));
		}
		{
			std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
			CHECK(Contains(readFailure(truncated, "truncated.wmodel"), "truncated"));
		}
		{
			// 越界引用:写出前自校验必须拒绝(否则错误会推迟到运行期加载)。
			Asset::WModelData invalid = MakeSmallModel();
			invalid.Submeshes[0].IndexCount = 999;
			std::string error;
			CHECK(!Asset::WModelIO::WriteFile((root / "invalid.wmodel").string(), invalid, &error));
			CHECK(Contains(error, "submesh"));
		}
		fs::remove_all(root);
	}

	// 4.真实 glTF 夹具导入 + Mesh::LoadWModel 读回(fixture 见 Game/assets/models/tests/D5Fixture.gltf)。
	void GltfFixtureImportsAndLoadsBack()
	{
		const fs::path root = TestRoot() / "import";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path fixture = fs::path(WLD_ASSETPATH) / "models" / "tests" / "D5Fixture.gltf";
		CHECK(fs::exists(fixture));

		Asset::GltfImportResult result;
		std::string error;
		CHECK(Asset::GltfImporter::ImportFile(fixture.string(), root.string(), &result, &error));
		CHECK(error.empty());
		CHECK(result.WModelPath == "models/D5Fixture.wmodel");
		CHECK(result.MeshCount == 1u);
		CHECK(result.SubmeshCount == 2u);
		CHECK(result.NodeCount == 3u);
		CHECK(result.MaterialSlotCount == 2u);
		CHECK(result.VertexCount == 7u);
		CHECK(result.IndexCount == 9u);
		CHECK(result.MaterialPaths.size() == 2u);
		CHECK(result.MaterialPaths[0] == "materials/D5Fixture_Tex.wmat");
		CHECK(result.MaterialPaths[1] == "materials/D5Fixture_Solid.wmat");
		CHECK(result.TexturePaths.size() == 1u);
		CHECK(result.TexturePaths[0] == "textures/D5Fixture_0.png");
		// 唯一允许的降级:primitive 1 没有 NORMAL → 面法线补齐 + 一条 warning。
		bool hasNormalWarning = false;
		for (const std::string& warning : result.Warnings)
			if (Contains(warning, "NORMAL"))
				hasNormalWarning = true;
		CHECK(hasNormalWarning);

		// 内嵌贴图原样字节写出(PNG 签名 + 夹具里 bufferView 的 69 字节)。
		const fs::path textureFile = root / result.TexturePaths[0];
		CHECK(fs::exists(textureFile));
		const std::vector<uint8_t> texture = ReadBytes(textureFile);
		CHECK(texture.size() == 69u);
		const uint8_t pngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		CHECK(std::memcmp(texture.data(), pngSignature, sizeof(pngSignature)) == 0);

		// 材质:字段填进现有 MaterialDesc;颜色从 glTF 线性值转成引擎的 sRGB 约定。
		{
			MaterialDesc desc;
			std::string parseError;
			const MaterialLoadResult parsed = MaterialIO::Parse(ReadText(root / result.MaterialPaths[0]), desc, &parseError);
			CHECK(parsed.Success);
			CHECK(desc.Name == "Tex");
			CHECK(desc.AlbedoTexture == result.TexturePaths[0]);
			CHECK(Nearly(desc.Roughness, 0.8f));
			CHECK(desc.BlendMode == MaterialBlendMode::Opaque);
			CHECK(!desc.DoubleSided);
		}
		{
			MaterialDesc desc;
			std::string parseError;
			const MaterialLoadResult parsed = MaterialIO::Parse(ReadText(root / result.MaterialPaths[1]), desc, &parseError);
			CHECK(parsed.Success);
			CHECK(desc.Name == "Solid");
			CHECK(Nearly(desc.Metallic, 0.2f));
			CHECK(Nearly(desc.Roughness, 0.6f));
			const float expected = std::pow(0.5f, 1.0f / 2.2f);
			CHECK(Nearly(desc.BaseColor.g, expected, 1e-4f));
		}

		// Mesh::LoadWModel:包围盒/子网格/节点树/材质槽全字段,并命中进程内缓存。
		const std::string wmodelPath = (root / result.WModelPath).string();
		Ref<Mesh> mesh = Mesh::LoadWModel(wmodelPath, &error);
		CHECK(mesh != nullptr);
		CHECK(error.empty());
		CHECK(mesh->GetVertexCount() == 7u);
		CHECK(mesh->GetIndexCount() == 9u);
		CHECK(NearlyVec3(mesh->GetBounds().Min, glm::vec3(-1.0f, 0.0f, 0.0f)));
		CHECK(NearlyVec3(mesh->GetBounds().Max, glm::vec3(1.0f, 1.0f, 1.0f)));
		CHECK(mesh->HasSubmeshes());
		const std::vector<MeshSubmesh>& submeshes = mesh->GetSubmeshes();
		CHECK(submeshes.size() == 2u);
		CHECK(submeshes[0].IndexOffset == 0u && submeshes[0].IndexCount == 3u && submeshes[0].MaterialSlot == 0);
		CHECK(NearlyVec3(submeshes[0].Bounds.Min, glm::vec3(-1.0f, 0.0f, 0.0f)));
		CHECK(NearlyVec3(submeshes[0].Bounds.Max, glm::vec3(1.0f, 1.0f, 0.0f)));
		CHECK(submeshes[1].IndexOffset == 3u && submeshes[1].IndexCount == 6u && submeshes[1].MaterialSlot == 1);
		CHECK(NearlyVec3(submeshes[1].Bounds.Min, glm::vec3(-1.0f, 0.0f, 1.0f)));
		CHECK(NearlyVec3(submeshes[1].Bounds.Max, glm::vec3(1.0f, 1.0f, 1.0f)));
		CHECK(mesh->GetMeshes().size() == 1u);
		CHECK(mesh->GetMeshes()[0].FirstSubmesh == 0u && mesh->GetMeshes()[0].SubmeshCount == 2u);
		CHECK(mesh->GetMaterialSlots() == result.MaterialPaths);
		CHECK(mesh->GetNodes().size() == 3u);
		CHECK(mesh->GetNodes()[0].Parent == -1 && mesh->GetNodes()[0].MeshIndex == -1);
		CHECK(mesh->GetNodes()[0].Name == "Root");
		CHECK(NearlyVec3(mesh->GetNodes()[0].Translation, glm::vec3(1.0f, 2.0f, 3.0f)));
		CHECK(mesh->GetNodes()[1].Parent == 0 && mesh->GetNodes()[1].MeshIndex == 0);
		CHECK(mesh->GetNodes()[1].Name == "Child");
		CHECK(Nearly(mesh->GetNodes()[1].Rotation.y, 0.70710678f, 1e-4f));
		CHECK(Nearly(mesh->GetNodes()[1].Rotation.w, 0.70710678f, 1e-4f));
		CHECK(mesh->GetNodes()[2].Parent == 1 && mesh->GetNodes()[2].MeshIndex == 0);
		CHECK(NearlyVec3(mesh->GetNodes()[2].Scale, glm::vec3(0.5f)));
		// 顶点:primitive 0 的 NORMAL 原样保留;primitive 1 缺法线 → 面法线 = +Z。
		struct FixtureVertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 TexCoord;
		};
		const MeshDesc& desc = mesh->GetDesc();
		CHECK(desc.VertexData.size() == sizeof(FixtureVertex) * 7u);
		const auto* vertices = reinterpret_cast<const FixtureVertex*>(desc.VertexData.data());
		CHECK(NearlyVec3(vertices[0].Position, glm::vec3(-1.0f, 0.0f, 0.0f)));
		CHECK(NearlyVec3(vertices[0].Normal, glm::vec3(0.0f, 0.0f, 1.0f)));
		for (size_t index = 3; index < 7; ++index)
			CHECK(NearlyVec3(vertices[index].Normal, glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f));
		// 进程内缓存:同一路径 → 同一个 Ref<Mesh>。
		CHECK(Mesh::LoadWModel(wmodelPath) == mesh);
		fs::remove_all(root);
	}

	// 4b.节点树实例化:3 个节点 → 3 个实体,Tag/TRS/MeshPath/MeshIndex/层级与世界矩阵正确。
	void ModelInstanceBuildsNodeTree()
	{
		static WorldContext context;
		const fs::path root = TestRoot() / "instance";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path fixture = fs::path(WLD_ASSETPATH) / "models" / "tests" / "D5Fixture.gltf";
		Asset::GltfImportResult imported;
		std::string error;
		// 顺带覆盖 filesystem::path 的免费函数入口(编辑器 --import-gltf 用同一个)。
		CHECK(Asset::ImportFile(fixture, root, &imported, &error));
		const std::string wmodelPath = (root / imported.WModelPath).string();

		auto scene = CreateRef<Scene>(context);
		const uint32_t created = Gameplay::InstantiateModel(wmodelPath, *scene, entt::null, &error);
		CHECK(created == 3u);
		entt::registry& registry = scene->GetRegistry();
		entt::entity rootEntity = entt::null;
		entt::entity childEntity = entt::null;
		entt::entity grandchildEntity = entt::null;
		for (const entt::entity handle : registry.view<TagComponent>())
		{
			const std::string& name = registry.get<TagComponent>(handle).Tag;
			if (name == "Root") rootEntity = handle;
			else if (name == "Child") childEntity = handle;
			else if (name == "Grandchild") grandchildEntity = handle;
		}
		CHECK(rootEntity != entt::null && childEntity != entt::null && grandchildEntity != entt::null);
		CHECK(!registry.all_of<MeshRendererComponent>(rootEntity));
		const MeshRendererComponent& childRenderer = registry.get<MeshRendererComponent>(childEntity);
		CHECK(childRenderer.MeshPath == wmodelPath);
		CHECK(childRenderer.MeshIndex == 0);
		CHECK(registry.get<MeshRendererComponent>(grandchildEntity).MeshIndex == 0);
		CHECK(registry.get<HierarchyComponent>(rootEntity).Parent == entt::null);
		CHECK(registry.get<HierarchyComponent>(childEntity).Parent == rootEntity);
		CHECK(registry.get<HierarchyComponent>(grandchildEntity).Parent == childEntity);
		CHECK(NearlyVec3(registry.get<TransformComponent>(childEntity).Location, glm::vec3(0.0f, 1.0f, 0.0f)));
		CHECK(NearlyVec3(registry.get<TransformComponent>(grandchildEntity).Scale, glm::vec3(0.5f)));
		// 世界矩阵:Root(1,2,3) + Child(0,1,0) = (1,3,3)。
		const glm::mat4& world = registry.get<WorldTransformComponent>(grandchildEntity).Matrix;
		CHECK(Nearly(world[3].x, 1.0f, 1e-4f));
		CHECK(Nearly(world[3].y, 3.0f, 1e-4f));
		CHECK(Nearly(world[3].z, 3.0f, 1e-4f));
		fs::remove_all(root);
	}

	// 5.不支持特性硬报错(不生成半成品)。
	void UnsupportedFeaturesHardFail()
	{
		const fs::path root = TestRoot() / "unsupported";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path skinPath = root / "skinned.gltf";
		const std::string skinJson =
			"{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Joint\"}],"
			"\"skins\":[{\"joints\":[0]}],"
			"\"meshes\":[{\"name\":\"M\",\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
			"\"accessors\":[{\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}],"
			"\"buffers\":[{\"byteLength\":36,\"uri\":\"missing.bin\"}]}";
		{
			std::ofstream file(skinPath, std::ios::binary | std::ios::trunc);
			file << skinJson;
		}
		Asset::GltfImportResult result;
		std::string error;
		CHECK(!Asset::GltfImporter::ImportFile(skinPath.string(), root.string(), &result, &error));
		CHECK(Contains(error, "skin"));

		const fs::path animationPath = root / "animated.gltf";
		const std::string animationJson =
			"{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
			"\"scenes\":[{\"nodes\":[0]}],"
			"\"nodes\":[{\"name\":\"Root\"}],"
			"\"animations\":[{\"name\":\"Clip\",\"channels\":[],\"samplers\":[]}],"
			"\"meshes\":[],\"buffers\":[]}";
		{
			std::ofstream file(animationPath, std::ios::binary | std::ios::trunc);
			file << animationJson;
		}
		CHECK(!Asset::GltfImporter::ImportFile(animationPath.string(), root.string(), &result, &error));
		CHECK(Contains(error, "animation"));

		// 源文件不存在 → 可读失败(不抛异常)。
		CHECK(!Asset::GltfImporter::ImportFile((root / "missing.gltf").string(), root.string(), &result, &error));
		CHECK(!error.empty());
		fs::remove_all(root);
	}

	// 6.MeshIndex schema 往返 + 存根出现该字段。
	void MeshIndexSchemaRoundTrip()
	{
		static WorldContext context;
		const Schema::TypeSchema* schema = context.Schemas().Find("World::MeshRendererComponent");
		CHECK(schema != nullptr);
		const Schema::FieldSchema* field = FindField(*schema, "MeshIndex");
		CHECK(field != nullptr);
		CHECK(field->K == Schema::Kind::Int32);
		CHECK(std::holds_alternative<int32_t>(field->Default));
		CHECK(std::get<int32_t>(field->Default) == 0);

		MeshRendererComponent component;
		field->Set(&component, Schema::Value(int32_t(7)));
		CHECK(component.MeshIndex == 7);
		CHECK(std::get<int32_t>(field->Get(&component)) == 7);

		const fs::path root = TestRoot() / "schema";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path scenePath = root / "MeshIndexRoundTrip.wd";
		auto source = CreateRef<Scene>(context);
		{
			entt::registry& registry = source->GetRegistry();
			const entt::entity handle = registry.create();
			registry.emplace<UUIDComponent>(handle, UUID());
			registry.emplace<TagComponent>(handle, "Model");
			registry.emplace<TransformComponent>(handle);
			MeshRendererComponent renderer;
			renderer.MeshPath = "models/D5Fixture.wmodel";
			renderer.MeshIndex = 3;
			registry.emplace<MeshRendererComponent>(handle, renderer);
		}
		{
			SceneSerializer serializer(source);
			CHECK(serializer.Serialize(scenePath.string()));
		}
		auto loaded = CreateRef<Scene>(context);
		{
			SceneSerializer serializer(loaded);
			CHECK(serializer.Deserialize(scenePath.string()));
		}
		{
			entt::registry& registry = loaded->GetRegistry();
			const auto view = registry.view<MeshRendererComponent>();
			CHECK(view.size() == 1u);
			const MeshRendererComponent& restored = registry.get<MeshRendererComponent>(*view.begin());
			CHECK(restored.MeshIndex == 3);
			CHECK(restored.MeshPath == "models/D5Fixture.wmodel");
		}
		fs::remove_all(root);

		// 入库存根必须已经带上该字段(由编辑器启动路径重生成;World.ScriptWorkflow 是逐字节门禁,
		// 这里再给一条直接的可见断言)。
		const fs::path stub = fs::path(WLD_ASSETPATH) / "scripts" / "intermediate" / "WorldEngineAPI.luau";
		CHECK(fs::exists(stub));
		const std::string stubText = ReadText(stub);
		CHECK(Contains(stubText, "MeshRenderer"));
		CHECK(Contains(stubText, "MeshIndex"));
	}
}

int main(int argc, char** argv)
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);

		// 无窗口导入模式(与编辑器 --import-gltf 同一条引擎路径):
		// WorldModelTests --import <source.gltf> <outputRoot>
		if (argc == 4 && std::string(argv[1]) == "--import")
		{
			using namespace World;
			const std::string sourcePath = argv[2];
			const std::string outputRoot = argv[3];
			Asset::GltfImportResult result;
			std::string error;
			if (!Asset::GltfImporter::ImportFile(sourcePath, outputRoot, &result, &error))
			{
				std::fprintf(stderr, "[import] failed: %s\n", error.c_str());
				return 1;
			}
			std::printf("[import] source=%s output=%s\n", sourcePath.c_str(), outputRoot.c_str());
			std::printf("[import] model=%s vertices=%u indices=%u meshes=%u submeshes=%u nodes=%u materials=%u\n",
				result.WModelPath.c_str(), result.VertexCount, result.IndexCount, result.MeshCount,
				result.SubmeshCount, result.NodeCount, result.MaterialSlotCount);
			for (const std::string& material : result.MaterialPaths)
				std::printf("[import] material=%s\n", material.c_str());
			for (const std::string& texture : result.TexturePaths)
				std::printf("[import] texture=%s\n", texture.c_str());
			for (const std::string& warning : result.Warnings)
				std::printf("[import] warning=%s\n", warning.c_str());

			const std::string wmodelPath = (std::filesystem::path(outputRoot) / result.WModelPath).string();
			const Ref<Mesh> mesh = Mesh::LoadWModel(wmodelPath, &error);
			if (!mesh)
			{
				std::fprintf(stderr, "[import] readback failed: %s\n", error.c_str());
				return 1;
			}
			std::printf("[import] readback bounds min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f) "
				"vertices=%u indices=%u submeshes=%zu nodes=%zu\n",
				mesh->GetBounds().Min.x, mesh->GetBounds().Min.y, mesh->GetBounds().Min.z,
				mesh->GetBounds().Max.x, mesh->GetBounds().Max.y, mesh->GetBounds().Max.z,
				mesh->GetVertexCount(), mesh->GetIndexCount(),
				mesh->GetSubmeshes().size(), mesh->GetNodes().size());
			std::printf("[import] OK\n");
			return 0;
		}

		const std::pair<const char*, void(*)()> tests[] = {
			{ "standard vertex layout is stride-32 position/normal/uv", StandardLayoutIsStable },
			{ ".wmodel round trip is complete and deterministic", WModelRoundTripIsDeterministic },
			{ ".wmodel rejects bad magic / version / truncation / out-of-range refs", WModelRejectsCorruptFiles },
			{ "glTF fixture imports (2 submeshes, embedded texture, nested nodes, face normals)", GltfFixtureImportsAndLoadsBack },
			{ "model node tree instantiates as entities (TRS / MeshIndex / hierarchy)", ModelInstanceBuildsNodeTree },
			{ "unsupported glTF features fail hard (skin / animation)", UnsupportedFeaturesHardFail },
			{ "MeshIndex schema round trip + committed Lua stub", MeshIndexSchemaRoundTrip },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.Model: all checks passed\n");
		else
			std::fprintf(stderr, "World.Model: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Model: fatal: %s\n", error.what());
		return 1;
	}
}
