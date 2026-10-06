#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Scene/Scene.h"
// GameUI(M11):世界空间 UI 的宿主侧喂参 —— `UI::UiWorldPositionResolver`(默认解析器的类型)。
#include "World/UI/UiWorldProjector.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>

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
		// W2:按关卡清单(levels.welevel)加载;请求在下一帧 Tick 内推进,进度经 GetLastLevelProgress 可查。
		bool LoadLevelById(const std::string& levelId, bool additive = false);
		const LevelLoadProgress& GetLastLevelProgress() const { return m_LastLevelProgress; }
		// 编辑器 Play/Simulate:注入运行时场景副本;startRuntime=false 用于 Simulate 语义。
		void SetScene(const Ref<Scene>& scene, bool startRuntime);

		// 宿主每帧入口:先跑 GameApp 阶段,再按需渲染(主相机实体缺失时跳过渲染)。
		void Tick(Timestep frameTime, bool render);

		void StartRuntime();
		void StopRuntime();
		bool IsRuntimeStarted() const { return m_RuntimeStarted; }

		// GameUI(M9):UI 优先消费输入 —— 宿主在 `Tick` **之前**调用,true = 本帧 UI 吃掉了指针。
		// 语义(只影响指针,不改其它):
		//   * 鼠标键位一律按抬起 ⇒ 不产生 Pressed/Released 边沿;
		//   * 喂给 `InputSystem::Sample` 的 `scrollDelta` 传 {0,0}(UI 吃掉了滚轮,玩法不得滚相机);
		//   * 鼠标**位置照常**传(相机瞄准可用)。
		// **消费一次即复位**:`Tick` 读取后立即清回 false;宿主每帧设置,不设置 = 下一帧自动 false,
		// 捕获状态不会泄漏到后续帧。默认 false ⇒ 关掉 `.wui` 时行为与引入本接口前逐字节一致。
		void SetPointerCaptured(bool captured) { m_PointerCaptured = captured; }
		bool IsPointerCaptured() const { return m_PointerCaptured; }

		Ref<Scene> GetScene() const { return m_Scene; }

		// ---- GameUI(M11):世界空间 UI 的运行时喂参 ----
		// `UiHost::SetWorldCamera` 要的"当前主相机 ViewProjection + 渲染面尺寸":
		//   * 相机 = **本帧要提交渲染的那台主相机**(与 `SubmitSceneRender` 同一选相机口径:
		//     复用文件内的 `ResolvePrimaryCamera`,不在这里另写一套选法;本方法只读);
		//   * `outViewProjection` = 投影矩阵 * inverse(相机世界矩阵)(与 `SubmitSceneRender` 同式);
		//   * `outScreenSize` = 渲染面在屏幕上的显示尺寸,单位与 `UI::UiSurface::PhysicalSize`
		//     同口径(Runtime = 窗口,编辑器 Play = 视口面板场景矩形)—— 世界锚点的
		//     "物理像素 → 设计单位"换算必须与构建 `UiViewport` 时用的是同一份尺寸。
		// 没有场景 / 没有主相机 / 渲染面尺寸无效 ⇒ false(宿主据此 `SetWorldSpace(false)`,
		// 纯屏幕空间行为与 f5b7da1 一致)。
		bool GetMainCameraViewProjection(glm::mat4& outViewProjection, glm::vec2& outScreenSize) const;

		// 默认世界位置解析器(实体名 → 世界位置),可直接作为 `UI::UiWorldPositionResolver`:
		//   * 名字 = `World::TagComponent::Tag`(NameId 的池内文本),命中**第一个**同名实体;
		//   * 位置优先 `WorldTransformComponent::Matrix` 的平移列(层级实体用求解后的世界矩阵),
		//     缺该组件时回退 `TransformComponent::Location`;
		//   * 找不到同名实体 / 名字为空 / 没有场景 ⇒ false(节点本帧隐藏,**不**画在 (0,0))。
		// 实体名索引按 `Scene::CurrentWorldTick()` 缓存:**每 tick 只线性扫场景一次**,之后每次
		// 解析是 O(1) 查表(不是每帧 O(n·k));场景指针或 tick 变化即整体重建。
		bool ResolveWorldPositionByName(std::string_view target, glm::vec3& outWorld) const;

		// 覆盖默认解析器(项目可换成脚本/自定义来源)。默认 = 上面的实体名解析。
		// 宿主应**只在场景/相机变化时**把它交给 `UiHost`,不要每帧重建 `std::function`。
		void SetWorldPositionResolver(UI::UiWorldPositionResolver resolver)
		{
			m_WorldPositionResolver = std::move(resolver);
		}
		const UI::UiWorldPositionResolver& GetWorldPositionResolver() const { return m_WorldPositionResolver; }

		// ---- T5c:异步资产加载 ----
		// 后台反序列化在**帧首**提交。不能只依赖 SceneRenderer::BeginScene:加载未完成时宿主会
		// 跳过场景渲染,提交点若寄生在渲染里就会死锁(不渲染 ⇒ 不提交 ⇒ 永远 pending)。
		void PumpAssetLoads();
		// 还在飞的资产数:>0 = 关卡仍在流式加载(宿主据此画加载界面 / 跳过场景渲染)。
		std::size_t PendingAssetLoads() const;
		// 进度摘要("loaded=N pending=M failed=K")。
		std::string DescribeAssetLoads() const;

	private:
		void SubmitSceneRender();
		// GameUI(M11):换场景 / 关会话时作废实体名索引(下一 tick 首次解析时重建),避免
		// "新场景恰好分配到同一地址、tick 又相同"时命中旧索引。
		void InvalidateWorldNameIndex() const;
		// W2:注入 SceneLoader + 进度回调,并尝试加载 levels.welevel(可选)。
		void InstallLevelServices(const GameAppDesc& desc);
		// 把视口尺寸同步给场景相机:优先宿主窗口,其次注入的渲染器,最后用默认 1280x720。
		void ApplyViewportToScene();

		Ref<Scene> m_Scene;
		Ref<SceneRenderer> m_SceneRenderer;
		LevelLoadProgress m_LastLevelProgress;
		std::unique_ptr<WorldContext> m_OwnedContext;
		std::string m_LoadedPath;
		bool m_RuntimeStarted = false;
		bool m_Initialized = false;
		// D5c-4a:本帧秒数(骨骼动画步长;SubmitSceneRender 在 Tick 之后调用)。
		float m_LastTickSeconds = 0.0f;
		// 只销毁自己创建的会话:复用他人会话的宿主不应该把会话一起带走。
		bool m_CreatedSession = false;
		// M9:本帧 UI 是否吃掉了指针(宿主每帧设置,Tick 消费一次即复位)。
		bool m_PointerCaptured = false;
		// GameUI(M11):世界空间 UI 的默认位置解析器(默认 = 按实体名的 `ResolveWorldPositionByName`)
		// 与它的实体名索引缓存。缓存按 (场景指针, CurrentWorldTick) 失效;键指向 StringPool 的
		// 稳定文本存储(deque,插入不失效),查表不产生每次解析的字符串分配。
		UI::UiWorldPositionResolver m_WorldPositionResolver;
		mutable std::unordered_map<std::string_view, entt::entity> m_WorldNameIndex;
		mutable const Scene* m_WorldNameIndexScene = nullptr;
		mutable uint64_t m_WorldNameIndexTick = 0;
		mutable bool m_WorldNameIndexValid = false;
	};
}
