#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <string>

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

	// W4-2:.wprefab 资产读写。
	// 实现方式:用临时场景承载子树,复用①已验证的实例化内核②场景序列化器
	// (因此 prefab 文件与 .wd 同格式、同 schema 版本,读档路径也只有一条)。
	//  - SaveFromScene:把 source 中 root 的子树导出为 prefab 文件;
	//  - InstantiateFromFile:读入 prefab 并作为实例挂到 destination 的 parent 下。
	WLD_API bool SaveFromScene(Scene& source, Entity root,
		const std::filesystem::path& path, std::string* error = nullptr);
	WLD_API PrefabInstanceResult InstantiateFromFile(const std::filesystem::path& path,
		Scene& destination, entt::entity parent = entt::null);
}
