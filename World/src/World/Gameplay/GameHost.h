#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/GameApp.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Scene/Scene.h"

#include <memory>
#include <string>

namespace World
{
	// 无 Application 的宿主(单元测试/工具)需要自持上下文:LoadLevel 要创建 Scene。
	class WorldContext;
}

namespace World::Gameplay
{
	// 宿主粘合层(P2a §3.1):持有场景、把场景更新接进 GameApp 的阶段、并按需提交渲染。
	// Runtime、Game.dll 与编辑器的 Play/Simulate 都走这里,避免三份重复的宿主代码。
	//
	// W1 范围:场景按路径直接加载(关卡清单/异步加载属 W2);渲染器可注入(编辑器复用视口渲染器),
	// 未注入时自建。
	class WLD_API GameHost
	{
	public:
		GameHost();
		~GameHost();

		// 确保 GameApp 以 desc 存在(已存在则复用),并注册阶段回调。
		void Init(const GameAppDesc& desc);
		void Shutdown();

		// 渲染器注入:必须在 Init 之后、首次 Tick 之前调用(编辑器场景)。
		void SetRenderer(const Ref<SceneRenderer>& renderer);
		SceneRenderer* GetRenderer() const { return m_SceneRenderer ? m_SceneRenderer.get() : nullptr; }

		// W1:按路径加载场景并(默认)进入运行时。返回是否成功。
		bool LoadLevel(const std::string& scenePath, bool startRuntime = true);
		// 编辑器 Play/Simulate:注入运行时场景副本;startRuntime=false 用于 Simulate 语义。
		void SetScene(const Ref<Scene>& scene, bool startRuntime);

		// 宿主每帧入口:先跑 GameApp 阶段,再按需渲染(主相机实体缺失时跳过渲染)。
		void Tick(Timestep frameTime, bool render);

		void StartRuntime();
		void StopRuntime();
		bool IsRuntimeStarted() const { return m_RuntimeStarted; }

	Ref<Scene> GetScene() const { return m_Scene; }

	private:
		void SubmitSceneRender();
		// 把视口尺寸同步给场景相机:优先宿主窗口,其次注入的渲染器,最后用默认 1280x720。
		void ApplyViewportToScene();

		Ref<Scene> m_Scene;
		Ref<SceneRenderer> m_SceneRenderer;
		std::unique_ptr<WorldContext> m_OwnedContext;
		std::string m_LoadedPath;
		bool m_RuntimeStarted = false;
		bool m_Initialized = false;
		// 只销毁自己创建的会话:复用他人会话的宿主不应该把会话一起带走。
		bool m_CreatedSession = false;
	};
}
