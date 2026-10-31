#include "wldpch.h"
#include "World/Level/WhiteboxBuilder.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace World
{
	void WhiteboxBuilder::BuildBox(const glm::vec3& dimensions, float, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const glm::vec3 h = dimensions * 0.5f;

		const glm::vec3 faceNormals[6] = {
			{  0,  0,  1 }, {  0,  0, -1 },
			{  1,  0,  0 }, { -1,  0,  0 },
			{  0,  1,  0 }, {  0, -1,  0 }
		};

		const glm::vec3 faceCorners[6][4] = {
			{ {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z} }, // +Z
			{ { h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x,  h.y, -h.z}, { h.x,  h.y, -h.z} }, // -Z
			{ { h.x, -h.y,  h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, { h.x,  h.y,  h.z} }, // +X
			{ {-h.x, -h.y, -h.z}, {-h.x, -h.y,  h.z}, {-h.x,  h.y,  h.z}, {-h.x,  h.y, -h.z} }, // -X
			{ {-h.x,  h.y,  h.z}, { h.x,  h.y,  h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z} }, // +Y
			{ {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x, -h.y,  h.z}, {-h.x, -h.y,  h.z} }  // -Y
		};

		for (int f = 0; f < 6; ++f)
		{
			const uint32_t base = static_cast<uint32_t>(outMesh.Vertices.size());
			for (int i = 0; i < 4; ++i)
			{
				outMesh.Vertices.push_back(faceCorners[f][i]);
				outMesh.Normals.push_back(faceNormals[f]);
			}
			outMesh.UVs.push_back({ 0, 0 });
			outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 });
			outMesh.UVs.push_back({ 0, 1 });

			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 1);
			outMesh.Indices.push_back(base + 2);

			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 2);
			outMesh.Indices.push_back(base + 3);
		}
	}

	void WhiteboxBuilder::BuildCylinder(float radius, float height, uint32_t segments, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const uint32_t seg = std::max(6u, segments);
		const float halfH = height * 0.5f;
		constexpr float k2Pi = 6.28318530718f;

		// 1. 侧面圆柱壁
		for (uint32_t i = 0; i <= seg; ++i)
		{
			const float theta = (static_cast<float>(i) / static_cast<float>(seg)) * k2Pi;
			const float x = std::cos(theta) * radius;
			const float z = std::sin(theta) * radius;
			const glm::vec3 n = glm::normalize(glm::vec3(x, 0.0f, z));

			outMesh.Vertices.push_back({ x, -halfH, z });
			outMesh.Vertices.push_back({ x,  halfH, z });

			outMesh.Normals.push_back(n);
			outMesh.Normals.push_back(n);

			const float u = static_cast<float>(i) / static_cast<float>(seg);
			outMesh.UVs.push_back({ u, 0.0f });
			outMesh.UVs.push_back({ u, 1.0f });

			if (i > 0)
			{
				const uint32_t base = (i - 1) * 2;
				outMesh.Indices.push_back(base + 0);
				outMesh.Indices.push_back(base + 1);
				outMesh.Indices.push_back(base + 2);

				outMesh.Indices.push_back(base + 1);
				outMesh.Indices.push_back(base + 3);
				outMesh.Indices.push_back(base + 2);
			}
		}

		// 2. 上底面封盖 (+Y)
		const uint32_t topCenterIdx = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ 0.0f, halfH, 0.0f });
		outMesh.Normals.push_back({ 0.0f, 1.0f, 0.0f });
		outMesh.UVs.push_back({ 0.5f, 0.5f });

		const uint32_t topRimBase = static_cast<uint32_t>(outMesh.Vertices.size());
		for (uint32_t i = 0; i <= seg; ++i)
		{
			const float theta = (static_cast<float>(i) / static_cast<float>(seg)) * k2Pi;
			const float x = std::cos(theta) * radius;
			const float z = std::sin(theta) * radius;
			outMesh.Vertices.push_back({ x, halfH, z });
			outMesh.Normals.push_back({ 0.0f, 1.0f, 0.0f });
			outMesh.UVs.push_back({ 0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta) });
		}
		for (uint32_t i = 0; i < seg; ++i)
		{
			outMesh.Indices.push_back(topCenterIdx);
			outMesh.Indices.push_back(topRimBase + i + 1);
			outMesh.Indices.push_back(topRimBase + i);
		}

		// 3. 下底面封盖 (-Y)
		const uint32_t botCenterIdx = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ 0.0f, -halfH, 0.0f });
		outMesh.Normals.push_back({ 0.0f, -1.0f, 0.0f });
		outMesh.UVs.push_back({ 0.5f, 0.5f });

		const uint32_t botRimBase = static_cast<uint32_t>(outMesh.Vertices.size());
		for (uint32_t i = 0; i <= seg; ++i)
		{
			const float theta = (static_cast<float>(i) / static_cast<float>(seg)) * k2Pi;
			const float x = std::cos(theta) * radius;
			const float z = std::sin(theta) * radius;
			outMesh.Vertices.push_back({ x, -halfH, z });
			outMesh.Normals.push_back({ 0.0f, -1.0f, 0.0f });
			outMesh.UVs.push_back({ 0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta) });
		}
		for (uint32_t i = 0; i < seg; ++i)
		{
			outMesh.Indices.push_back(botCenterIdx);
			outMesh.Indices.push_back(botRimBase + i);
			outMesh.Indices.push_back(botRimBase + i + 1);
		}
	}

	void WhiteboxBuilder::BuildSphere(float radius, uint32_t segments, uint32_t rings, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const uint32_t seg = std::max(6u, segments);
		const uint32_t rng = std::max(4u, rings);
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float k2Pi = 6.28318530718f;

		for (uint32_t r = 0; r <= rng; ++r)
		{
			const float v = static_cast<float>(r) / static_cast<float>(rng);
			const float phi = v * kPi;
			const float sinPhi = std::sin(phi);
			const float cosPhi = std::cos(phi);

			for (uint32_t c = 0; c <= seg; ++c)
			{
				const float u = static_cast<float>(c) / static_cast<float>(seg);
				const float theta = u * k2Pi;
				const glm::vec3 n = glm::normalize(glm::vec3(sinPhi * std::cos(theta), cosPhi, sinPhi * std::sin(theta)));

				outMesh.Vertices.push_back(n * radius);
				outMesh.Normals.push_back(n);
				outMesh.UVs.push_back({ u, v });
			}
		}

		for (uint32_t r = 0; r < rng; ++r)
		{
			for (uint32_t c = 0; c < seg; ++c)
			{
				const uint32_t i0 = r * (seg + 1) + c;
				const uint32_t i1 = i0 + 1;
				const uint32_t i2 = (r + 1) * (seg + 1) + c;
				const uint32_t i3 = i2 + 1;

				outMesh.Indices.push_back(i0);
				outMesh.Indices.push_back(i2);
				outMesh.Indices.push_back(i1);

				outMesh.Indices.push_back(i1);
				outMesh.Indices.push_back(i2);
				outMesh.Indices.push_back(i3);
			}
		}
	}

	void WhiteboxBuilder::BuildStairs(const glm::vec3& dimensions, uint32_t steps, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const uint32_t nSteps = std::max(1u, steps);
		const float stepH = dimensions.y / static_cast<float>(nSteps);
		const float stepD = dimensions.z / static_cast<float>(nSteps);
		const float halfW = dimensions.x * 0.5f;
		const float halfH = dimensions.y * 0.5f;
		const float halfD = dimensions.z * 0.5f;

		for (uint32_t i = 0; i < nSteps; ++i)
		{
			const float y0 = -halfH + static_cast<float>(i) * stepH;
			const float y1 = y0 + stepH;
			const float z0 = -halfD + static_cast<float>(i) * stepD;
			const float z1 = z0 + stepD;

			// 踏面 (Tread, +Y, 法线严格垂直于 (0, 1, 0))
			const uint32_t baseT = static_cast<uint32_t>(outMesh.Vertices.size());
			outMesh.Vertices.push_back({ -halfW, y1, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z1 });
			outMesh.Vertices.push_back({ -halfW, y1, z1 });
			for (int k = 0; k < 4; ++k)
				outMesh.Normals.push_back({ 0, 1, 0 });
			outMesh.UVs.push_back({ 0, 0 });
			outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 });
			outMesh.UVs.push_back({ 0, 1 });
			outMesh.Indices.push_back(baseT + 0);
			outMesh.Indices.push_back(baseT + 2);
			outMesh.Indices.push_back(baseT + 1);
			outMesh.Indices.push_back(baseT + 0);
			outMesh.Indices.push_back(baseT + 3);
			outMesh.Indices.push_back(baseT + 2);

			// 踢面 (Riser, -Z, 法线严格垂直于 (0, 0, -1))
			const uint32_t baseR = static_cast<uint32_t>(outMesh.Vertices.size());
			outMesh.Vertices.push_back({ -halfW, y0, z0 });
			outMesh.Vertices.push_back({  halfW, y0, z0 });
			outMesh.Vertices.push_back({  halfW, y1, z0 });
			outMesh.Vertices.push_back({ -halfW, y1, z0 });
			for (int k = 0; k < 4; ++k)
				outMesh.Normals.push_back({ 0, 0, -1 });
			outMesh.UVs.push_back({ 0, 0 });
			outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 });
			outMesh.UVs.push_back({ 0, 1 });
			outMesh.Indices.push_back(baseR + 0);
			outMesh.Indices.push_back(baseR + 2);
			outMesh.Indices.push_back(baseR + 1);
			outMesh.Indices.push_back(baseR + 0);
			outMesh.Indices.push_back(baseR + 3);
			outMesh.Indices.push_back(baseR + 2);
		}
	}

	void WhiteboxBuilder::BuildWedge(const glm::vec3& dimensions, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const glm::vec3 h = dimensions * 0.5f;

		// 坡面法线严格垂直于倾斜方向 (0, dimensions.y, dimensions.z)
		const glm::vec3 slopeNormal = glm::normalize(glm::vec3(0.0f, dimensions.z, -dimensions.y));

		// 1. 底面矩形 (Bottom, -Y)
		const uint32_t b0 = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ -h.x, -h.y, -h.z });
		outMesh.Vertices.push_back({  h.x, -h.y, -h.z });
		outMesh.Vertices.push_back({  h.x, -h.y,  h.z });
		outMesh.Vertices.push_back({ -h.x, -h.y,  h.z });
		for (int k = 0; k < 4; ++k)
			outMesh.Normals.push_back({ 0.0f, -1.0f, 0.0f });
		outMesh.UVs.push_back({ 0, 0 });
		outMesh.UVs.push_back({ 1, 0 });
		outMesh.UVs.push_back({ 1, 1 });
		outMesh.UVs.push_back({ 0, 1 });
		outMesh.Indices.push_back(b0 + 0);
		outMesh.Indices.push_back(b0 + 2);
		outMesh.Indices.push_back(b0 + 1);
		outMesh.Indices.push_back(b0 + 0);
		outMesh.Indices.push_back(b0 + 3);
		outMesh.Indices.push_back(b0 + 2);

		// 2. 垂直后背面矩形 (Back, +Z)
		const uint32_t b1 = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ -h.x, -h.y, h.z });
		outMesh.Vertices.push_back({  h.x, -h.y, h.z });
		outMesh.Vertices.push_back({  h.x,  h.y, h.z });
		outMesh.Vertices.push_back({ -h.x,  h.y, h.z });
		for (int k = 0; k < 4; ++k)
			outMesh.Normals.push_back({ 0.0f, 0.0f, 1.0f });
		outMesh.UVs.push_back({ 0, 0 });
		outMesh.UVs.push_back({ 1, 0 });
		outMesh.UVs.push_back({ 1, 1 });
		outMesh.UVs.push_back({ 0, 1 });
		outMesh.Indices.push_back(b1 + 0);
		outMesh.Indices.push_back(b1 + 1);
		outMesh.Indices.push_back(b1 + 2);
		outMesh.Indices.push_back(b1 + 0);
		outMesh.Indices.push_back(b1 + 2);
		outMesh.Indices.push_back(b1 + 3);

		// 3. 倾斜坡面矩形 (Slope, 朝向前上方)
		const uint32_t b2 = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ -h.x, -h.y, -h.z }); // 底前左
		outMesh.Vertices.push_back({  h.x, -h.y, -h.z }); // 底前右
		outMesh.Vertices.push_back({  h.x,  h.y,  h.z }); // 顶后右
		outMesh.Vertices.push_back({ -h.x,  h.y,  h.z }); // 顶后左
		for (int k = 0; k < 4; ++k)
			outMesh.Normals.push_back(slopeNormal);
		outMesh.UVs.push_back({ 0, 0 });
		outMesh.UVs.push_back({ 1, 0 });
		outMesh.UVs.push_back({ 1, 1 });
		outMesh.UVs.push_back({ 0, 1 });
		outMesh.Indices.push_back(b2 + 0);
		outMesh.Indices.push_back(b2 + 2);
		outMesh.Indices.push_back(b2 + 1);
		outMesh.Indices.push_back(b2 + 0);
		outMesh.Indices.push_back(b2 + 3);
		outMesh.Indices.push_back(b2 + 2);

		// 4. 左侧直角三角形 (-X)
		const uint32_t b3 = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({ -h.x, -h.y, -h.z });
		outMesh.Vertices.push_back({ -h.x,  h.y,  h.z });
		outMesh.Vertices.push_back({ -h.x, -h.y,  h.z });
		for (int k = 0; k < 3; ++k)
			outMesh.Normals.push_back({ -1.0f, 0.0f, 0.0f });
		outMesh.UVs.push_back({ 0, 0 });
		outMesh.UVs.push_back({ 1, 1 });
		outMesh.UVs.push_back({ 1, 0 });
		outMesh.Indices.push_back(b3 + 0);
		outMesh.Indices.push_back(b3 + 1);
		outMesh.Indices.push_back(b3 + 2);

		// 5. 右侧直角三角形 (+X)
		const uint32_t b4 = static_cast<uint32_t>(outMesh.Vertices.size());
		outMesh.Vertices.push_back({  h.x, -h.y, -h.z });
		outMesh.Vertices.push_back({  h.x, -h.y,  h.z });
		outMesh.Vertices.push_back({  h.x,  h.y,  h.z });
		for (int k = 0; k < 3; ++k)
			outMesh.Normals.push_back({ 1.0f, 0.0f, 0.0f });
		outMesh.UVs.push_back({ 0, 0 });
		outMesh.UVs.push_back({ 1, 0 });
		outMesh.UVs.push_back({ 1, 1 });
		outMesh.Indices.push_back(b4 + 0);
		outMesh.Indices.push_back(b4 + 1);
		outMesh.Indices.push_back(b4 + 2);
	}

	void WhiteboxBuilder::BuildArch(const glm::vec3& dimensions, float innerRadius, WhiteboxMesh& outMesh)
	{
		outMesh.Vertices.clear();
		outMesh.Normals.clear();
		outMesh.UVs.clear();
		outMesh.Indices.clear();

		const glm::vec3 h = dimensions * 0.5f;
		const float innerR = (innerRadius > 0.05f) ? std::min(innerRadius, h.x * 0.85f) : h.x * 0.5f;
		const float lintelHeight = std::max(0.2f, dimensions.y * 0.25f);
		const float lintelBottomY = h.y - lintelHeight;

		const auto addQuad = [&outMesh](
			const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
			const glm::vec3& normal)
		{
			const uint32_t base = static_cast<uint32_t>(outMesh.Vertices.size());
			outMesh.Vertices.push_back(p0);
			outMesh.Vertices.push_back(p1);
			outMesh.Vertices.push_back(p2);
			outMesh.Vertices.push_back(p3);
			for (int k = 0; k < 4; ++k)
				outMesh.Normals.push_back(normal);
			outMesh.UVs.push_back({ 0, 0 });
			outMesh.UVs.push_back({ 1, 0 });
			outMesh.UVs.push_back({ 1, 1 });
			outMesh.UVs.push_back({ 0, 1 });
			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 1);
			outMesh.Indices.push_back(base + 2);
			outMesh.Indices.push_back(base + 0);
			outMesh.Indices.push_back(base + 2);
			outMesh.Indices.push_back(base + 3);
		};

		// 顶外表面 (+Y)
		addQuad({ -h.x,  h.y,  h.z }, {  h.x,  h.y,  h.z }, {  h.x,  h.y, -h.z }, { -h.x,  h.y, -h.z }, { 0, 1, 0 });
		// 左外侧面 (-X)
		addQuad({ -h.x, -h.y, -h.z }, { -h.x, -h.y,  h.z }, { -h.x,  h.y,  h.z }, { -h.x,  h.y, -h.z }, { -1, 0, 0 });
		// 右外侧面 (+X)
		addQuad({  h.x, -h.y,  h.z }, {  h.x, -h.y, -h.z }, {  h.x,  h.y, -h.z }, {  h.x,  h.y,  h.z }, { 1, 0, 0 });
		// 左柱底面 (-Y)
		addQuad({ -h.x, -h.y, -h.z }, { -innerR, -h.y, -h.z }, { -innerR, -h.y,  h.z }, { -h.x, -h.y,  h.z }, { 0, -1, 0 });
		// 右柱底面 (-Y)
		addQuad({  innerR, -h.y, -h.z }, {  h.x, -h.y, -h.z }, {  h.x, -h.y,  h.z }, {  innerR, -h.y,  h.z }, { 0, -1, 0 });

		// 左柱内侧面 (+X)
		addQuad({ -innerR, -h.y,  h.z }, { -innerR, -h.y, -h.z }, { -innerR, lintelBottomY, -h.z }, { -innerR, lintelBottomY,  h.z }, { 1, 0, 0 });
		// 右柱内侧面 (-X)
		addQuad({  innerR, -h.y, -h.z }, {  innerR, -h.y,  h.z }, {  innerR, lintelBottomY,  h.z }, {  innerR, lintelBottomY, -h.z }, { -1, 0, 0 });
		// 拱门下腹底面 (-Y, 天花板)
		addQuad({ -innerR, lintelBottomY, -h.z }, {  innerR, lintelBottomY, -h.z }, {  innerR, lintelBottomY,  h.z }, { -innerR, lintelBottomY,  h.z }, { 0, -1, 0 });

		// 前立面 (+Z)
		addQuad({ -h.x, -h.y, h.z }, { -innerR, -h.y, h.z }, { -innerR, lintelBottomY, h.z }, { -h.x, lintelBottomY, h.z }, { 0, 0, 1 });
		addQuad({  innerR, -h.y, h.z }, {  h.x, -h.y, h.z }, {  h.x, lintelBottomY, h.z }, {  innerR, lintelBottomY, h.z }, { 0, 0, 1 });
		addQuad({ -h.x, lintelBottomY, h.z }, {  h.x, lintelBottomY, h.z }, {  h.x, h.y, h.z }, { -h.x, h.y, h.z }, { 0, 0, 1 });

		// 后立面 (-Z)
		addQuad({ -innerR, -h.y, -h.z }, { -h.x, -h.y, -h.z }, { -h.x, lintelBottomY, -h.z }, { -innerR, lintelBottomY, -h.z }, { 0, 0, -1 });
		addQuad({  h.x, -h.y, -h.z }, {  innerR, -h.y, -h.z }, {  innerR, lintelBottomY, -h.z }, {  h.x, lintelBottomY, -h.z }, { 0, 0, -1 });
		addQuad({  h.x, lintelBottomY, -h.z }, { -h.x, lintelBottomY, -h.z }, { -h.x, h.y, -h.z }, {  h.x, h.y, -h.z }, { 0, 0, -1 });
	}

	void WhiteboxBuilder::BuildMesh(const WhiteboxComponent& comp, WhiteboxMesh& outMesh)
	{
		switch (static_cast<WhiteboxComponent::ShapeType>(comp.Shape))
		{
		case WhiteboxComponent::ShapeType::Box:
			BuildBox(comp.Dimensions, comp.BevelSize, outMesh);
			break;
		case WhiteboxComponent::ShapeType::Cylinder:
			BuildCylinder(comp.Dimensions.x * 0.5f, comp.Dimensions.y, std::max(6u, static_cast<uint32_t>(comp.Subdivisions * 8)), outMesh);
			break;
		case WhiteboxComponent::ShapeType::Sphere:
			BuildSphere(comp.Dimensions.x * 0.5f, std::max(6u, static_cast<uint32_t>(comp.Subdivisions * 8)), std::max(4u, static_cast<uint32_t>(comp.Subdivisions * 4)), outMesh);
			break;
		case WhiteboxComponent::ShapeType::Stairs:
			BuildStairs(comp.Dimensions, std::max(1u, static_cast<uint32_t>(comp.Steps)), outMesh);
			break;
		case WhiteboxComponent::ShapeType::Wedge:
			BuildWedge(comp.Dimensions, outMesh);
			break;
		case WhiteboxComponent::ShapeType::Arch:
			BuildArch(comp.Dimensions, comp.Dimensions.x * 0.25f, outMesh);
			break;
		default:
			BuildBox(comp.Dimensions, comp.BevelSize, outMesh);
			break;
		}
	}
}
