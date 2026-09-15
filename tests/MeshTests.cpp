// P1b D2:网格资产模型(标准布局/包围盒/索引校验/默认物体)。
#include "World/Renderer/Mesh.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World;

		// 1. 标准布局:位置/法线/UV,stride 32。
		{
			const MeshVertexLayout layout = Mesh::MakeStandardLayout();
			CHECK(layout.Attributes.size() == 3);
			CHECK(layout.GetStride(0) == 32);
			CHECK(layout.Attributes[0].Location == 0 && layout.Attributes[0].Offset == 0);
			CHECK(layout.Attributes[1].Offset == 12);
			CHECK(layout.Attributes[2].Offset == 24);
		}

		// 2. 默认物体:立方体 24 顶点/36 索引,包围盒 ±0.5;平面 4 顶点/6 索引。
		{
			const Ref<Mesh> cube = Mesh::CreateUnitCube(1.0f);
			CHECK(cube != nullptr);
			CHECK(cube->GetVertexCount() == 24);
			CHECK(cube->GetIndexCount() == 36);
			const MeshBounds cubeBounds = cube->GetBounds();
			CHECK(std::fabs(cubeBounds.Min.x + 0.5f) < 1e-5f && std::fabs(cubeBounds.Max.y - 0.5f) < 1e-5f);
			CHECK(std::fabs(cubeBounds.GetRadius() - glm::length(glm::vec3(0.5f))) < 1e-4f);

			const Ref<Mesh> plane = Mesh::CreateUnitPlane(2.0f);
			CHECK(plane != nullptr);
			CHECK(plane->GetVertexCount() == 4);
			CHECK(plane->GetIndexCount() == 6);
			CHECK(std::fabs(plane->GetBounds().Min.y) < 1e-6f && std::fabs(plane->GetBounds().Max.y) < 1e-6f);
			CHECK(std::fabs(plane->GetBounds().Max.x - 1.0f) < 1e-5f);
		}

		// 3. 非法描述必须被拒绝:顶点数据不是 stride 整数倍 / 索引越界 / 空数据。
		{
			MeshDesc desc;
			desc.Layout = Mesh::MakeStandardLayout();
			CHECK(Mesh::Create(desc) == nullptr);   // 空顶点数据

			desc.VertexData.resize(32 * 3 + 7);     // 3.2 个顶点
			CHECK(Mesh::Create(desc) == nullptr);

			desc.VertexData.resize(32 * 3);
			desc.Indices = { 0, 1, 2 };
			CHECK(Mesh::Create(desc) != nullptr);
			desc.Indices = { 0, 1, 3 };             // 3 == 顶点数,越界
			CHECK(Mesh::Create(desc) == nullptr);
		}

		// 4. 自定义布局:仅位置(float3,stride 12)也能算包围盒。
		{
			MeshDesc desc;
			desc.Layout.Attributes = { { 0, 0, Rhi::Format::R32G32B32_SFLOAT, 0 } };
			desc.Layout.Bindings = { { 0, 12, false } };
			const float positions[6] = { -1.0f, -2.0f, -3.0f, 4.0f, 5.0f, 6.0f };
			desc.VertexData.resize(sizeof(positions));
			std::memcpy(desc.VertexData.data(), positions, sizeof(positions));
			desc.Indices = { 0, 1, 0 };

			const Ref<Mesh> mesh = Mesh::Create(desc);
			CHECK(mesh != nullptr);
			CHECK(mesh->GetVertexCount() == 2);
			CHECK(std::fabs(mesh->GetBounds().Min.x + 1.0f) < 1e-6f);
			CHECK(std::fabs(mesh->GetBounds().Max.z - 6.0f) < 1e-6f);
		}

		std::printf("World.Mesh: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Mesh FAILED: %s\n", error.what());
		return 1;
	}
}
