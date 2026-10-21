#include "wldpch.h"
#include "World/Scene/SceneOctree.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		bool RayAABB(const glm::vec3& origin, const glm::vec3& dir, float maxDist,
			const glm::vec3& bMin, const glm::vec3& bMax)
		{
			float tmin = 0.0f;
			float tmax = maxDist;

			for (int i = 0; i < 3; ++i)
			{
				if (std::abs(dir[i]) < 1e-6f)
				{
					if (origin[i] < bMin[i] || origin[i] > bMax[i])
						return false;
				}
				else
				{
					const float invD = 1.0f / dir[i];
					float t0 = (bMin[i] - origin[i]) * invD;
					float t1 = (bMax[i] - origin[i]) * invD;
					if (t0 > t1)
						std::swap(t0, t1);
					tmin = std::max(tmin, t0);
					tmax = std::min(tmax, t1);
					if (tmax < tmin)
						return false;
				}
			}
			return true;
		}

		bool SphereAABB(const glm::vec3& center, float radius,
			const glm::vec3& bMin, const glm::vec3& bMax)
		{
			float d2 = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				if (center[i] < bMin[i])
				{
					const float d = bMin[i] - center[i];
					d2 += d * d;
				}
				else if (center[i] > bMax[i])
				{
					const float d = center[i] - bMax[i];
					d2 += d * d;
				}
			}
			return d2 <= (radius * radius);
		}

		bool AabbOverlap(const glm::vec3& aMin, const glm::vec3& aMax,
			const glm::vec3& bMin, const glm::vec3& bMax)
		{
			return (aMin.x <= bMax.x && aMax.x >= bMin.x) &&
			       (aMin.y <= bMax.y && aMax.y >= bMin.y) &&
			       (aMin.z <= bMax.z && aMax.z >= bMin.z);
		}
	}

	bool SceneOctree::Node::IsLeaf() const
	{
		return (Children[0] == nullptr);
	}

	SceneOctree::SceneOctree()
	{
	}

	void SceneOctree::Clear()
	{
		m_Root.reset();
		m_TotalElements = 0;
	}

	void SceneOctree::Build(const std::vector<OctreeElement>& elements,
		const glm::vec3& worldCenter, float halfSize,
		uint32_t maxDepth, uint32_t maxElementsPerNode)
	{
		Clear();
		m_MaxDepth = maxDepth;
		m_MaxElementsPerNode = maxElementsPerNode;

		m_Root = std::make_unique<Node>();
		m_Root->Center = worldCenter;
		m_Root->HalfSize = halfSize;
		m_Root->Depth = 0;

		for (const auto& elem : elements)
		{
			Insert(elem);
		}
	}

	void SceneOctree::SubdivideNode(Node* node)
	{
		const float childHalf = node->HalfSize * 0.5f;
		const uint32_t nextDepth = node->Depth + 1;

		int idx = 0;
		for (float z : { -1.0f, 1.0f })
		{
			for (float y : { -1.0f, 1.0f })
			{
				for (float x : { -1.0f, 1.0f })
				{
					auto child = std::make_unique<Node>();
					child->Center = node->Center + glm::vec3(x, y, z) * childHalf;
					child->HalfSize = childHalf;
					child->Depth = nextDepth;
					node->Children[idx++] = std::move(child);
				}
			}
		}

		// 将原有元素重新分发给子节点
		std::vector<OctreeElement> oldElements = std::move(node->Elements);
		node->Elements.clear();

		for (const auto& elem : oldElements)
		{
			InsertToNode(node, elem);
		}
	}

	void SceneOctree::InsertToNode(Node* node, const OctreeElement& elem)
	{
		if (node->IsLeaf())
		{
			if (node->Elements.size() < m_MaxElementsPerNode || node->Depth >= m_MaxDepth)
			{
				node->Elements.push_back(elem);
				return;
			}
			SubdivideNode(node);
		}

		// 派发到相交的子节点
		bool inserted = false;
		for (int i = 0; i < 8; ++i)
		{
			if (node->Children[i] && AabbOverlap(elem.BoundsMin, elem.BoundsMax,
				node->Children[i]->GetBoundsMin(), node->Children[i]->GetBoundsMax()))
			{
				InsertToNode(node->Children[i].get(), elem);
				inserted = true;
			}
		}

		if (!inserted)
		{
			node->Elements.push_back(elem);
		}
	}

	void SceneOctree::Insert(const OctreeElement& elem)
	{
		if (!m_Root)
		{
			const glm::vec3 center = (elem.BoundsMin + elem.BoundsMax) * 0.5f;
			const float halfSize = std::max({
				elem.BoundsMax.x - elem.BoundsMin.x,
				elem.BoundsMax.y - elem.BoundsMin.y,
				elem.BoundsMax.z - elem.BoundsMin.z,
				100.0f
			});
			m_Root = std::make_unique<Node>();
			m_Root->Center = center;
			m_Root->HalfSize = halfSize;
			m_Root->Depth = 0;
		}

		InsertToNode(m_Root.get(), elem);
		++m_TotalElements;
	}

	void SceneOctree::QueryFrustum(const FrustumPlanes& frustum, std::vector<uint32_t>& outEntities) const
	{
		if (!m_Root)
			return;

		std::vector<const Node*> stack = { m_Root.get() };
		while (!stack.empty())
		{
			const Node* curr = stack.back();
			stack.pop_back();

			if (!AabbInFrustum(frustum, curr->GetBoundsMin(), curr->GetBoundsMax()))
				continue;

			for (const auto& elem : curr->Elements)
			{
				if (AabbInFrustum(frustum, elem.BoundsMin, elem.BoundsMax))
				{
					outEntities.push_back(elem.EntityId);
				}
			}

			if (!curr->IsLeaf())
			{
				for (int i = 0; i < 8; ++i)
				{
					if (curr->Children[i])
						stack.push_back(curr->Children[i].get());
				}
			}
		}
	}

	void SceneOctree::QueryRay(const glm::vec3& origin, const glm::vec3& dir, float maxDist,
		std::vector<uint32_t>& outCandidates) const
	{
		if (!m_Root)
			return;

		std::vector<const Node*> stack = { m_Root.get() };
		while (!stack.empty())
		{
			const Node* curr = stack.back();
			stack.pop_back();

			if (!RayAABB(origin, dir, maxDist, curr->GetBoundsMin(), curr->GetBoundsMax()))
				continue;

			for (const auto& elem : curr->Elements)
			{
				if (RayAABB(origin, dir, maxDist, elem.BoundsMin, elem.BoundsMax))
				{
					outCandidates.push_back(elem.EntityId);
				}
			}

			if (!curr->IsLeaf())
			{
				for (int i = 0; i < 8; ++i)
				{
					if (curr->Children[i])
						stack.push_back(curr->Children[i].get());
				}
			}
		}
	}

	void SceneOctree::QuerySphere(const glm::vec3& center, float radius, std::vector<uint32_t>& outEntities) const
	{
		if (!m_Root)
			return;

		std::vector<const Node*> stack = { m_Root.get() };
		while (!stack.empty())
		{
			const Node* curr = stack.back();
			stack.pop_back();

			if (!SphereAABB(center, radius, curr->GetBoundsMin(), curr->GetBoundsMax()))
				continue;

			for (const auto& elem : curr->Elements)
			{
				if (SphereAABB(center, radius, elem.BoundsMin, elem.BoundsMax))
				{
					outEntities.push_back(elem.EntityId);
				}
			}

			if (!curr->IsLeaf())
			{
				for (int i = 0; i < 8; ++i)
				{
					if (curr->Children[i])
						stack.push_back(curr->Children[i].get());
				}
			}
		}
	}

	size_t SceneOctree::GetTotalNodeCount() const
	{
		if (!m_Root)
			return 0;

		size_t count = 0;
		std::vector<const Node*> stack = { m_Root.get() };
		while (!stack.empty())
		{
			const Node* curr = stack.back();
			stack.pop_back();
			++count;
			if (!curr->IsLeaf())
			{
				for (int i = 0; i < 8; ++i)
					if (curr->Children[i])
						stack.push_back(curr->Children[i].get());
			}
		}
		return count;
	}

	size_t SceneOctree::GetTotalElementCount() const
	{
		return m_TotalElements;
	}
}
