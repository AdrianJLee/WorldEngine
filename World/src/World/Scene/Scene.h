#pragma once
#include "World/Core/Timestep.h"
#include "World/Core/UUID.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/EditorCamera.h"
#include <box2d/id.h>
#include <entt.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace World
{
	class Entity;
	enum class SceneState { Stopped, Starting, Running, Stopping };

	class Scene
	{
	public:
		explicit Scene(WorldContext& context);
		~Scene();
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

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

		bool IsActive() const { return m_State != SceneState::Stopped; }
		bool IsRunning() const { return m_State == SceneState::Running; }
		bool IsPendingDestroy(entt::entity entity) const;
		// Accepted commands execute once at a safe point; callbacks enqueue the next batch.
		bool DeferStructuralChange(std::function<void(Scene&)> command);
		void FlushStructuralChanges();
		void AssertOwnerThread() const;

		Entity GetPrimaryCameraEntity();
		WorldContext& GetContext() { return *m_Context; }
		const WorldContext& GetContext() const { return *m_Context; }
		void DuplicateEntity(Entity entity);
		entt::registry& GetRegistry();
		const entt::registry& GetRegistry() const;
		static void CopyScene(Ref<Scene>& other, Ref<Scene>& newScene);

	private:
		friend class Entity;
		friend class SceneRenderer;
		friend class SceneHierarchyPanel;
		friend class SceneSerializer;

		struct ScriptSource
		{
			entt::entity EntityHandle = entt::null;
			entt::id_type Component = 0;
			uint64_t Generation = 0;
			bool Destroying = false;
		};
		enum class ChangeKind { General, DestroyEntity, RemoveComponent };
		struct StructuralChange
		{
			ChangeKind Kind = ChangeKind::General;
			std::function<void(Scene&)> Command;
			ScriptSource Source;
			entt::entity Target = entt::null;
			entt::id_type Component = 0;
		};

		void AssertStructuralWrite() const;
		bool IsPendingRemoval(entt::entity entity, entt::id_type component) const;
		bool IsSourceAlive(const ScriptSource& source) const;
		void RequestDestroy(entt::entity entity);
		void RequestRemove(entt::entity entity, entt::id_type component);
		void DestroyEntityNow(entt::entity entity);
		void RemoveComponentNow(entt::entity entity, entt::id_type component);
		void InvokeCallback(const ScriptSource& source, const std::function<void()>& callback);
		void StartPendingScripts();
		void UpdateScriptSnapshot(Timestep ts, const std::vector<entt::entity>& native, const std::vector<entt::entity>& lua);
		void DestroyNativeScript(entt::entity entity, bool faulted = false);
		void DestroyLuaScript(entt::entity entity, bool faulted = false);
		void FaultSource(const ScriptSource& source, const std::string& error);
		void StopScene();
		void OnPhysics2DStart();
		void OnUpdatePhysics2D(Timestep ts);
		void OnPhysics2DStop();
		void DestroyPhysicsBody(entt::entity entity);

		entt::registry m_Registry;
		WorldContext* m_Context = nullptr;
		std::shared_ptr<const uint8_t> m_Lifetime = std::make_shared<const uint8_t>(0);
		std::thread::id m_OwnerThread;
		SceneState m_State = SceneState::Stopped;
		bool m_StopRequested = false;
		bool m_Committing = false;
		unsigned m_CallbackDepth = 0;
		uint64_t m_NextGeneration = 0;
		ScriptSource m_CallbackSource;
		std::vector<StructuralChange> m_Changes;
		std::unordered_set<entt::entity> m_PendingDestroy;
		std::unordered_map<entt::entity, std::unordered_set<entt::id_type>> m_PendingRemove;
		uint32_t m_ViewportWidth = 0, m_ViewportHeight = 0;
		b2WorldId m_PhysicsWorldId = b2_nullWorldId;
		// 上次反序列化时未能识别的组件节点（按实体 UUID 保存原始 YAML 片段），供保存时回写，避免缺插件静默丢数据。
		std::unordered_map<UUID, std::string> m_UnknownComponentNodes;
	};
}
