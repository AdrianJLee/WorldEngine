#pragma once

#include "World/Core/Core.h"

#include <filesystem>
#include <string>

namespace World
{
	class Scene;
	class WorldContext;

	// 编辑文档状态：持有编辑场景、路径、dirty 与最近错误。
	// 本类不依赖 ImGui 或文件对话框；UI 由 EditorLayer 负责。
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

		// 加载成功才替换当前场景/路径并清 dirty；失败保留原状态。
		bool LoadFromFile(const std::filesystem::path& path);
		// 保存成功才回写路径并清 dirty；失败保持原路径与 dirty。
		bool SaveTo(const std::filesystem::path& path);
		// 新场景：清路径与 dirty。
		void New();

	private:
		WorldContext* m_Context = nullptr;
		Ref<Scene> m_Scene;
		std::filesystem::path m_Path;
		bool m_Dirty = false;
		std::string m_LastError;
	};
}
