#pragma once
#include "World/Core/Timestep.h"
#include "World/Renderer/EditorCamera.h"

#include <box2d/id.h>
#include <entt.hpp>

namespace World
{
	class CommandBuffer;

	class Scene
	{
	public:
		Scene();
		~Scene();

		void OnUpdateEditor(Timestep ts, const EditorCamera& camera);
		void OnUpdateRuntime(Timestep ts);
		void OnUpdateSimulation(Timestep ts, const EditorCamera& camera);
		void OnViewportResize(uint32_t width, uint32_t height);

		void OnRuntimeStart();
		void OnRuntimeStop();

		void OnSimulationStart();
		void OnSimulationStop();

		void OnScriptStart();
		void OnScriptUpdate(Timestep ts);
		void OnScriptDestroy();

		class Entity GetPrimaryCameraEntity();
		void DuplicateEntity(Entity entity);
	public:

		static void CopyScene(Ref<Scene>& other, Ref<Scene>& newScene);

	private:
		void OnPhysics2DStart();
		void OnUpdatePhysics2D(Timestep ts);
		void OnPhysics2DStop();
	private:
		friend class Entity;
		friend class SceneRenderer;

		entt::registry m_Registry;

		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;

		friend class SceneHierarchyPanel;
		friend class SceneSerializer;

		b2WorldId m_PhysicsWorldId;

	};

}

