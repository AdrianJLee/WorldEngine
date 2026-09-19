#pragma once

#include "World/Core/Export.h"
#include "World/Core/Asset/ModelImportSettings.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace World::Asset
{
	// P1b D5:glTF 2.0 导入结果。所有路径都是**相对 outputRoot(内容根)**的逻辑路径,
	// 与 .wmodel 的材质槽、场景里的资产引用同一约定。
	struct GltfImportResult
	{
		std::string WModelPath;                    // 写出的模型,如 "models/rock.wmodel"
		std::vector<std::string> MaterialPaths;    // "materials/rock_<name>.wmat",下标 = 材质槽
		std::vector<std::string> TexturePaths;     // "textures/rock_<imageIndex>.<png|jpg>"
		std::vector<std::string> Warnings;         // 降级/忽略项(缺法线、MASK 按 Opaque、未支持扩展等)
		uint32_t VertexCount = 0;
		uint32_t IndexCount = 0;
		uint32_t MeshCount = 0;
		uint32_t SubmeshCount = 0;
		uint32_t NodeCount = 0;
		uint32_t MaterialSlotCount = 0;
		uint64_t SourceFingerprint = 0;            // 源文件内容 FNV-1a64(写入 .wmodel meta)
		uint8_t UpAxis = 0;                        // 0 = Y;1 = Z(与 meta 一致)
	};

	// D5b-1:导入器要写进 .wmodel meta 的身份(源指纹由内核按源字节算,不在这里传)。
	struct GltfImportMetadata
	{
		uint32_t ImporterVersion = 1;
		uint64_t SettingsHash = 0;
		uint8_t UpAxis = 0;      // 0 = Y;1 = Z(调用方必须与 settings 一致)
		float Scale = 1.0f;
		// .wmodel 的逻辑路径(相对内容根);材质/贴图与它同目录。空 = 默认按源文件所在目录
		// 推导(与源同目录同名,见 plan §D5b-1 多产物落盘规则)。
		std::string LogicalModelPath;
		// 源资产的逻辑路径(相对内容根,如 "models/tests/rock.gltf"):写进 .wmodel meta,
		// 供编辑器判断"源/设置是否已变 → 需要重导"。空 = 调用方没有源上下文(测试自造)。
		std::string SourceLogicalPath;
		// D10(用户 2026-09-19 决定 Q1=方案 A):**产物目的地**(相对内容根的逻辑目录,
		// 如 "models/props")。空 = 按源所在目录(SouceLogicalPath 的目录)推导,cook 与
		// 编辑器因此天然同一条规则:产物始终落在"用户选的那个文件夹"里。
		std::string DestinationLogicalDir;
		// D10:内容根的**绝对路径**,只有做"同内容复用"(材质/贴图去重)时才需要。
		// 空 = 跳过复用查找(行为退化为每个模型一份副本)。
		std::string ContentRootAbsolute;
	};

	// D5b-1:内存产物(LogicalPath 与 ImportFile 写出的磁盘布局一致,相对 outputRoot)。
	struct GltfInMemoryOutput
	{
		std::string LogicalPath;
		std::vector<uint8_t> Data;
	};

	// D5b-1:内存导入结果。Outputs 已按"贴图 → 材质 → 模型"排序,模型最后写(见 ImportFile)。
	struct GltfImportBytesResult
	{
		GltfImportResult Summary;
		GltfImportMetadata Metadata;
		std::vector<GltfInMemoryOutput> Outputs;
	};

	// glTF 2.0(.gltf / .glb)→ .wmodel + .wmat + 贴图。**CPU-only**:不依赖 RHI 设备或窗口,
	// headless 可用(编辑器 `--import-gltf` 与单测走同一条路径)。
	//
	// 契约(plan §D5 冻结决定):
	//  - 输出落在 outputRoot 的 models/、materials/、textures/ 下;.wmodel 的材质槽存
	//    **相对内容根**的 .wmat 路径;材质字段填进现有 MaterialDesc;
	//  - 贴图**原样字节**写出(不重编码);external uri 相对 glTF 文件所在目录解析;
	//  - 静态网格:节点树/TRS、多 mesh/多 primitive → meshes/submeshes + 材质槽;
	//    pbrMetallicRoughness(baseColor/metallic/roughness/emissive)、alphaMode、doubleSided;
	//  - D5c:glTF skins → .wmodel Skins[](关节名/父级/反绑定矩阵/绑定 TRS),节点 mesh+skin
	//    → MeshRange.SkinIndex,顶点 JOINTS_0/WEIGHTS_0 → 布局 2 的 SkinVertices;
	//    glTF animations → Animations[](按 settings.AnimationSampleRate 烘等间隔关键帧);
	//  - 不支持特性(morph/sparse/Draco/KTX2/非三角图元/必须的不支持扩展)**硬报错**;
	//    降级:顶点没有 NORMAL 时按面法线补齐、STEP/CUBICSPLINE 按采样率烘线性关键帧,
	//    各记一条 warning;
	//  - 失败返回 false + error 可读原因;校验阶段失败时**不写出**任何输出文件
	//    (贴图/材质/模型全部先在内存准备,最后才落盘;落盘中途 IO 失败可能留下已写出的前几个文件)。
	class WLD_API GltfImporter
	{
	public:
		// D5b-1 内核(cook 复用):只产出内存字节,不写盘;失败返回 false + error。
		// .wmodel 字节在返回前用 WModelIO::Parse 自校验(坏模型在 cook 期报错)。
		// ImportFile 是本函数的薄壳(逐项落盘,行为不变)。
		static bool ImportAsBytes(const std::string& sourcePath,
			const ModelImportSettings& settings, const GltfImportMetadata& metadata,
			GltfImportBytesResult* result, std::string* error);

		// destinationLogicalDir(相对 outputRoot 的逻辑目录,如 "models/props";空 = 源所在目录)
		// 决定产物落点 —— 用户决定 Q1=方案 A:导入到哪个文件夹就落在哪个文件夹。
		static bool ImportFile(const std::string& sourcePath, const std::string& outputRoot,
			GltfImportResult* result, std::string* error,
			const std::string& destinationLogicalDir = std::string());
	};

	// 便捷入口(编辑器 `--import-gltf` 等调用方持有 filesystem::path):
	// 等价于 GltfImporter::ImportFile(source.string(), outputRoot.string(), ...)。
	WLD_API bool ImportFile(const std::filesystem::path& sourcePath,
		const std::filesystem::path& outputRoot, GltfImportResult* result, std::string* error,
		const std::string& destinationLogicalDir = std::string());
}
