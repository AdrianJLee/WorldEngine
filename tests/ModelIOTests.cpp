// P1b D5:模型导入 headless 回归。
//
// 覆盖:
//   1. 标准顶点布局契约(stride 32 的 position/normal/uv,.wmodel v2 依赖它);
//   2. .wmodel v2 往返:写→读全字段(含 meta)一致 + 确定性(两次序列化逐字节相同);
//   3. 坏 magic / 未知版本 / v1 / 截断 / 越界引用 / 缺 meta → 可读错误(硬失败,不"尽力解析");
//   4. 真实 glTF 夹具导入(2 primitive + 内嵌贴图 + 嵌套节点):节点树、submesh、
//      材质槽、贴图原样写出、缺法线按面法线补齐、Mesh::LoadWModel 读回校验 + 进程内缓存;
//   5. D5b:.wimport 设置 Load/Save/Hash;ImportAsBytes 内核 = ImportFile 的字节来源;
//      cook 从源-only 项目产出多产物(.wmodel/.wmat)、增量 0 changed、设置变化重烘、
//      scale/upAxis 烘焙、exportMaterials/exportTextures 开关、坏源 cook 非零失败;
//   6. 不支持特性(skin/动画)硬报错;
//   7. MeshRendererComponent.MeshIndex 的 schema 往返(SceneSerializer)+ 存根出现该字段。
#include "wldpch.h"

