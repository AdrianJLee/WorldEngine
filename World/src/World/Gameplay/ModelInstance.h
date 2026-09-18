#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <string>

namespace World::Gameplay
{
	// P1b D5:按 .wmodel 的节点树实例化模型(内容浏览器双击模型的引擎侧入口)。
	struct ModelInstanceResult
	{
		Entity Root;                  // 第一个根节点实体(没有根节点时 = 唯一实体)
		uint32_t EntityCount = 0;     // 建出的实体数(= 节点数,或纯网格资产的 1)
		bool IsValid() const { return Root.IsValid() && EntityCount > 0; }
	};

	// 每个节点建一个实体:Tag = 节点名(空名用 "Node <i>")、Transform = 节点 TRS;
	// 引用 mesh 的节点挂 MeshRendererComponent{ MeshPath = modelPath, MeshIndex = 节点 mesh 下标 }。
	// 父节点先建、随后按节点下标连层级(glTF 允许节点乱序;环会被 .wmodel 解析拒绝)。
	// parent 有效时把所有根节点挂到它下面。
	// 返回建出的实体数;**0 = 失败**(读盘/解析错误写进 error)。
	WLD_API uint32_t InstantiateModel(const std::string& modelPath, Scene& scene,
		entt::entity parent = entt::null, std::string* error = nullptr);

	// 详细版本:额外给出根实体(选中/继续操作模型实例的调用方用)。
	WLD_API ModelInstanceResult InstantiateModelDetailed(const std::string& modelPath, Scene& scene,
		entt::entity parent = entt::null, std::string* error = nullptr);
}
