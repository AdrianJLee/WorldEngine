#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/LevelList.h"
#include "World/Scene/Scene.h"

#include <functional>
#include <string>
#include <vector>

namespace World::Gameplay
{
	enum class LevelLoadState : uint8_t
	{
		Idle = 0,
		Reading,        // 已接受请求,等待宿主 Pump(下一帧开始真正读盘/反序列化)
		Deserializing,
		Activating,     // 场景已就绪,正在替换/压栈
		Failed,
	};

	const char* LevelLoadStateName(LevelLoadState state);

	struct LevelLoadProgress
	{
		std::string LevelId;
		LevelLoadState State = LevelLoadState::Idle;
		float Progress = 0.0f;      // 0..1,供加载界面使用
		std::string Error;
	};

	// 关卡服务(P2a W2):负责"加载/卸载/场景栈"的流程与状态机,不负责 Scene 的构造方式。
	// 场景构造由宿主注入的 SceneLoader 完成(默认实现走 SceneSerializer + VFS),
	// 这样服务本身可以脱离设备/窗口单测,也把"谁拥有 WorldContext"留给宿主。
	//
	// 线程约定:主线程对象。RequestLoad 只登记请求,真正的加载在宿主调用 Pump() 时发生,
	// 因此"异步"体现为跨帧的加载状态(不阻塞请求帧),不会在工作线程上碰 Scene/注册表。
	class WLD_API LevelService
	{
	public:
		// 返回 nullptr 表示失败,error 需填写原因。
		using SceneLoader = std::function<Ref<Scene>(const std::string& scenePath, std::string* error)>;
		using ProgressCallback = std::function<void(const LevelLoadProgress&)>;

		void SetLevelList(LevelList list);
		const LevelList& GetLevelList() const { return m_LevelList; }

		void SetSceneLoader(SceneLoader loader) { m_Loader = std::move(loader); }
		void SetProgressCallback(ProgressCallback callback) { m_ProgressCallback = std::move(callback); }

		// 请求加载关卡:未知 id / 未设置 loader / 已在加载同一关卡时返回 false。
		bool RequestLoad(const std::string& levelId, bool additive = false);
		// 宿主每帧调用:推进排队的加载请求(读盘 → 反序列化 → 激活)并派发进度回调。
		void Pump();

		void Unload(const std::string& levelId);
		void UnloadAll();

		bool IsLoading() const { return m_State != LevelLoadState::Idle && m_State != LevelLoadState::Failed; }
		LevelLoadState GetState() const { return m_State; }
		const std::string& GetPendingLevelId() const { return m_PendingLevelId; }
		const std::string& GetLastError() const { return m_LastError; }

		// 场景栈:索引 0 为主关卡,其后为附加层(UI/过场)。
		const std::vector<std::string>& GetActiveLevels() const { return m_ActiveLevelIds; }
		const std::string& GetPrimaryLevel() const;
		Ref<Scene> GetPrimaryScene() const;
		Ref<Scene> FindScene(const std::string& levelId) const;

	private:
		struct ActiveLevel
		{
			std::string Id;
			Ref<Scene> Scene_;
		};

		bool IsActive(const std::string& levelId) const;
		void Emit(LevelLoadState state, const std::string& levelId, float progress, const std::string& error = {});

		LevelList m_LevelList;
		SceneLoader m_Loader;
		ProgressCallback m_ProgressCallback;

		std::string m_PendingLevelId;
		bool m_PendingAdditive = false;
		LevelLoadState m_State = LevelLoadState::Idle;
		std::string m_LastError;

		std::vector<ActiveLevel> m_ActiveLevels;
		std::vector<std::string> m_ActiveLevelIds;
	};
}