#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/GltfImporter.h"
#include "World/Core/Asset/ModelImportSettings.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Asset/WModelIO.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/ModelInstance.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/SceneSerializer.h"
#include "World/WUI/WuiJson.h"

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
		// D5b v2:meta 必须由导入器写入;手工构造的用例同样给一套可区分的值。
		data.Meta.Valid = true;
		data.Meta.SourceFingerprint = 0x1122334455667788ULL;
		data.Meta.ImporterVersion = 1;
		data.Meta.SettingsHash = 0x99AABBCCDDEEFF00ULL;
		data.Meta.UpAxis = 0;
		data.Meta.Scale = 1.0f;
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
		// D5b v2:meta 全字段往返(源指纹/导入器版本/设置哈希/轴/缩放)。
		CHECK(loaded.Meta.Valid);
		CHECK(loaded.Meta.SourceFingerprint == source.Meta.SourceFingerprint);
		CHECK(loaded.Meta.ImporterVersion == source.Meta.ImporterVersion);
		CHECK(loaded.Meta.SettingsHash == source.Meta.SettingsHash);
		CHECK(loaded.Meta.UpAxis == source.Meta.UpAxis);
		CHECK(Nearly(loaded.Meta.Scale, source.Meta.Scale));
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
		{
			// v1 拒绝:旧格式没有 meta(源指纹/设置哈希),必须报"请重新导入"而不是猜。
			std::vector<uint8_t> version1 = bytes;
			version1[4] = 1;   // version = 1(小端)
			version1[5] = 0;
			version1[6] = 0;
			version1[7] = 0;
			const std::string error = readFailure(version1, "version1.wmodel");
			CHECK(Contains(error, "version 1"));
			CHECK(Contains(error, "re-import"));
		}
		{
			// v2 契约:没有 meta 的 WModelData 不允许写出(导入器必须先填 meta)。
			Asset::WModelData noMeta = MakeSmallModel();
			noMeta.Meta = Asset::WModelData::MetaData {};
			std::string error;
			CHECK(!Asset::WModelIO::WriteFile((root / "no-meta.wmodel").string(), noMeta, &error));
			CHECK(Contains(error, "meta"));
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
		// D10:没给目的地(源在内容根外)时退回 models/ 作为目的地 —— 材质/贴图落在
		// 目的地的 materials//textures/ 子目录里,而不是内容根的同名目录。
		CHECK(result.MaterialPaths[0] == "models/materials/D5Fixture_Tex.wmat");
		CHECK(result.MaterialPaths[1] == "models/materials/D5Fixture_Solid.wmat");
		CHECK(result.TexturePaths.size() == 1u);
		CHECK(result.TexturePaths[0] == "models/textures/D5Fixture_0.png");
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

	// D5b-1 通用工具:测试自己的路径后缀替换(不依赖引擎内部实现)。
	std::filesystem::path WithSuffix(const std::filesystem::path& path, const char* suffix)
	{
		std::filesystem::path copy = path;
		copy.replace_extension(suffix);
		return copy;
	}

	uint64_t Fnv1a64(const std::vector<uint8_t>& bytes)
	{
		uint64_t hash = 14695981039346656037ULL;
		for (const uint8_t byte : bytes)
		{
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	// 5.D5b 导入设置:.wimport Load/Save/Hash(缺省不失败、坏 JSON 回默认、哈希覆盖字段)。
	void ModelImportSettingsRoundTrip()
	{
		const fs::path root = TestRoot() / "settings";
		fs::remove_all(root);
		fs::create_directories(root);

		// 缺文件 → Default() + 不报错。
		{
			std::string reason = "not cleared";
			const Asset::ModelImportSettings settings =
				Asset::ModelImportSettings::Load((root / "missing.gltf").string(), &reason);
			CHECK(reason.empty());
			CHECK(Nearly(settings.Scale, 1.0f));
			CHECK(settings.UpAxis == 0u);
			CHECK(settings.ExportMaterials);
			CHECK(settings.ExportTextures);
			CHECK(settings.ImportAnimations);
			CHECK(settings.GenerateNormals);
		}
		{
			// 坏 JSON → 默认值 + 可读 reason(不失败、不抛异常)。
			const fs::path source = root / "broken.gltf";
			WriteBytes(source, std::vector<uint8_t> { '{', '"', 's' });
			// .wimport 与源同目录同名 —— 测试自己写一份坏 JSON 版本。
			WriteBytes(WithSuffix(source, ".wimport"), std::vector<uint8_t> { '{', '"', 's' });
			std::string reason;
			const Asset::ModelImportSettings settings =
				Asset::ModelImportSettings::Load(source.string(), &reason);
			CHECK(!reason.empty());
			CHECK(Nearly(settings.Scale, 1.0f));
			CHECK(Asset::ModelImportSettings::Hash(settings) == Asset::ModelImportSettings::Hash(
				Asset::ModelImportSettings::Default()));
		}
		{
			// Save → Load 往返,且 Hash 覆盖每个字段。
			const fs::path source = root / "roundtrip.gltf";
			WriteBytes(source, std::vector<uint8_t> { ' ' });
			Asset::ModelImportSettings settings = Asset::ModelImportSettings::Default();
			settings.Scale = 2.0f;
			settings.UpAxis = 1;
			settings.ExportTextures = false;
			settings.GenerateNormals = true;   // 非默认值用例只覆盖 scale/upAxis/贴图开关
			std::string reason;
			CHECK(Asset::ModelImportSettings::Save(source.string(), settings, &reason));
			CHECK(reason.empty());
			const fs::path settingsPath = WithSuffix(source, ".wimport");
			CHECK(fs::exists(settingsPath));

			const Asset::ModelImportSettings loaded =
				Asset::ModelImportSettings::Load(source.string(), &reason);
			CHECK(reason.empty());
			CHECK(Nearly(loaded.Scale, 2.0f));
			CHECK(loaded.UpAxis == 1u);
			CHECK(loaded.ExportMaterials);
			CHECK(!loaded.ExportTextures);
			CHECK(loaded.GenerateNormals);
			CHECK(loaded.ImportAnimations);   // 默认 true,未被 Save 改动
			CHECK(Asset::ModelImportSettings::Hash(loaded) == Asset::ModelImportSettings::Hash(settings));

			Asset::ModelImportSettings changed = settings;
			changed.ExportTextures = true;
			CHECK(Asset::ModelImportSettings::Hash(changed) != Asset::ModelImportSettings::Hash(settings));
			changed = settings;
			changed.ImportAnimations = false;
			CHECK(Asset::ModelImportSettings::Hash(changed) != Asset::ModelImportSettings::Hash(settings));
			changed = settings;
			changed.UpAxis = 0;
			CHECK(Asset::ModelImportSettings::Hash(changed) != Asset::ModelImportSettings::Hash(settings));
		}
		fs::remove_all(root);
	}

	// 6.D5b 内核:ImportAsBytes = ImportFile 的字节来源(同源同设置 → 逐字节一致)+ meta 契约。
	void GltfImportKernelMatchesFileImport()
	{
		const fs::path root = TestRoot() / "kernel";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path fixture = fs::path(WLD_ASSETPATH) / "models" / "tests" / "D5Fixture.gltf";
		CHECK(fs::exists(fixture));
		// 该用例比较的是"内核字节"与"ImportFile 写盘字节":夹具 .wimport 与 Default()
		// 同值,两条路径的设置保持一致。

		Asset::GltfImportResult fileResult;
		std::string error;
		CHECK(Asset::GltfImporter::ImportFile(fixture.string(), root.string(), &fileResult, &error));
		CHECK(error.empty());
		const std::vector<uint8_t> sourceBytes = ReadBytes(fixture);
		CHECK(fileResult.SourceFingerprint == Fnv1a64(sourceBytes));
		CHECK(fileResult.UpAxis == 0u);

		const Asset::ModelImportSettings settings = Asset::ModelImportSettings::Default();
		Asset::GltfImportMetadata metadata;
		metadata.ImporterVersion = 1;
		metadata.SettingsHash = Asset::ModelImportSettings::Hash(settings);
		metadata.UpAxis = settings.UpAxis;
		metadata.Scale = settings.Scale;
		// v3 meta 内嵌源逻辑路径(ImportFile 按"源相对输出根"自动算)。内核调用必须
		// 传入同一个值,字节才能逐项一致 —— 直接从 ImportFile 的产物读出来复用,
		// 避免在测试里复制该路径规则;同时证明它确实被写进了盘上产物。
		{
			Asset::WModelData fileModel;
			CHECK(Asset::WModelIO::ReadFile((root / fileResult.WModelPath).string(), fileModel, &error));
			CHECK(error.empty());
			CHECK(fileModel.Meta.Valid);
			CHECK(!fileModel.Meta.SourcePath.empty());
			metadata.SourceLogicalPath = fileModel.Meta.SourcePath;
		}
		Asset::GltfImportBytesResult bytes;
		CHECK(Asset::GltfImporter::ImportAsBytes(fixture.string(), settings, metadata, &bytes, &error));
		CHECK(error.empty());
		CHECK(bytes.Summary.WModelPath == fileResult.WModelPath);
		CHECK(bytes.Summary.SourceFingerprint == fileResult.SourceFingerprint);
		CHECK(bytes.Summary.VertexCount == fileResult.VertexCount);
		CHECK(bytes.Summary.IndexCount == fileResult.IndexCount);
		CHECK(bytes.Metadata.SettingsHash == metadata.SettingsHash);
		CHECK(bytes.Metadata.ImporterVersion == 1u);
		// D10:没给目的地时退回 models/(源在内容根外);材质/贴图在它的子目录里。
		CHECK(fileResult.WModelPath == "models/D5Fixture.wmodel");
		CHECK(fileResult.MaterialPaths[0] == "models/materials/D5Fixture_Tex.wmat");

		// 内存产物 → 逐项与落盘字节一致;模型必须是最后一项(.wmodel 作为提交标记)。
		CHECK(!bytes.Outputs.empty());
		CHECK(bytes.Outputs.back().LogicalPath == fileResult.WModelPath);
		for (const Asset::GltfInMemoryOutput& output : bytes.Outputs)
			CHECK(ReadBytes(root / output.LogicalPath) == output.Data);

		// .wmodel meta:内核用源文件内容算指纹;解析回来与传入 metadata 一致。
		Asset::WModelData model;
		CHECK(Asset::WModelIO::ReadFile((root / fileResult.WModelPath).string(), model, &error));
		CHECK(model.Meta.Valid);
		CHECK(model.Meta.SourceFingerprint == fileResult.SourceFingerprint);
		CHECK(model.Meta.ImporterVersion == metadata.ImporterVersion);
		CHECK(model.Meta.SettingsHash == metadata.SettingsHash);
		CHECK(model.Meta.UpAxis == metadata.UpAxis);
		CHECK(Nearly(model.Meta.Scale, metadata.Scale));
		// ImportFile 的薄壳契约:字节与磁盘逐项一致(见上面的循环),路径集合也一致。
		CHECK(fileResult.MaterialPaths.size() == 2u);
		CHECK(fileResult.TexturePaths.size() == 1u);
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

	// D5b 工具:把真实夹具复制进临时内容根(源-only 项目:只有 .gltf + .wimport)。
	void CopyFixture(const fs::path& content)
	{
		const fs::path source = fs::path(WLD_ASSETPATH) / "models" / "tests" / "D5Fixture.gltf";
		CHECK(fs::exists(source));
		fs::create_directories(content / "models" / "tests");
		fs::copy_file(source, content / "models" / "tests" / "D5Fixture.gltf",
			fs::copy_options::overwrite_existing);
		const fs::path settings = WithSuffix(source, ".wimport");
		CHECK(fs::exists(settings));
		fs::copy_file(settings, content / "models" / "tests" / "D5Fixture.wimport",
			fs::copy_options::overwrite_existing);
	}

	Asset::CookSummary RunCook(const fs::path& content, const fs::path& project,
		const fs::path& output)
	{
		(void)content;   // 内容已在磁盘准备;这里只保证清单与内容根一致。
		std::string error;
		Asset::ProjectManifest manifest;
		manifest.Id = "com.test.model";
		manifest.ContentRoot = "content";
		manifest.StartScene = "models/tests/D5Fixture.gltf";
		manifest.Packages = { "packages/Base.wpak" };
		CHECK(Asset::ProjectManifest::Save(project / "project.we.yaml", manifest, &error));
		CHECK(error.empty());

		Asset::CookPipeline pipeline(Asset::DefaultImporters());
		Asset::CookSummary summary;
		pipeline.Cook(manifest, project / "project.we.yaml", output, false, &summary);
		return summary;
	}

	// 6.D5b cook:cook 期坏源 → 非零失败 + 可读错误(不写半成品)。
	void CookFailsOnBrokenSource()
	{
		const fs::path root = TestRoot() / "cook-broken";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path content = root / "content";
		fs::create_directories(content / "models");
		// 截断的 JSON:先被 cgltf 解析拒绝,报可读错误(不静默生成空模型)。
		WriteBytes(content / "models" / "Broken.gltf", std::vector<uint8_t> { '{', ' ', '"' });

		Asset::CookPipeline pipeline(Asset::DefaultImporters());
		Asset::CookSummary summary;
		Asset::ProjectManifest manifest;
		manifest.Id = "com.test.broken";
		manifest.ContentRoot = "content";
		manifest.StartScene = "models/Broken.gltf";
		manifest.Packages = { "packages/Base.wpak" };
		const std::vector<Asset::CookEntryResult> results =
			pipeline.Cook(manifest, root / "project.we.yaml", root / "cooked-output", false, &summary);
		CHECK(summary.Failed == 1u);
		CHECK(results.size() == 1u && results[0].Failed);
		CHECK(Contains(results[0].Error, "glTF import failed"));
		CHECK(!fs::exists(root / "cooked-output" / "cooked" / "models" / "Broken.wmodel"));
		fs::remove_all(root);
	}

	// 7.D5b cook:源-only 项目 → 多产物(.wmodel/.wmat/贴图)+ 增量 0 changed。
	// 夹具 `.wimport` 是**默认值**(与源同目录,验证 sidecar 解析路径;非默认路径见用例 8/9/10)。
	void CookModelSourceProducesArtifacts()
	{
		const fs::path root = TestRoot() / "cook";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path content = root / "content";
		CopyFixture(content);
		Asset::CookPipeline pipeline(Asset::DefaultImporters());
		Asset::CookSummary summary;
		Asset::ProjectManifest manifest;
		manifest.Id = "com.test.model";
		manifest.ContentRoot = "content";
		manifest.StartScene = "models/tests/D5Fixture.gltf";
		manifest.Packages = { "packages/Base.wpak" };
		const std::vector<Asset::CookEntryResult> results =
			pipeline.Cook(manifest, root / "project.we.yaml", root / "out", false, &summary);
		CHECK(summary.Total == 1u && summary.Changed == 1u && summary.Failed == 0u);
		CHECK(results.size() == 1u && !results[0].Failed);

		const fs::path cooked = root / "out" / "cooked";
		// D10(方案 A):产物落在**源所在目录**里(cook 的目的地 = 源的逻辑目录)。
		const fs::path modelFile = cooked / "models" / "tests" / "D5Fixture.wmodel";
		CHECK(fs::exists(modelFile));
		// 默认设置:材质与贴图都产出。
		CHECK(fs::exists(cooked / "models" / "tests" / "materials" / "D5Fixture_Tex.wmat"));
		CHECK(fs::exists(cooked / "models" / "tests" / "materials" / "D5Fixture_Solid.wmat"));
		CHECK(fs::exists(cooked / "models" / "tests" / "textures" / "D5Fixture_0.png"));

		Asset::WModelData model;
		std::string error;
		CHECK(Asset::WModelIO::ReadFile(modelFile.string(), model, &error));
		CHECK(error.empty());
		CHECK(model.Meta.Valid);
		CHECK(model.Meta.ImporterVersion == 1u);
		CHECK(model.Meta.Scale == 1.0f);
		CHECK(model.Meta.SourceFingerprint == Fnv1a64(ReadBytes(content / "models" / "tests" / "D5Fixture.gltf")));
		// 默认 scale=1:包围盒与源几何一致 [-1,0,0]~[1,1,1]。
		CHECK(Nearly(model.Bounds.Min.x, -1.0f) && Nearly(model.Bounds.Min.y, 0.0f)
			&& Nearly(model.Bounds.Min.z, 0.0f));
		CHECK(Nearly(model.Bounds.Max.x, 1.0f) && Nearly(model.Bounds.Max.y, 1.0f)
			&& Nearly(model.Bounds.Max.z, 1.0f));
		CHECK(model.MaterialSlots.size() == 2u);
		CHECK(model.MaterialSlots[0] == "models/tests/materials/D5Fixture_Tex.wmat");
		// v3 meta 记录源逻辑路径(编辑器"需要重导"判断的依据)。
		CHECK(model.Meta.SourcePath == "models/tests/D5Fixture.gltf");
		// 贴图路径指向导出的贴图(默认 exportTextures=true)。
		{
			MaterialDesc desc;
			std::string parseError;
			const MaterialLoadResult parsed =
				MaterialIO::Parse(ReadText(cooked / "models" / "tests" / "materials" / "D5Fixture_Tex.wmat"),
					desc, &parseError);
			CHECK(parsed.Success);
			CHECK(desc.AlbedoTexture == "models/tests/textures/D5Fixture_0.png");
			CHECK(desc.NormalTexture.empty());
		}

		// 增量:同一输入第二次 cook 必须 0 changed。
		Asset::CookPipeline second(Asset::DefaultImporters());
		Asset::CookSummary secondSummary;
		second.Cook(manifest, root / "project.we.yaml", root / "out", false, &secondSummary);
		CHECK(secondSummary.Changed == 0u && secondSummary.Skipped == 1u && secondSummary.Failed == 0u);
		fs::remove_all(root);
	}

	// 8.D5b cook:改 .wimport → 重烘;scale/upAxis 烘焙改包围盒轴映射+尺寸。
	void CookReactsToImportSettings()
	{
		const fs::path root = TestRoot() / "cook-settings";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path content = root / "content";
		CopyFixture(content);
		const fs::path output = root / "out";
		const fs::path modelFile = output / "cooked" / "models" / "tests" / "D5Fixture.wmodel";
		const fs::path settingsPath = content / "models" / "tests" / "D5Fixture.wimport";

		const Asset::CookSummary first =
			RunCook(content, root, output);
		CHECK(first.Changed == 1u && first.Failed == 0u);

		std::string error;
		Wui::JsonValue rootSettings;
		rootSettings.type = Wui::JsonValue::Type::Object;
		rootSettings.Object.push_back({ "scale", Wui::JsonValue::MakeNumber(2.0) });
		rootSettings.Object.push_back({ "upAxis", Wui::JsonValue::MakeString("Z") });
		rootSettings.Object.push_back({ "exportTextures", Wui::JsonValue::MakeBool(false) });
		{
			std::ofstream file(settingsPath, std::ios::binary | std::ios::trunc);
			file << rootSettings.Dump();
		}

		const Asset::CookSummary second =
			RunCook(content, root, output);
		CHECK(second.Changed == 1u && second.Failed == 0u);
		Asset::WModelData model;
		CHECK(Asset::WModelIO::ReadFile(modelFile.string(), model, &error));
		CHECK(error.empty());
		CHECK(model.Meta.UpAxis == 1u);
		CHECK(model.Meta.Scale == 2.0f);
		// upAxis=Z:-90° 绕 X 轴,(x,y,z) → (x,z,-y);源 [-1,0,0]~[1,1,1] → [-2,0,-2]~[2,2,0]。
		CHECK(Nearly(model.Bounds.Min.x, -2.0f) && Nearly(model.Bounds.Min.y, 0.0f)
			&& Nearly(model.Bounds.Min.z, -2.0f));
		CHECK(Nearly(model.Bounds.Max.x, 2.0f) && Nearly(model.Bounds.Max.y, 2.0f)
			&& Nearly(model.Bounds.Max.z, 0.0f));

		// 未改设置 → 0 changed(设置哈希参与复合指纹,值相同不重烘)。
		const Asset::CookSummary third =
			RunCook(content, root, output);
		CHECK(third.Changed == 0u && third.Skipped == 1u && third.Failed == 0u);
		fs::remove_all(root);
	}

	// 9.D5b cook:exportMaterials=false → 不产出 .wmat、材质槽留空。
	void CookWithoutMaterials()
	{
		const fs::path root = TestRoot() / "cook-nomat";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path content = root / "content";
		CopyFixture(content);
		Wui::JsonValue settings;
		settings.type = Wui::JsonValue::Type::Object;
		settings.Object.push_back({ "scale", Wui::JsonValue::MakeNumber(1.0) });
		settings.Object.push_back({ "upAxis", Wui::JsonValue::MakeString("Y") });
		settings.Object.push_back({ "exportMaterials", Wui::JsonValue::MakeBool(false) });
		settings.Object.push_back({ "exportTextures", Wui::JsonValue::MakeBool(true) });
		{
			std::ofstream file(content / "models" / "tests" / "D5Fixture.wimport",
				std::ios::binary | std::ios::trunc);
			file << settings.Dump();
		}

		const fs::path output = root / "out";
		const Asset::CookSummary summary =
			RunCook(content, root, output);
		CHECK(summary.Changed == 1u && summary.Failed == 0u);
		const fs::path cooked = output / "cooked";
		// 产物布局与默认设置一致:模型固定 models/<stem>.wmodel,材质/贴图平铺在各自目录。
		CHECK(fs::exists(cooked / "models" / "tests" / "D5Fixture.wmodel"));
		CHECK(!fs::exists(cooked / "models" / "tests" / "materials" / "D5Fixture_Tex.wmat"));
		Asset::WModelData model;
		std::string error;
		CHECK(Asset::WModelIO::ReadFile((cooked / "models" / "tests" / "D5Fixture.wmodel").string(),
			model, &error));
		CHECK(model.Meta.SourcePath == "models/tests/D5Fixture.gltf");
		CHECK(model.MaterialSlots.size() == 2u);
		CHECK(model.MaterialSlots[0].empty() && model.MaterialSlots[1].empty());
		for (const Asset::WModelSubmesh& submesh : model.Submeshes)
			CHECK(submesh.MaterialSlot == -1);
		fs::remove_all(root);
	}

	// 11.D10:导入落点 = 调用方指定的目的地目录;同内容材质/贴图复用(不再每个模型一份副本)。
	void DestinationLayoutAndReuse()
	{
		const fs::path root = TestRoot() / "destination";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path fixture = fs::path(WLD_ASSETPATH) / "models" / "tests" / "D5Fixture.gltf";
		CHECK(fs::exists(fixture));

		// ① 目的地目录:产物必须落在 models/props/(模型 + 它的 materials//textures/ 子目录)。
		Asset::GltfImportResult first;
		std::string error;
		CHECK(Asset::GltfImporter::ImportFile(fixture.string(), root.string(), &first, &error,
			"models/props"));
		CHECK(error.empty());
		CHECK(first.WModelPath == "models/props/D5Fixture.wmodel");
		CHECK(fs::exists(root / first.WModelPath));
		CHECK(first.MaterialPaths[0] == "models/props/materials/D5Fixture_Tex.wmat");
		CHECK(first.TexturePaths[0] == "models/props/textures/D5Fixture_0.png");
		const size_t materialCountAfterFirst = static_cast<size_t>(
			std::distance(fs::directory_iterator(root / "models" / "props" / "materials"),
				fs::directory_iterator()));

		// ② 同源同目的地再导一次:同内容材质/贴图**复用同一批文件**(D10 去重)。
		Asset::GltfImportResult second;
		CHECK(Asset::GltfImporter::ImportFile(fixture.string(), root.string(), &second, &error,
			"models/props"));
		CHECK(error.empty());
		CHECK(second.MaterialPaths == first.MaterialPaths);
		CHECK(second.TexturePaths == first.TexturePaths);
		const size_t materialCountAfterSecond = static_cast<size_t>(
			std::distance(fs::directory_iterator(root / "models" / "props" / "materials"),
				fs::directory_iterator()));
		CHECK(materialCountAfterSecond == materialCountAfterFirst);
		fs::remove_all(root);
	}

	// 10.D5b:派生产物在内存产物里可枚举(材质/贴图在前、模型最后);cook.db 记录源、
	// 不把 .wimport 当资产;.wimport 变化触发重烘(复合指纹,见用例 8)。
	void CookDatabaseTracksSettingsDependency()
	{
		const fs::path root = TestRoot() / "cook-db";
		fs::remove_all(root);
		fs::create_directories(root);
		const fs::path content = root / "content";
		CopyFixture(content);
		const fs::path output = root / "out";
		RunCook(content, root, output);

		// 内存产物枚举:贴图/材质在前,模型最后(.wmodel 作为提交标记)。
		{
			Asset::GltfImportBytesResult bytes;
			std::string importError;
			Asset::GltfImportMetadata metadata;
			metadata.ImporterVersion = 1;
			metadata.SettingsHash = Asset::ModelImportSettings::Hash(Asset::ModelImportSettings::Default());
			// 与 cook 的 ModelImporter 相同(D10 方案 A):产物落在**源所在目录**
			// (models/tests/ + 它的 materials//textures/ 子目录),
			// 源逻辑路径由 SourceLogicalPath 记录进 meta(同时决定目的地)。
			metadata.LogicalModelPath.clear();
			metadata.SourceLogicalPath = "models/tests/D5Fixture.gltf";
			CHECK(Asset::GltfImporter::ImportAsBytes(
				(content / "models" / "tests" / "D5Fixture.gltf").string(),
				Asset::ModelImportSettings::Default(), metadata, &bytes, &importError));
			CHECK(importError.empty());
			for (size_t index = 0; index + 1 < bytes.Outputs.size(); ++index)
			{
				const std::string& path = bytes.Outputs[index].LogicalPath;
				CHECK(path.rfind("models/tests/textures/", 0) == 0
					|| path.rfind("models/tests/materials/", 0) == 0);
			}
			CHECK(bytes.Outputs.back().LogicalPath == "models/tests/D5Fixture.wmodel");
		}

		std::string parseError;
		const std::optional<Wui::JsonValue> database =
			Wui::JsonValue::Parse(ReadText(output / "cook.db.json"), &parseError);
		CHECK(database.has_value());
		const Wui::JsonValue* entries = database->Find("entries");
		CHECK(entries != nullptr && entries->type == Wui::JsonValue::Type::Array);
		bool hasSource = false;
		for (const Wui::JsonValue& entry : entries->Array)
		{
			const Wui::JsonValue* path = entry.Find("path");
			CHECK(path != nullptr);
			if (path->AsString() == "models/tests/D5Fixture.gltf")
				hasSource = true;
			CHECK(path->AsString() != "models/tests/D5Fixture.wimport");   // 设置文件不是资产
			CHECK(path->AsString() != "models/tests/D5Fixture.wmodel");    // 派生产物不进内容根清单
		}
		CHECK(hasSource);
		fs::remove_all(root);
	}

	// 11.不支持特性硬报错(不生成半成品)。
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
		// 每次运行从干净沙箱开始(失败时留下的半成品不参与下一次断言)。
		std::error_code cleanupEc;
		std::filesystem::remove_all(TestRoot(), cleanupEc);

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
			{ "D5b import settings .wimport round trip + hash covers fields", ModelImportSettingsRoundTrip },
			{ "D5b glTF kernel bytes match ImportFile output + v2 meta contract", GltfImportKernelMatchesFileImport },
			{ "model node tree instantiates as entities (TRS / MeshIndex / hierarchy)", ModelInstanceBuildsNodeTree },
			{ "D5b cook fails on broken source with a readable error", CookFailsOnBrokenSource },
			{ "D5b cook produces .wmodel/.wmat from source-only project + incremental 0 changed", CookModelSourceProducesArtifacts },
			{ "D5b cook reacts to .wimport scale/upAxis changes with baked geometry", CookReactsToImportSettings },
			{ "D5b cook without exported materials leaves empty slots", CookWithoutMaterials },
			{ "D5b derived outputs are enumerable and ordered (model last)", CookDatabaseTracksSettingsDependency },
			{ "D10 destination folder layout + content-hash material/texture reuse", DestinationLayoutAndReuse },
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
