#pragma once

#include "World/Core/Timestep.h"
#include "World/Level/Core/ChunkIndex.h"
#include "World/Level/Core/DeltaState.h"
#include "World/Level/Runtime/ChunkStreamingCoordinator.h"
#include "World/Level/Streaming/SpatialGridDriver.h"

#include <filesystem>
#include <memory>
#include <string>

namespace World
{
	class Scene;
	class WorldContext;
}

namespace World::Level
{
	// 把"关卡的流送块"接到宿主:索引 → 空间驱动器 → 按需载入/激活/卸载。
	//
	// 分层意图:索引解决"发行包里没有目录遍历",驱动器解决"该流送谁",协调器解决
	// "怎么装/卸",Scene 只负责承载实体。宿主(Runtime/Play)只需每帧喂相机。
	//
	// 载入源两选一:块在内容根里(开发树)走文件系统;块在发行包(vpak)里走
	// `Vfs::Exists` + `Vfs::Read`(逻辑路径)。两条路都汇到
	// `StreamableChunk::LoadFromMemory` —— 格式解析只有一处。
	class WLD_API LevelStreamingRuntime
	{
	public:
		LevelStreamingRuntime();
		~LevelStreamingRuntime();

		// 初始化一个关卡的流送。
		//
		//   * `levelId`    —— 关卡目录名(`levels/<levelId>/`),也是 Vfs 逻辑路径的一段;
		//   * `physicalDir`—— 该关卡在磁盘上的目录(**可空**)。发行包里内容根不一定存在,
		//                     此时一律走 Vfs 逻辑路径 `levels/<levelId>/cooked/...`。
		//
		// 索引两处都不存在(未烘焙)⇒ 返回 false 且不改变状态 —— 调用方据此退回整包实例化。
		bool Initialize(WorldContext& context, const std::string& levelId,
			const std::filesystem::path& physicalDir, std::string* error = nullptr);

		bool IsReady() const;
		const std::string& GetLevelId() const;

		void SetScene(Scene* scene);
		void SetDeltaState(DeltaStateRegistry* deltaState);

		// 视距(米):进入 ViewDistance 加载,离开 UnloadDistance 卸载;两者之间保持现状
		// (滞后缓冲带,防边界抖动)。缺省取自项目清单 `streaming:`。
		void SetViewDistances(float viewDistance, float unloadDistance);
		// 主线程每帧用于实体实例化的时间片(毫秒)。实例化必须走主线程(写 ECS 注册表),
		// 这个上限是"掉帧"与"卡顿"之间的旋钮。
		void SetTimeBudgetMs(float budgetMs);
		// 真异步读盘:字节在 JobSystem 后台线程读,主线程只做实例化。
		// 关闭 = 同步读(小关卡省一个线程;大关卡换区时会顿)。
		void SetAsyncIO(bool enabled);

		// 宿主每帧调用(相机位置/视投影;引擎的 Demo/验证只用到位置)。
		void Update(Timestep frameTime, const glm::vec3& viewPosition, const glm::mat4& viewProjection);

		size_t TotalChunks() const;
		size_t LoadedChunks() const;
		size_t ActiveChunks() const;
		// 当前活跃块里承载的实体数(校验"块真的长出实体了",而不是只有块状态)。
		size_t ActiveEntityCount() const;

	// 后台预读:把"驱动器本帧裁决要装、但字节还没到手"的块排进 JobSystem。
	// 分离成公开方法是为了让 Update 保持"只做本帧该做的事",预读时机由宿主决定。
	size_t PendingPrefetches() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_Impl;
	};
}
