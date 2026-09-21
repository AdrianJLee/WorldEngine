#pragma once

#include "World/Core/Export.h"
#include "World/Core/Asset/ModelImportSettings.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace World::Asset
{
	// .wmodel v5(P4-U11)—— glTF 导入产出的 CPU 侧模型资产(不可变)。
	// GPU 资源由 Renderer3D 首次提交时创建,本文件与加载器**不依赖 RHI 设备**,可 headless 使用。
	//
	// 顶点布局两种:
	//   id 1 = standard(position/normal/uv,stride 32,与 Mesh::MakeStandardLayout() 逐字段对应);
	//   id 2 = skinned(标准 32B 之后追加 joints/weights,stride 64);
	// 法线缺失时由导入器按面法线补齐。
	namespace WModelIO
	{
		// v5:P4-U11 起 meta 里带**完整导入设置**(ModelImportSettings)—— 资产自描述,
		// 不再依赖源旁边的 `.wimport` 旁路文件。
		// v1–v4 一律拒绝并提示"请重新导入"(旧文件没有这些区块的读写口径,不做"尽力解析")。
		constexpr uint32_t kFormatVersion = 5;
		constexpr uint32_t kVertexLayoutStandard = 1;
		constexpr uint32_t kVertexLayoutSkinned = 2;
		// 每个 skin 的关节数上限(与 D5c-3 骨骼调色板 ≤128 关节的约定一致)。
		constexpr uint32_t kMaxJointsPerSkin = 128;

		// 动画通道路径(与 glTF 的 T/R/S 一致);T/S 只写 xyz,w 保留。
		enum class WModelAnimationPath : uint8_t
		{
			Translation = 0,
			Rotation = 1,
			Scale = 2,
		};
	}

	struct WModelVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
	};
	static_assert(sizeof(WModelVertex) == 32, "WModelVertex must match the standard vertex layout (32 bytes)");

	// 顶点布局 id 2:标准 32B 之后追加 joints + weights。关节下标用 float 存(0..127 可精确表示),
	// 因为 GL 后端的 glVertexArrayAttribFormat 会把整数顶点属性按浮点读(与 D8b-2 实例化同一个坑),
	// 容器里**不引入整数顶点属性**。
	struct WModelSkinVertex
	{
		glm::vec4 Joints { 0.0f };
		glm::vec4 Weights { 0.0f };
	};
	static_assert(sizeof(WModelSkinVertex) == 32, "WModelSkinVertex must be 4 joints + 4 weights (32 bytes)");

	struct WModelBounds
	{
		glm::vec3 Min { 0.0f };
		glm::vec3 Max { 0.0f };
	};

	// 子网格:一段连续索引 + 材质槽(-1 = 无材质,渲染走 Color 常量色路径)。
	struct WModelSubmesh
	{
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		int32_t MaterialSlot = -1;
		WModelBounds Bounds;
	};

	// 一个 mesh = 一段连续 submesh(glTF mesh ↔ .wmodel mesh,下标一一对应)。
	struct WModelMeshRange
	{
		uint32_t FirstSubmesh = 0;
		uint32_t SubmeshCount = 0;
		// v4:绑定到 Skins[] 的下标(-1 = 非蒙皮网格,继续走静态路径)。
		int32_t SkinIndex = -1;
	};

	// 节点:Parent / MeshIndex 都是**下标**(-1 = 无);Rotation 是四元数(x,y,z,w),
	// 与 TransformComponent::SetRotationQuat 对应。
	struct WModelNode
	{
		int32_t Parent = -1;
		int32_t MeshIndex = -1;
		glm::vec3 Translation { 0.0f };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f };
		std::string Name;
	};

	// v4:骨架。JointParents / 顶点里的 joints 用**本 skin 关节集合**下标;动画通道的 TargetNode
	// 用**节点**下标(与 glTF 一致,运行时节点树就是 Nodes[])。
	// JointNodes[j] 是关节 j 对应的节点下标 —— "关节集合下标 ↔ 节点下标"的唯一映射来源
	// (D5c-3a 冻结):调色板取 nodeWorld[JointNodes[j]],不再靠"两种下标恰好重合"。
	// JointNames / JointNodes / JointParents / InverseBindMatrices / Bind* 长度必须一致 = 该 skin 的 jointCount。
	struct WModelSkin
	{
		std::string Name;
		std::vector<std::string> JointNames;
		// 关节 j → Nodes[] 下标(必须是有效节点下标 0..nodeCount-1;Parse 拒绝越界引用)。
		std::vector<uint32_t> JointNodes;
		std::vector<int32_t> JointParents;
		std::vector<glm::mat4> InverseBindMatrices;
		std::vector<glm::vec3> BindTranslations;
		std::vector<glm::quat> BindRotations;
		std::vector<glm::vec3> BindScales;
	};

	// v4:动画关键帧(vec4;T/S 只用 xyz,R 是四元数 xyzw)。导入期已按固定采样率烘成关键帧,
	// 运行时只做线性插值。
	struct WModelAnimationKey
	{
		float Time = 0.0f;
		glm::vec4 Value { 0.0f };
	};

	struct WModelAnimationChannel
	{
		uint32_t TargetNode = 0;   // 节点下标(glTF 里关节也是节点)
		WModelIO::WModelAnimationPath Path = WModelIO::WModelAnimationPath::Translation;
		std::vector<WModelAnimationKey> Keys;
	};

	struct WModelAnimation
	{
		std::string Name;
		float Duration = 0.0f;
		std::vector<WModelAnimationChannel> Channels;
	};

	struct WModelData
	{
		// `meta` 区块。Valid 只在解析成功时置位;WriteFile 要求 Valid(导入器必须写)。
		struct MetaData
		{
			bool Valid = false;
			uint64_t SourceFingerprint = 0;   // 源文件内容 FNV-1a64
			uint32_t ImporterVersion = 0;     // ModelImporter 版本(当前 1)
			uint64_t SettingsHash = 0;        // ModelImportSettings::Hash
			uint8_t UpAxis = 0;               // 0 = Y;1 = Z(导入期已绕 X 轴 -90° 烘焙)
			float Scale = 1.0f;               // 导入期统一缩放(已烘焙)
			// 源资产逻辑路径(相对内容根,如 "models/tests/rock.gltf";空 = 非导入产物/无源)。
			// 有它才能做"源改了/设置改了 → 需要重导",不必靠"同目录同名"去猜。
			std::string SourcePath;
			// v5:产生这个资产时用的**完整导入设置**(P4-U11)。HasSettings=false 只可能是
			// 手写/外来产物 —— 导入器总是写 true。
			bool HasSettings = false;
			ModelImportSettings Settings;
		};
		MetaData Meta;

		uint32_t Flags = 0;   // 保留字段(当前恒 0)
		// 顶点布局(见 WModelIO::kVertexLayout*);布局 2 时 SkinVertices 必须与 Vertices 一一对应。
		uint32_t VertexLayoutId = WModelIO::kVertexLayoutStandard;
		std::vector<WModelVertex> Vertices;
		// 布局 2 的每顶点 joints/weights;布局 1 时为**空**(Parse 不会给出垃圾数据)。
		std::vector<WModelSkinVertex> SkinVertices;
		std::vector<uint32_t> Indices;
		WModelBounds Bounds;   // 全模型包围盒
		std::vector<WModelSubmesh> Submeshes;
		std::vector<WModelMeshRange> Meshes;
		std::vector<WModelNode> Nodes;
		// 相对内容根的 .wmat 路径(如 "materials/rock.wmat");下标 = submesh.MaterialSlot。
		std::vector<std::string> MaterialSlots;
		// v4:骨架与动画(可空)。空 = 静态资产,零额外开销。
		std::vector<WModelSkin> Skins;
		std::vector<WModelAnimation> Animations;
	};

	// .wmodel v4 读写。格式是小端、版本化且**严格**的:
	//  - 未知 magic / 未知版本 / 截断 / 越界引用一律返回可读错误,绝不"尽力解析";
	//  - v1–v3 **不再兼容**:读到旧版本返回"请重新导入"的可读错误(重导会写出 v4);
	//  - 每个 skin 的关节数上限 kMaxJointsPerSkin = 128(超出给可读错误)。
	//  - Serialize 确定性:同一份数据结构两次序列化逐字节相同(无时间戳、无填充差异)。
	//
	// 字节布局(plan §D5c 冻结格式):
	//   Header{ magic 'WMDL' / version / flags / vertexLayoutId / reserved / vertexCount /
	//           indexCount / meshCount / submeshCount / nodeCount / materialSlotCount /
	//           jointCount(= Skins[] 数组长度) / animationCount }
	//   Meta{sourceFingerprint(u64) / importerVersion(u32) / settingsHash(u64) / upAxis(u8) / scale(f32) /
	//        reserved(u32) / sourcePath(长度前缀字符串,相对内容根) / hasSettings(u8) /
	//        [settings{scale(f32) / upAxis(u8) / exportMaterials(u8) / exportTextures(u8) /
	//                  importAnimations(u8) / importSkins(u8) / animationSampleRate(f32) /
	//                  generateNormals(u8) / reuseMaterials(u8) / reuseTextures(u8) /
	//                  sharedMaterialFolder(长度前缀字符串)}]  ← v5,hasSettings=1 时才有}
	//   Bounds{min,max} → Submeshes[] → Meshes[] → Nodes[] → MaterialSlots[] →
	//   Skins[] → Animations[] →
	//   VertexData(vertexCount × stride:布局 1 = 32B,布局 2 = 64B) → Indices(u32 × indexCount)
	//   Meshes[]:firstSubmesh(u32) / submeshCount(u32) / skinIndex(i32,-1 = 非蒙皮)
	//   Skins[]:name / jointCount(u32,≤128) / jointNames[] / jointNodes[u32] / jointParents[i32] /
	//           inverseBind[jointCount × mat4(f32×16)] / bindTranslation[jointCount × vec3] /
	//           bindRotation[jointCount × quat(f32×4)] / bindScale[jointCount × vec3]
	//   Animations[]:name / duration(f32) / channelCount(u32) /
	//           channel{ targetNode(u32) / path(u8:0=T,1=R,2=S) / keyCount(u32) /
	//                    key{ time(f32) / value(vec4) } }
	namespace WModelIO
	{
		// 顶点布局 id → 顶点 stride(id 1 → 32,id 2 → 64,其它 → 0 表示不支持)。
		WLD_API uint32_t GetVertexStride(uint32_t vertexLayoutId);

		WLD_API std::vector<uint8_t> Serialize(const WModelData& data);
		WLD_API bool Parse(const uint8_t* bytes, size_t size, WModelData& out, std::string* error);
		WLD_API bool WriteFile(const std::string& path, const WModelData& data, std::string* error);
		WLD_API bool ReadFile(const std::string& path, WModelData& out, std::string* error);
		// P4-U11:只解析 header + meta(几何不读,GPU/内存都不碰)。编辑器与 cook 判断
		// "这份 .wmodel 是不是这个源的产物 / 用了什么导入设置"时用它,不必加载整个模型。
		WLD_API bool ReadMeta(const std::string& path, WModelData::MetaData& out, std::string* error);
	}
}
