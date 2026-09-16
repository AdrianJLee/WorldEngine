#include "wldpch.h"
#include "World/Renderer/Mesh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstring>

namespace World
{
	namespace
	{
		// 标准顶点结构:与 MakeStandardLayout 的 offset/stride 保持一致。
		struct StandardVertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 TexCoord;
		};
		static_assert(sizeof(StandardVertex) == 32, "StandardVertex must stay 32 bytes (stride contract)");

		uint32_t FormatSize(Rhi::Format format)
		{
			using Rhi::Format;
			switch (format)
			{
			case Format::R32_SFLOAT: return 4;
			case Format::R32G32_SFLOAT: return 8;
			case Format::R32G32B32_SFLOAT: return 12;
			case Format::R32G32B32A32_SFLOAT: return 16;
			default: return 0;
			}
		}
	}

	uint32_t MeshVertexLayout::GetStride(uint32_t binding) const
	{
		for (const Rhi::VertexBinding& declared : Bindings)
			if (declared.Binding == binding)
				return declared.Stride;

		// 没有显式声明时按属性推导:取"该绑定内最大 offset + 该属性大小"。
		uint32_t stride = 0;
		for (const Rhi::VertexAttribute& attribute : Attributes)
		{
			if (attribute.Binding != binding)
				continue;
			stride = std::max(stride, attribute.Offset + FormatSize(attribute.Format));
		}
		return stride;
	}

	MeshVertexLayout Mesh::MakeStandardLayout()
	{
		MeshVertexLayout layout;
		layout.Attributes = {
			{ 0, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(StandardVertex, Position)) },
			{ 1, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(StandardVertex, Normal)) },
			{ 2, 0, Rhi::Format::R32G32_SFLOAT, static_cast<uint32_t>(offsetof(StandardVertex, TexCoord)) },
		};
		layout.Bindings = { { 0, static_cast<uint32_t>(sizeof(StandardVertex)), false } };
		return layout;
	}

	MeshBounds Mesh::ComputeBounds(const MeshVertexLayout& layout, const std::vector<uint8_t>& vertexData)
	{
		MeshBounds bounds;
		const uint32_t stride = layout.GetStride(0);
		if (stride == 0 || vertexData.empty() || vertexData.size() % stride != 0)
			return bounds;

		uint32_t positionOffset = UINT32_MAX;
		for (const Rhi::VertexAttribute& attribute : layout.Attributes)
			if (attribute.Location == 0 && attribute.Binding == 0)
				positionOffset = attribute.Offset;
		if (positionOffset == UINT32_MAX || positionOffset + sizeof(float) * 3 > stride)
			return bounds;

		const uint32_t vertexCount = static_cast<uint32_t>(vertexData.size() / stride);
		glm::vec3 min(std::numeric_limits<float>::max());
		glm::vec3 max(std::numeric_limits<float>::lowest());
		for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
		{
			float position[3] = { 0.0f, 0.0f, 0.0f };
			std::memcpy(position, vertexData.data() + vertex * stride + positionOffset, sizeof(position));
			const glm::vec3 value(position[0], position[1], position[2]);
			min = glm::min(min, value);
			max = glm::max(max, value);
		}
		bounds.Min = min;
		bounds.Max = max;
		return bounds;
	}

	Mesh::Mesh(MeshDesc desc, MeshBounds bounds) : m_Desc(std::move(desc)), m_Bounds(bounds) {}

	Ref<Mesh> Mesh::Create(const MeshDesc& desc)
	{
		const uint32_t stride = desc.Layout.GetStride(0);
		if (stride == 0 || desc.VertexData.empty())
			return nullptr;
		if (desc.VertexData.size() % stride != 0)
			return nullptr;

		const uint32_t vertexCount = static_cast<uint32_t>(desc.VertexData.size() / stride);
		for (const uint32_t index : desc.Indices)
			if (index >= vertexCount)
				return nullptr;

		return Ref<Mesh>(new Mesh(desc, ComputeBounds(desc.Layout, desc.VertexData)));
	}

	uint32_t Mesh::GetVertexCount() const
	{
		const uint32_t stride = m_Desc.Layout.GetStride(0);
		return stride == 0 ? 0 : static_cast<uint32_t>(m_Desc.VertexData.size() / stride);
	}

	Ref<Mesh> Mesh::CreateUnitCube(float size)
	{
		const float h = size * 0.5f;
		// 每个面 4 个独立顶点(法线/UV 不共享),与大多数引擎的默认立方体一致。
		struct Face
		{
			glm::vec3 Normal;
			glm::vec3 Corners[4];
		};
		const Face faces[6] = {
			{ {  0,  0,  1 }, { { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h } } },
			{ {  0,  0, -1 }, { {  h, -h, -h }, { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h } } },
			{ {  1,  0,  0 }, { {  h, -h,  h }, {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h } } },
			{ { -1,  0,  0 }, { { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h } } },
			{ {  0,  1,  0 }, { { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h }, { -h,  h, -h } } },
			{ {  0, -1,  0 }, { { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h } } },
		};
		const glm::vec2 uvs[4] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };

		MeshDesc desc;
		desc.DebugName = "UnitCube";
		desc.Layout = MakeStandardLayout();
		desc.VertexData.resize(sizeof(StandardVertex) * 24);
		auto* vertices = reinterpret_cast<StandardVertex*>(desc.VertexData.data());
		for (uint32_t face = 0; face < 6; ++face)
			for (uint32_t corner = 0; corner < 4; ++corner)
			{
				StandardVertex& vertex = vertices[face * 4 + corner];
				vertex.Position = faces[face].Corners[corner];
				vertex.Normal = faces[face].Normal;
				vertex.TexCoord = uvs[corner];
			}
		desc.Indices.reserve(36);
		for (uint32_t face = 0; face < 6; ++face)
		{
			const uint32_t base = face * 4;
			// 约定(与 UnitPlane 一致):顶点按面对外法线逆时针排列,即
			// (v1-v0)×(v2-v0) 指向**外**法线方向。之前这里是 {0,2,1, 2,0,3},
			// 叉积指向内法线 —— cube 与 plane 绕序相反,任何单一 FrontFace 约定
			// 都必然让其中一个"里外反了"(实测:立方体看到内壁)。
			desc.Indices.insert(desc.Indices.end(),
				{ base + 0, base + 1, base + 2, base + 0, base + 2, base + 3 });
		}
		return Create(desc);
	}

	Ref<Mesh> Mesh::CreateUnitPlane(float size)
	{
		const float h = size * 0.5f;
		MeshDesc desc;
		desc.DebugName = "UnitPlane";
		desc.Layout = MakeStandardLayout();
		desc.VertexData.resize(sizeof(StandardVertex) * 4);
		auto* vertices = reinterpret_cast<StandardVertex*>(desc.VertexData.data());
		vertices[0] = { { -h, 0.0f, -h }, { 0, 1, 0 }, { 0, 1 } };
		vertices[1] = { {  h, 0.0f, -h }, { 0, 1, 0 }, { 1, 1 } };
		vertices[2] = { {  h, 0.0f,  h }, { 0, 1, 0 }, { 1, 0 } };
		vertices[3] = { { -h, 0.0f,  h }, { 0, 1, 0 }, { 0, 0 } };
		desc.Indices = { 0, 2, 1, 2, 0, 3 };
		return Create(desc);
	}

	Ref<Mesh> Mesh::CreateUnitSphere(float size, uint32_t segments, uint32_t rings)
	{
		// D3:材质预览的标准球。角度约定与 Mesh::CreateUnitCube/Plane 一致(外法线朝外、
		// 绕序从外侧看逆时针),这样 3D 管线用同一套 FrontFace/剔除设置即可。
		segments = std::max(3u, segments);
		rings = std::max(2u, rings);
		const float radius = size * 0.5f;

		MeshDesc desc;
		desc.DebugName = "UnitSphere";
		desc.Layout = MakeStandardLayout();

		const uint32_t vertexColumns = segments + 1;
		const uint32_t vertexRows = rings + 1;
		desc.VertexData.resize(sizeof(StandardVertex) * vertexColumns * vertexRows);
		auto* vertices = reinterpret_cast<StandardVertex*>(desc.VertexData.data());

		constexpr float kPi = 3.14159265358979323846f;
		for (uint32_t row = 0; row < vertexRows; ++row)
		{
			const float v = static_cast<float>(row) / static_cast<float>(rings);
			const float phi = v * kPi;                  // 0(北极) → π(南极)
			const float sinPhi = std::sin(phi);
			const float cosPhi = std::cos(phi);
			for (uint32_t column = 0; column < vertexColumns; ++column)
			{
				const float u = static_cast<float>(column) / static_cast<float>(segments);
				const float theta = u * 2.0f * kPi;
				const glm::vec3 normal { sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta) };
				StandardVertex& vertex = vertices[row * vertexColumns + column];
				vertex.Position = normal * radius;
				vertex.Normal = normal;
				vertex.TexCoord = { u, v };
			}
		}

		desc.Indices.reserve(static_cast<size_t>(segments) * rings * 6);
		for (uint32_t row = 0; row < rings; ++row)
		{
			for (uint32_t column = 0; column < segments; ++column)
			{
				const uint32_t a = row * vertexColumns + column;
				const uint32_t b = a + 1;
				const uint32_t c = a + vertexColumns;
				const uint32_t d = c + 1;
				// 从外侧看逆时针(与 cube/plane 的约定一致)。
				desc.Indices.insert(desc.Indices.end(), { a, c, b, b, c, d });
			}
		}
		return Create(desc);
	}
}
