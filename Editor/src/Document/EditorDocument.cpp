#include "EditorDocument.h"

#include "World/Core/Log.h"
#include "World/Scene/SceneSerializer.h"

namespace World
{
	EditorDocument::EditorDocument(WorldContext& context)
		: m_Context(&context), m_Scene(CreateRef<Scene>(context))
	{
	}

	bool EditorDocument::LoadFromFile(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot load scene: empty path.";
			return false;
		}

		Ref<Scene> newScene = CreateRef<Scene>(*m_Context);
		SceneSerializer serializer(newScene);
		if (!serializer.Deserialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to load scene: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Scene = newScene;
		m_Path = path;
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	bool EditorDocument::SaveTo(const std::filesystem::path& path)
	{
		if (path.empty())
		{
			m_LastError = "Cannot save scene: empty path.";
			return false;
		}

		SceneSerializer serializer(m_Scene);
		if (!serializer.Serialize(path.string()))
		{
			m_LastError = serializer.GetLastError().empty()
				? "Failed to save scene: " + path.string()
				: serializer.GetLastError();
			return false;
		}

		m_Path = path;
		m_Dirty = false;
		m_LastError.clear();
		return true;
	}

	void EditorDocument::New()
	{
		m_Scene = CreateRef<Scene>(*m_Context);
		m_Path.clear();
		m_Dirty = false;
		m_LastError.clear();
	}
}
