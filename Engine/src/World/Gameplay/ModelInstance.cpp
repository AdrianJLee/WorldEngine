#include "wldpch.h"
#include "World/Gameplay/ModelInstance.h"

#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace World::Gameplay
{
	ModelInstanceResult InstantiateModelDetailed(const std::string& modelPath, Scene& scene,
		entt::entity parent, std::string* error)
	{
		ModelInstanceResult result;
		const Ref<Mesh> mesh = Mesh::LoadWModel(modelPath, error);
		if (!mesh)
			return result;

		entt::registry& registry = scene.GetRegistry();
		const std::vector<MeshNode>& nodes = mesh->GetNodes();

		// 纯网格资产(没有节点树):建一个实体整体引用 mesh 0,双击仍然有结果。
		if (nodes.empty())
		{
			const entt::entity handle = registry.create();
			registry.emplace_or_replace<UUIDComponent>(handle, UUID());
			registry.emplace_or_replace<TagComponent>(handle, mesh->GetDesc().DebugName);
			registry.emplace_or_replace<TransformComponent>(handle, TransformComponent());
			MeshRendererComponent renderer;
			renderer.MeshPath = modelPath;
			renderer.MeshIndex = 0;
			registry.emplace_or_replace<MeshRendererComponent>(handle, renderer);
			if (parent != entt::null && registry.valid(parent))
				Hierarchy::SetParent(registry, handle, parent);
			Hierarchy::UpdateWorldTransforms(registry);
			result.Root = Entity(&scene, handle);
			result.EntityCount = 1;
			return result;
		}

		// 建实体(与 Prefab::Instantiate 同一条直接注册表路径,避免活动场景的结构写断言;
		// 层级在第二遍统一连接,所以节点乱序也正确)。
		// 注意:MSVC 下 std::vector 的 (count, value) 构造会把 entt::null 过载到 allocator 构造函数,
		// 必须显式转换成 entt::entity。
		std::vector<entt::entity> handles(nodes.size(), static_cast<entt::entity>(entt::null));
		for (size_t index = 0; index < nodes.size(); ++index)
		{
			const MeshNode& node = nodes[index];
			const entt::entity handle = registry.create();
			handles[index] = handle;
			registry.emplace_or_replace<UUIDComponent>(handle, UUID());
			registry.emplace_or_replace<TagComponent>(handle,
				node.Name.empty() ? ("Node " + std::to_string(index)) : node.Name);

			TransformComponent transform;
			transform.Location = node.Translation;
			transform.RotationQuat = node.Rotation;
			transform.Rotation = glm::eulerAngles(node.Rotation);
			transform.Scale = node.Scale;
			transform.RecalculateTransform();
			registry.emplace_or_replace<TransformComponent>(handle, transform);

			if (node.MeshIndex >= 0)
			{
				MeshRendererComponent renderer;
				renderer.MeshPath = modelPath;
				renderer.MeshIndex = node.MeshIndex;
				registry.emplace_or_replace<MeshRendererComponent>(handle, renderer);
			}
		}

		for (size_t index = 0; index < nodes.size(); ++index)
		{
			const int32_t parentIndex = nodes[index].Parent;
			if (parentIndex < 0 || static_cast<size_t>(parentIndex) >= handles.size())
				continue;
			Hierarchy::SetParent(registry, handles[index], handles[static_cast<size_t>(parentIndex)]);
		}
		if (parent != entt::null && registry.valid(parent))
		{
			for (size_t index = 0; index < nodes.size(); ++index)
				if (nodes[index].Parent < 0)
					Hierarchy::SetParent(registry, handles[index], parent);
		}
		Hierarchy::UpdateWorldTransforms(registry);

		for (size_t index = 0; index < nodes.size(); ++index)
		{
			if (nodes[index].Parent < 0)
			{
				result.Root = Entity(&scene, handles[index]);
				break;
			}
		}
		if (!result.Root.IsValid())
			result.Root = Entity(&scene, handles.front());
		result.EntityCount = static_cast<uint32_t>(handles.size());
		return result;
	}

	uint32_t InstantiateModel(const std::string& modelPath, Scene& scene, entt::entity parent,
		std::string* error)
	{
		return InstantiateModelDetailed(modelPath, scene, parent, error).EntityCount;
	}
}
