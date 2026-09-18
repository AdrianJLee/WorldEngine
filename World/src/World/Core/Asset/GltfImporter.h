#pragma once

#include "World/Core/Export.h"

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
	//  - 不支持特性(skin/动画/morph/sparse/Draco/KTX2/非三角图元/必须的不支持扩展)
	//    **硬报错**;唯一降级:顶点没有 NORMAL 时按面法线补齐并记一条 warning;
	//  - 失败返回 false + error 可读原因;校验阶段失败时**不写出**任何输出文件
	//    (贴图/材质/模型全部先在内存准备,最后才落盘;落盘中途 IO 失败可能留下已写出的前几个文件)。
	class WLD_API GltfImporter
	{
	public:
		static bool ImportFile(const std::string& sourcePath, const std::string& outputRoot,
			GltfImportResult* result, std::string* error);
	};

	// 便捷入口(编辑器 `--import-gltf` 等调用方持有 filesystem::path):
	// 等价于 GltfImporter::ImportFile(source.string(), outputRoot.string(), ...)。
	WLD_API bool ImportFile(const std::filesystem::path& sourcePath,
		const std::filesystem::path& outputRoot, GltfImportResult* result, std::string* error);
}
