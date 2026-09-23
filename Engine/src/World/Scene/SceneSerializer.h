#pragma once
#include "Scene.h"
#include <string>

namespace World
{
	class SceneSerializer
	{
	public:
		SceneSerializer(const Ref<Scene>& scene);
		bool Serialize(const std::string& filepath);
		bool Deserialize(const std::string& filepath);

		bool SerializeRuntime(const std::string& filepath);
		bool DeserializeRuntime(const std::string& filepath);

		const std::string& GetLastError() const { return m_LastError; }

	private:
		Ref<Scene> m_Scene;
		std::string m_LastError;
	};
}
