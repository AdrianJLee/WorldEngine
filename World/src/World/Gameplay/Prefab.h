#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

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

	// W4-3:实例覆盖记录。
	// 由"改动发生处"登记(属性面板/脚本),而不是靠全量 diff 反推——这样覆盖信息永远与真实编辑一致。
	struct PrefabInstanceRecord
	{
		std::string PrefabPath;                 // 来源 prefab(空 = 非 prefab 实例)
		entt::entity Root = entt::null;         // 实例子树根
		// 实体 -> 被覆盖的字段名(如 "TransformComponent.Location");空集合表示该实体无覆盖。
		std::unordered_map<uint32_t, std::vector<std::string>> Overrides;

		bool IsValid() const { return Root != entt::null; }
	};

	WLD_API void MarkOverride(PrefabInstanceRecord& record, entt::entity entity, const std::string& field);
	WLD_API bool HasOverride(const PrefabInstanceRecord& record, entt::entity entity);
	WLD_API size_t GetOverrideCount(const PrefabInstanceRecord& record);
	WLD_API void ClearOverrides(PrefabInstanceRecord& record);

	// 回滚实例:重新实例化来源 prefab 到临时场景,按树序把组件值拷回实例实体
	// (契约:实例与 prefab 结构同构;整体回滚,字段级回滚需要 schema 字段访问,列入后续增量)。
	WLD_API bool RevertInstance(PrefabInstanceRecord& record, Scene& scene);
	// 断链(Unpack):把实例变成普通实体——解除 prefab 关联并清空覆盖记录。
	// 之后编辑不再被登记为覆盖,Revert/Apply 也不再可用(实体本身保持不变)。
	// 嵌套 prefab 的数据表达:实例记录本身可再引用其它 prefab(编辑器侧维护多份记录),
	// 因此断链只需清掉本层关联,不影响其子实例。
	WLD_API bool UnpackInstance(PrefabInstanceRecord& record);
	// 是否还能回滚(有来源且根仍然有效)。
	WLD_API bool CanRevert(const PrefabInstanceRecord& record, const Scene& scene);
}
