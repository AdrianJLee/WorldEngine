#pragma once

#include <cstdint>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>

namespace World::Level
{
	// Level 2.0(OFPA)单实体文件。
	//
	// **权威载荷是 `DocumentYaml`** —— 一个单实体场景文档(`Entities: [ {...} ]`),
	// 与 `.wd` 里实体块逐字段同源(读写都走 SceneSerializer),所以实体上的**全部组件**
	// (MeshRenderer / Camera / Light / 脚本…)都随文件走,不只是 Transform。
	// 这正是"一个实体一个文件、Git 逐行 diff、多人零冲突"能成立的前提:
	// 改一个物体的一个字段 = 只动它自己那一个小文件。
	//
	// Position / RotationEuler / Scale / Name 是**派生视图**(从文档解析而来,用于空间切分
	// 与二进制快速记录),不落盘;文档是唯一事实源,改文档后由 RefreshDerivedFromDocument() 重算。
	struct EntityDescriptor
	{
		uint64_t EntityUUID{ 0 };
		uint64_t ChunkID{ 0 };
		uint32_t DataLayerMask{ 1 };
		std::string DocumentYaml;

		glm::vec3 Position{ 0.0f };
		glm::vec3 RotationEuler{ 0.0f };
		glm::vec3 Scale{ 1.0f };
		std::string Name;

		// 从 DocumentYaml 重算派生视图(EntityUUID / Name / Position / RotationEuler / Scale)。
		void RefreshDerivedFromDocument();

		bool Save(const std::filesystem::path& filepath, std::string* outError = nullptr) const;
		bool Load(const std::filesystem::path& filepath, std::string* outError = nullptr);
	};
}
