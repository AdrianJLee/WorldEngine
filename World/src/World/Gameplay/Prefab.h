#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdint>

namespace World::Gameplay
{
	struct PrefabInstanceResult
	{
		Entity Root;                  // 实例化出来的子树根(挂在 parent 下)
		uint32_t EntityCount = 0;     // 复制的实体数(含子树全部后代)
		bool IsValid() const { return Root.IsValid() && EntityCount > 0; }
	};

	// Prefab 核心(P2a W4):把一棵实体子树深拷贝到目标场景(可跨场景),并挂到指定父节点下。
	//
	// 契约:
	//  - 复制引擎内置组件值(Tag/Transform/Sprite/Circle/MeshRenderer/Camera/2D 物理);
	//  - UUID 重新生成:实例必须有独立身份,否则存档与引用会撞;
	//  - 层级按源结构重建(顺序保留),实例根挂到 parent 下;
	//  - 非法输入(源实体不在给定场景)返回无效结果,不抛异常。
	// 文件资产(.wprefab)读写、实例覆盖与嵌套/断链在 W4 后续增量接入。
	WLD_API PrefabInstanceResult Instantiate(const Scene& source, Entity sourceRoot,
		Scene& destination, entt::entity parent = entt::null);
}
