#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/FrustumCull.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace World
{
	struct OctreeElement
	{
		uint32_t EntityId { 0 };
		glm::vec3 BoundsMin { 0.0f };
		glm::vec3 BoundsMax { 0.0f };
	};

	class WLD_API SceneOctree
	{
	public:
		struct Node
		{
			glm::vec3 Center { 0.0f };
			float HalfSize { 0.0f };
			uint32_t Depth { 0 };
			std::unique_ptr<Node> Children[8];
			std::vector<OctreeElement> Elements;

			bool IsLeaf() const;
			glm::vec3 GetBoundsMin() const { return Center - glm::vec3(HalfSize); }
			glm::vec3 GetBoundsMax() const { return Center + glm::vec3(HalfSize); }
		};

		SceneOctree();
		~SceneOctree() = default;

		void Clear();

		void Build(const std::vector<OctreeElement>& elements,
			const glm::vec3& worldCenter, float halfSize,
			uint32_t maxDepth = 6, uint32_t maxElementsPerNode = 16);

		void Insert(const OctreeElement& elem);

		// 层级视锥剔除查询 (O(log N) 快速剪枝)
		void QueryFrustum(const FrustumPlanes& frustum, std::vector<uint32_t>& outEntities) const;

		// 射线相交测试候选集过滤
		void QueryRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist,
			std::vector<uint32_t>& outCandidates) const;

		// 球形范围近邻查询
		void QuerySphere(const glm::vec3& center, float radius, std::vector<uint32_t>& outEntities) const;

		size_t GetTotalNodeCount() const;
		size_t GetTotalElementCount() const;

	private:
		void SubdivideNode(Node* node);
		void InsertToNode(Node* node, const OctreeElement& elem);

		std::unique_ptr<Node> m_Root;
		uint32_t m_MaxDepth { 6 };
		uint32_t m_MaxElementsPerNode { 16 };
		size_t m_TotalElements { 0 };
	};
}

