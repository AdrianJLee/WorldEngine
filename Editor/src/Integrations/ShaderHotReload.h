#pragma once

#include "World/Renderer/MaterialSurface.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace World::Editor
{
	// HOTR-P1-T1:编辑器级材质着色器(`.slang`)热重载服务。
	//
	// 动机:`MaterialLibrary::PollAssetChanges` 会把"材质引用的 `.slang` 内容变了"报进
	// `AssetHotReloadReport::ChangedShaders`,但在此之前没有消费方 —— 只有打开对应材质面板时,
	// 面板自己的单飞编译线程才会重编译 + Install。本服务让"面板没开"的路径也走同一条链路:
	// 改盘 → 后台线程重编译 → 主线程帧边界 Install(路径键,引用方材质已由 MaterialLibrary
	// 失效,渲染侧会换到新管线)。
	//
	// 线程纪律(与 MaterialEditorPanel 的后台编译同一口径):
	//  - Enqueue / Pump / Shutdown 只在**主线程**调用;
	//  - 编译器(slangc)只在工作线程跑;Install(建 GPU 管线)只在 Pump(主线程帧边界);
	//  - 每路径单飞:同路径的新请求覆盖旧的未启动请求,编译期间到来的新请求让旧产物作废(后来者胜);
	//  - 编译失败只记 WARN,**保留旧管线**(不 Install、不清理 MaterialSurfaceRuntime);
	//  - 绝不使用 `<路径>#preview` 键(那条键只属于打开中的材质面板的实时预览)。
	class ShaderHotReload
	{
	public:
		ShaderHotReload() = default;
		~ShaderHotReload();

		ShaderHotReload(const ShaderHotReload&) = delete;
		ShaderHotReload& operator=(const ShaderHotReload&) = delete;

		// 主线程:登记一份材质着色器的**逻辑路径**(相对内容根;与 ChangedShaders 的书写一致)。
		// 空路径 / 库文件(`shaders/lib/**`)被跳过;同路径重复调用 = 后来者胜。
		void Enqueue(const std::string& logicalPath);

		// 主线程帧边界:把后台编译成功的产物 Install 到路径键;没有产物时是 no-op。
		void Pump();

		// 停止并 join 工作线程(幂等;OnDetach 与析构都会调)。
		void Shutdown();

	private:
		struct CompileRequest
		{
			uint64_t Serial = 0;
			std::string LogicalPath;                       // 规范化后的内容根相对路径(日志/键同一形态)
			// 源文本在**主线程**读取后随请求交付(与 MaterialEditorPanel 的后台编译同口径:
			// 工作线程只跑编译器,不碰 VFS / 应用上下文 —— Vfs 没有线程安全承诺)。
			std::string Source;
			SurfaceShaderBackend Backend = SurfaceShaderBackend::VulkanSpirV;   // 起请求时的设备后端
			std::vector<std::filesystem::path> IncludeRoots;   // 主线程算好的绝对 include 根
		};

		struct CompileOutcome
		{
			uint64_t Serial = 0;
			std::string LogicalPath;
			bool Success = false;
			SurfaceArtifact Artifact;
			std::string Error;              // 失败原因(读取失败或第一条诊断);成功时为空
			double ElapsedMilliseconds = 0.0;
		};

		void WorkerLoop();

		std::thread m_Worker;
		std::mutex m_Mutex;
		std::condition_variable m_Cv;
		// 待编译(键 = 逻辑路径;Order 记录首次入队顺序,替换不入 Order)。
		std::unordered_map<std::string, CompileRequest> m_Pending;
		std::deque<std::string> m_PendingOrder;
		// 已编译待消费(新产物覆盖未消费的旧产物;键的先后顺序不变)。
		std::unordered_map<std::string, CompileOutcome> m_Ready;
		std::deque<std::string> m_ReadyOrder;
		uint64_t m_Serial = 0;
		bool m_Shutdown = false;
	};
}
