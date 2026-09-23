#include "wldpch.h"
#include "World/Renderer/Mesh.h"

#include "World/Core/Asset/WModelIO.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_map>

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

		// D5c-3b 布局 2 顶点:标准 32B + joints/weights 32B = 64B。
		// 与 WModelIO::WModelVertex + WModelIO::WModelSkinVertex 逐字段对应(memcpy 逐块拷贝)。
		struct SkinnedVertex
		{
			glm::vec3 Position;
			glm::vec3 Normal;
			glm::vec2 TexCoord;
			glm::vec4 Joints;
			glm::vec4 Weights;
		};
		static_assert(sizeof(SkinnedVertex) == 64, "SkinnedVertex must stay 64 bytes (stride contract)");
		static_assert(offsetof(SkinnedVertex, Joints) == 32, "SkinnedVertex joints must start at byte 32");
		// joints+weights 块大小用本地布局表达(不再引用 Asset 侧类型:Mesh.cpp 只依赖自己的
		// 顶点布局,避免跨模块头文件/PCH 的可见性问题——实测引用 Asset::WModelIO::WModelSkinVertex 编译不过)。
		static_assert(sizeof(SkinnedVertex) - offsetof(SkinnedVertex, Joints) == 32,
			"skin vertex block (joints+weights) must be 32 bytes");

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

		// .wmodel 进程内缓存(键 = 规范化路径)。不导出:生命周期与进程一致,
		// Renderer3D 的 GPU 缓冲缓存按 Mesh 指针做键,因此 MeshGpu 里持有 Owner Ref,
		// 保证"缓存被清空后新 Mesh 复用同一地址"不会命中旧 GPU 资源。
		std::unordered_map<std::string, Ref<Mesh>>& WModelCache()
		{
			static std::unordered_map<std::string, Ref<Mesh>> cache;
			return cache;
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

	MeshVertexLayout Mesh::MakeSkinnedLayout()
	{
		MeshVertexLayout layout;
		layout.Attributes = {
			{ 0, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(SkinnedVertex, Position)) },
			{ 1, 0, Rhi::Format::R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(SkinnedVertex, Normal)) },
			{ 2, 0, Rhi::Format::R32G32_SFLOAT, static_cast<uint32_t>(offsetof(SkinnedVertex, TexCoord)) },
			{ 3, 0, Rhi::Format::R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(SkinnedVertex, Joints)) },
			{ 4, 0, Rhi::Format::R32G32B32A32_SFLOAT, static_cast<uint32_t>(offsetof(SkinnedVertex, Weights)) },
		};
		layout.Bindings = { { 0, static_cast<uint32_t>(sizeof(SkinnedVertex)), false } };
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

	Mesh::Mesh(MeshDesc desc, MeshBounds bounds, std::vector<MeshSubmesh> submeshes,
		std::vector<MeshRange> meshes, std::vector<MeshNode> nodes, std::vector<std::string> materialSlots)
		: m_Desc(std::move(desc)), m_Bounds(bounds), m_Submeshes(std::move(submeshes)),
		m_Meshes(std::move(meshes)), m_Nodes(std::move(nodes)), m_MaterialSlots(std::move(materialSlots))
	{
	}

	Ref<Mesh> Mesh::LoadWModel(const std::string& path, std::string* error)
	{
		if (path.empty())
		{
			if (error) *error = "path is empty";
			return nullptr;
		}

		const std::string key = std::filesystem::path(path).lexically_normal().generic_string();
		auto& cache = WModelCache();
		const auto cached = cache.find(key);
		if (cached != cache.end())
		{
			if (error) error->clear();
			return cached->second;
		}

		Asset::WModelData data;
		std::string loadError;
		if (!Asset::WModelIO::ReadFile(path, data, &loadError))
		{
			if (error) *error = loadError;
			return nullptr;
		}
		if (data.Vertices.empty() || data.Indices.empty())
		{
			if (error) *error = "'" + path + "': .wmodel has no geometry";
			return nullptr;
		}
		// 包围盒健全性:格式里存了 min/max,坏值(反向/NaN)会让阴影正交矩阵与后续剔除失真,
		// 属于"坏文件硬报错"的一部分。
		const auto finite = [](const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		};
		if (!finite(data.Bounds.Min) || !finite(data.Bounds.Max)
			|| data.Bounds.Min.x > data.Bounds.Max.x
			|| data.Bounds.Min.y > data.Bounds.Max.y
			|| data.Bounds.Min.z > data.Bounds.Max.z)
		{
			if (error) *error = "'" + path + "': .wmodel has invalid bounds";
			return nullptr;
		}

		MeshDesc desc;
		desc.DebugName = std::filesystem::path(path).stem().string();
		desc.VertexLayoutId = data.VertexLayoutId;
		if (data.VertexLayoutId == kVertexLayoutSkinned)
		{
			// 布局 2:解析器已保证 SkinVertices 与 Vertices 一一对应(长度不齐时 Parse 就报错),
			// 这里再挡一次,避免手工构造的 WModelData 越界读。
			if (data.SkinVertices.size() != data.Vertices.size())
			{
				if (error) *error = "'" + path + "': skinned .wmodel has mismatched joints/weights count";
				return nullptr;
			}
			desc.Layout = MakeSkinnedLayout();
			desc.VertexData.resize(data.Vertices.size() * sizeof(SkinnedVertex));
			// 逐顶点拼接(标准 32B + skin 32B):WModelVertex 与 StandardVertex,
			// WModelSkinVertex 与 SkinnedVertex 的后半段逐字段一致,整块 memcpy 即可。
			for (size_t vertex = 0; vertex < data.Vertices.size(); ++vertex)
			{
				auto* target = reinterpret_cast<SkinnedVertex*>(
					desc.VertexData.data() + vertex * sizeof(SkinnedVertex));
				std::memcpy(&target->Position, &data.Vertices[vertex], sizeof(StandardVertex));
				std::memcpy(&target->Joints, &data.SkinVertices[vertex],
					sizeof(SkinnedVertex) - offsetof(SkinnedVertex, Joints));
			}
		}
		else
		{
			// 布局 1 逐字节不变:同一份 MakeStandardLayout + StandardVertex 直拷。
			desc.Layout = MakeStandardLayout();
			desc.VertexData.resize(data.Vertices.size() * sizeof(StandardVertex));
			std::memcpy(desc.VertexData.data(), data.Vertices.data(), desc.VertexData.size());
		}
		desc.Indices = std::move(data.Indices);

		Ref<Mesh> mesh = Create(desc);
		if (!mesh)
		{
			if (error) *error = "'" + path + "': .wmodel geometry is invalid (index out of range)";
			return nullptr;
		}

		std::vector<MeshSubmesh> submeshes;
		submeshes.reserve(data.Submeshes.size());
		for (const Asset::WModelSubmesh& source : data.Submeshes)
			submeshes.push_back({ source.IndexOffset, source.IndexCount, source.MaterialSlot,
				MeshBounds { source.Bounds.Min, source.Bounds.Max } });
		std::vector<MeshRange> ranges;
		ranges.reserve(data.Meshes.size());
		for (const Asset::WModelMeshRange& source : data.Meshes)
			ranges.push_back({ source.FirstSubmesh, source.SubmeshCount });
		std::vector<MeshNode> nodes;
		nodes.reserve(data.Nodes.size());
		for (const Asset::WModelNode& source : data.Nodes)
			nodes.push_back({ source.Parent, source.MeshIndex, source.Translation, source.Rotation,
				source.Scale, source.Name });

		// 文件里的包围盒即资产契约(导入器按几何算出);Create 已按顶点重算一遍,
		// 这里用文件值覆盖,保持 .wmodel 的"存什么读什么"语义。
		Ref<Mesh> result(new Mesh(std::move(desc), MeshBounds { data.Bounds.Min, data.Bounds.Max },
			std::move(submeshes), std::move(ranges), std::move(nodes), data.MaterialSlots));
		cache.emplace(key, result);
		if (error) error->clear();
		return result;
	}

	void Mesh::ClearWModelCache()
	{
		WModelCache().clear();
	}

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
