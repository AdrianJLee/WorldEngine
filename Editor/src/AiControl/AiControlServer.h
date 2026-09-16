#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>

namespace World::Editor
{
	// AI 控制通道:127.0.0.1 上的 JSON 行协议,把"用户能做的操作 / 能看到的状态"
	// 变成脚本可调用的命令(见 tools/codex/tasks/20260916-2200-ai-accessible-editor/plan.md)。
	//
	// 线程模型:
	//   * 监听/会话线程只做 socket 与解析,把命令塞进队列后**阻塞等结果**;
	//   * 命令在主线程(EditorLayer 每帧 Pump)执行 —— 与鼠标操作同一线程、同一顺序,
	//     因此 handler 里可以放心触碰编辑器/渲染器状态。
	// 协议(每行一个 JSON 对象,字段全部扁平):
	//   请求 {"seq":1,"cmd":"ui.invoke","id":"123456","value":"..."}
	//   响应 {"seq":1,"ok":true,"result":"..."} / {"seq":1,"ok":false,"error":"..."}
	class AiControlServer
	{
	public:
		using Args = std::map<std::string, std::string>;
		// 返回 true 表示命令已执行(结果写进 result);false 表示失败(原因写进 error)。
		using Handler = std::function<bool(const std::string& cmd, const Args& args,
			std::string& result, std::string& error)>;

		AiControlServer() = default;
		~AiControlServer();

		bool Start(uint16_t port, Handler handler);
		void Stop();
		bool Running() const { return m_Running; }
		uint16_t Port() const { return m_Port; }
		// 当前是否有等不到主线程处理的命令(供脚本等待注入被消费)。
		bool HasPending() const;

		// 主线程每帧调用:执行排队的命令并回送响应。
		void Pump();

	private:
		struct Request
		{
			uint64_t Seq = 0;
			std::string Command;
			Args Arguments;
			std::string Response;
			bool Done = false;
			std::mutex Mutex;
			std::condition_variable Cv;
		};

		void ListenLoop();
		void SessionLoop(void* clientSocket);

		Handler m_Handler;
		bool m_Running = false;
		uint16_t m_Port = 0;
		uintptr_t m_ListenSocket = static_cast<uintptr_t>(~0ull);
		std::thread m_ListenThread;
		mutable std::mutex m_QueueMutex;
		std::condition_variable m_QueueCv;
		std::deque<std::shared_ptr<Request>> m_Queue;
	};
}
