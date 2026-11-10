#pragma once

#include "World/Core/Export.h"
#include "World/Core/Core.h"
#include "World/Level/Authoring/LevelPackage.h"
#include "World/Scene/Scene.h"

#include <map>
#include <memory>
#include <string>

namespace World
{
	class WorldContext;
}

namespace World::Ai
{
	// Headless 创作会话:把"AI 直接写 YAML"换成"操作真实 ECS + 真实关卡包"。
	//
	// 为什么需要它:嵌入式 headless MCP 只有流程命令,没有任何创作动词;编辑器虽然
	// 有 scene.* / material.*,但只有在 Editor 进程活着并且 --ai-control 连上时才可见。
	// 这个类是**引擎侧**的创作面,Editor 与本进程 MCP 都能用同一套语义。
	//
	// 事实源仍是 LevelPackage(.wlevel + levels/<Id>/entities/*.entity)。所有修改先落在
	// 真实 Scene 上,Save() 时经 CaptureFromScene 逐组件序列化回单实体文件 —— 与编辑器
	// 保存走同一条路径,不存在"AI 专用格式"。
	class WLD_API SceneAuthoring
	{
	public:
		static SceneAuthoring& Get();
		static void RegisterCommands(class CommandRegistry& reg);

		// path 依次尝试:绝对路径 → ProjectDir()/path → ProjectDir()/assets/path →
		// ProjectDir()/assets/scenes/path;无扩展名时补 .wlevel。成功返回 true。
		bool Open(const std::string& path, std::string* outError);
		bool CreateLevel(const std::string& path, const std::string& id, std::string* outError);
		bool Save(std::string* outError);
		void Close();

		bool IsOpen() const { return m_Scene != nullptr; }
		bool IsDirty() const { return m_Dirty; }
		const std::string& EntryPath() const { return m_EntryPath; }
		Scene* GetScene() const { return m_Scene.get(); }
		Level::LevelPackage* GetPackage() { return &m_Package; }

		void MarkDirty() { m_Dirty = true; }

		// 解析实体:handle=<uint32> / name=<Tag> / uuid=<uint64>。name 命中多个时报错,
		// 避免"改错那个同名实体"这种静默错误。
		Entity ResolveEntity(const std::map<std::string, std::string>& args, std::string* outError);

		std::string ResolveEntryPath(const std::string& path) const;

	private:
		SceneAuthoring() = default;

		std::unique_ptr<WorldContext> m_Context;
		Ref<Scene> m_Scene;
		Level::LevelPackage m_Package;
		std::string m_EntryPath;
		bool m_Dirty = false;
	};
}
