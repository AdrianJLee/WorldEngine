#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	struct WhiteboxMesh
	{
		std::vector<glm::vec3> Vertices;
		std::vector<glm::vec3> Normals;
		std::vector<glm::vec2> UVs;
		std::vector<uint32_t> Indices;
	};

	class WLD_API WhiteboxBuilder
	{
	public:
		static void BuildBox(const glm::vec3& dimensions, float bevelSize, WhiteboxMesh& outMesh);
		static void BuildCylinder(float radius, float height, uint32_t segments, WhiteboxMesh& outMesh);
		static void BuildStairs(const glm::vec3& dimensions, uint32_t steps, WhiteboxMesh& outMesh);
		static void BuildWedge(const glm::vec3& dimensions, WhiteboxMesh& outMesh);
		static void BuildArch(const glm::vec3& dimensions, float innerRadius, WhiteboxMesh& outMesh);
	};
}
