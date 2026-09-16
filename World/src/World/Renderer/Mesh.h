#pragma once

#include "World/Core/Export.h"
#include "World/RHI/RhiPipeline.h"

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

	private:
		explicit Mesh(MeshDesc desc, MeshBounds bounds);

		MeshDesc m_Desc;
		MeshBounds m_Bounds;
	};
}
