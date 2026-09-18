#pragma once

#include "World/Core/Export.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace World::Asset
{
	// P1b D5:.wmodel v1 —— glTF 导入产出的 CPU 侧模型资产(不可变)。
	// GPU 资源由 Renderer3D 首次提交时创建,本文件与加载器**不依赖 RHI 设备**,可 headless 使用。
	//
	// 顶点布局固定为 standard(position/normal/uv,stride 32,vertexLayoutId = 1),
	// 与 Mesh::MakeStandardLayout() 逐字段对应;法线缺失时由导入器按面法线补齐。
	struct WModelVertex
	{
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 TexCoord;
	};
	static_assert(sizeof(WModelVertex) == 32, "WModelVertex must match the standard vertex layout (32 bytes)");

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

	struct WModelData
	{
		uint32_t Flags = 0;   // 保留字段(当前恒 0)
		std::vector<WModelVertex> Vertices;
		std::vector<uint32_t> Indices;
		WModelBounds Bounds;   // 全模型包围盒
		std::vector<WModelSubmesh> Submeshes;
		std::vector<WModelMeshRange> Meshes;
		std::vector<WModelNode> Nodes;
		// 相对内容根的 .wmat 路径(如 "materials/rock.wmat");下标 = submesh.MaterialSlot。
		std::vector<std::string> MaterialSlots;
	};

	// .wmodel v1 读写。格式是小端、版本化且**严格**的:
	//  - 未知 magic / 未知版本 / 截断 / 越界引用一律返回可读错误,绝不"尽力解析";
	//  - Serialize 确定性:同一份数据结构两次序列化逐字节相同(无时间戳、无填充差异)。
	//
	// 字节布局(plan §D5 冻结格式):
	//   Header{ magic 'WMDL' / version / flags / vertexLayoutId / reserved / vertexCount /
	//           indexCount / meshCount / submeshCount / nodeCount / materialSlotCount }
	//   Bounds{min,max} → Submeshes[] → Meshes[] → Nodes[] → MaterialSlots[] →
	//   VertexData(vertexCount × 32B) → Indices(u32 × indexCount)
	namespace WModelIO
	{
		constexpr uint32_t kFormatVersion = 1;
		constexpr uint32_t kVertexLayoutStandard = 1;

		WLD_API std::vector<uint8_t> Serialize(const WModelData& data);
		WLD_API bool Parse(const uint8_t* bytes, size_t size, WModelData& out, std::string* error);
		WLD_API bool WriteFile(const std::string& path, const WModelData& data, std::string* error);
		WLD_API bool ReadFile(const std::string& path, WModelData& out, std::string* error);
	}
}
