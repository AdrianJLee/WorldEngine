#pragma once

#include "World/Core/Export.h"
#include "World/RHI/RhiPipeline.h"

#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	// 网格顶点布局:直接复用 RHI 的属性/绑定描述,管线与网格共用同一份声明。
	struct MeshVertexLayout
	{
		std::vector<Rhi::VertexAttribute> Attributes;
		std::vector<Rhi::VertexBinding> Bindings;

		uint32_t GetStride(uint32_t binding = 0) const;
	};

	struct MeshBounds
	{
		glm::vec3 Min { 0.0f };
		glm::vec3 Max { 0.0f };

		glm::vec3 GetCenter() const { return (Min + Max) * 0.5f; }
		glm::vec3 GetExtents() const { return (Max - Min) * 0.5f; }
		float GetRadius() const { return glm::length(GetExtents()); }
	};

	// ---- P1b D5:.wmodel 的附加数据(内置 primitive 为空) ----
	// 子网格:一段连续索引 + 材质槽(-1 = 无材质)+ 局部包围盒。
	struct MeshSubmesh
	{
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		int32_t MaterialSlot = -1;
		MeshBounds Bounds;
	};

	// 一个 mesh = 一段连续 submesh(下标 = glTF mesh 下标 = MeshRendererComponent::MeshIndex)。
	struct MeshRange
	{
		uint32_t FirstSubmesh = 0;
		uint32_t SubmeshCount = 0;
	};

	// 节点树(实例化 ModelInstance 用):Parent/MeshIndex 为下标,-1 = 无;Rotation 为四元数。
	struct MeshNode
	{
		int32_t Parent = -1;
		int32_t MeshIndex = -1;
		glm::vec3 Translation { 0.0f };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f };
		std::string Name;
	};

	// 网格数据(P1b D2):CPU 侧不可变数据 + 布局 + 包围盒。
	// GPU 缓冲由 Renderer3D 在首次提交时按布局创建(此处不依赖 RHI 设备,便于导入器/单测使用)。
	struct MeshDesc
	{
		std::string DebugName = "Mesh";
		std::vector<uint8_t> VertexData;
		std::vector<uint32_t> Indices;
		MeshVertexLayout Layout;
	};

	class WLD_API Mesh
	{
	public:
		// 校验布局/数据一致性(顶点数据必须是 stride 的整数倍、索引必须在范围内);
		// 不合法的描述返回 nullptr,由调用方决定回退(导入器会报错并跳过该网格)。
		static Ref<Mesh> Create(const MeshDesc& desc);

		const MeshDesc& GetDesc() const { return m_Desc; }
		const MeshVertexLayout& GetLayout() const { return m_Desc.Layout; }
		uint32_t GetVertexCount() const;
		uint32_t GetIndexCount() const { return static_cast<uint32_t>(m_Desc.Indices.size()); }
		const MeshBounds& GetBounds() const { return m_Bounds; }

		// 便捷构造(编辑器默认物体与测试用)。
		static Ref<Mesh> CreateUnitCube(float size = 1.0f);
		static Ref<Mesh> CreateUnitPlane(float size = 1.0f);
		// D3:材质预览用的标准球(UV 球,半径 = size/2,经纬分段)。
		static Ref<Mesh> CreateUnitSphere(float size = 1.0f, uint32_t segments = 32, uint32_t rings = 16);

		// 标准布局:location0=Position(float3) / 1=Normal(float3) / 2=TexCoord(float2),stride 32。
		static MeshVertexLayout MakeStandardLayout();
		// 按 layout 从顶点数据里取 Position(location 0)算包围盒;找不到 Position 时返回全零包围盒。
		static MeshBounds ComputeBounds(const MeshVertexLayout& layout, const std::vector<uint8_t>& vertexData);

		// ---- P1b D5:.wmodel 加载 ----
		// 从磁盘加载 .wmodel(纯 CPU,不依赖 RHI 设备,可 headless)并做**进程内缓存**:
		// 同一路径永远返回同一个 Ref<Mesh>(每个实体重复提交不会重复读盘/重复上传 GPU 缓冲)。
		// 失败(坏 magic/版本/截断/越界/坏包围盒)返回 nullptr,error 给可读原因,绝不"尽力解析"。
		static Ref<Mesh> LoadWModel(const std::string& path, std::string* error = nullptr);
		// 清空进程内缓存(重新导入/热重载后调用);已经取出的 Ref 仍然有效。
		static void ClearWModelCache();

		// 子网格/节点树访问器:HasSubmeshes() == false 时按"整网格 + 单材质"提交(内置 cube/plane/sphere)。
		bool HasSubmeshes() const { return !m_Submeshes.empty(); }
		const std::vector<MeshSubmesh>& GetSubmeshes() const { return m_Submeshes; }
		const std::vector<MeshRange>& GetMeshes() const { return m_Meshes; }
		const std::vector<MeshNode>& GetNodes() const { return m_Nodes; }
		// submesh.MaterialSlot 指向这里的下标;路径相对内容根(如 "materials/rock.wmat")。
		const std::vector<std::string>& GetMaterialSlots() const { return m_MaterialSlots; }

	private:
		explicit Mesh(MeshDesc desc, MeshBounds bounds);
		Mesh(MeshDesc desc, MeshBounds bounds, std::vector<MeshSubmesh> submeshes,
			std::vector<MeshRange> meshes, std::vector<MeshNode> nodes, std::vector<std::string> materialSlots);

		MeshDesc m_Desc;
		MeshBounds m_Bounds;
		std::vector<MeshSubmesh> m_Submeshes;
		std::vector<MeshRange> m_Meshes;
		std::vector<MeshNode> m_Nodes;
		std::vector<std::string> m_MaterialSlots;
	};
}
