#pragma once

#include "World/Core/Core.h"
#include "World/Level/Authoring/LevelPackage.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World
{
	class Scene;
	class WorldContext;

	// 编辑文档状态:持有编辑场景、路径、dirty 与最近错误。
	// 本类不依赖 UI 框架或文件对话框;UI 由 EditorLayer 负责。
	//
	// 文档有两种形态:
	//   · **关卡包目录**(`assets/levels/<名>/`,Level 2.0 OFPA)—— 首选形态。一个实体一个
	//     文件,保存时按 UUID 增量写回并剪掉已删除实体的文件;数据层归属随实体的
	//     LevelEntityComponent 一起落盘。
	//   · **单场景 `.wd`** —— 仍保留给 Prefab 与老内容;不作为关卡入口。
	class EditorDocument
	{
	public:
		explicit EditorDocument(WorldContext& context);

		const Ref<Scene>& GetScene() const { return m_Scene; }
		const std::filesystem::path& GetPath() const { return m_Path; }
		bool HasPath() const { return !m_Path.empty(); }
		bool IsDirty() const { return m_Dirty; }
		void MarkDirty() { m_Dirty = true; }
		const std::string& GetLastError() const { return m_LastError; }
		void ClearError() { m_LastError.clear(); }

		// 当前文档是否是 Level 2.0 关卡包(决定"关卡"相关面板/数据层开关是否可用)。
		bool IsLevelPackage() const { return !m_LevelPackagePath.empty(); }
		const std::vector<Level::DataLayerDefinition>& GetDataLayers() const { return m_DataLayers; }

		// 加载成功才替换当前场景/路径并清 dirty;失败保留原状态。
		bool LoadFromFile(const std::filesystem::path& path);
		// 保存成功才回写路径并清 dirty;失败保持原路径与 dirty。
		bool SaveTo(const std::filesystem::path& path);
		// 新场景:清路径与 dirty。
		void New();

	private:
		WorldContext* m_Context = nullptr;
		Ref<Scene> m_Scene;
		std::filesystem::path m_Path;
		bool m_Dirty = false;
		std::string m_LastError;

		// 关卡包形态的缓存:保留 manifest 的数据层定义与每个实体的 ChunkID,
		// 保存时按 UUID 对齐场景实体后写回(而不是把整包推倒重来)。
		std::filesystem::path m_LevelPackagePath;
		std::vector<Level::DataLayerDefinition> m_DataLayers;
	};
}
