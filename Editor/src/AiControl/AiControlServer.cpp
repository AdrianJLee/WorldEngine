// winsock2 必须在 Windows.h(经 wldpch.h 间接引入)之前包含,否则类型重定义。
#include <winsock2.h>
#include <ws2tcpip.h>

#include "wldpch.h"
#include "AiControlServer.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace World::Editor
{
	namespace
	{
		void EnsureWinsock()
		{
			static bool initialized = false;
			if (initialized)
				return;
			WSADATA data {};
			WSAStartup(MAKEWORD(2, 2), &data);
			initialized = true;
		}

		// 会话/监听线程检查关机标志的时间片(见 ListenLoop/SessionLoop 的说明)。
		constexpr long kPollSliceMicroseconds = 100 * 1000;

		// 可选取证钩子:`WLD_AI_TRACE=<文件路径>` 时把控制通道的生命周期逐行 flush 追加写入
		// 该文件。探针通常把编辑器 stdout 重定向到 NUL,这是"Stop 没被调用 / join 卡住 /
		// 正常退出"三者之间唯一可靠的证据通道;不设该环境变量时不产生任何输出。
		void AiTrace(const char* format, ...)
		{
			static std::FILE* sink = []() -> std::FILE*
			{
				const char* path = std::getenv("WLD_AI_TRACE");
				return (path && *path) ? std::fopen(path, "a") : nullptr;
			}();
			if (!sink)
				return;
			static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
			const double milliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - origin).count();
			std::fprintf(sink, "[ai-trace %9.1fms] ", milliseconds);
			va_list arguments;
			va_start(arguments, format);
			std::vfprintf(sink, format, arguments);
			va_end(arguments);
			std::fputc('\n', sink);
			std::fflush(sink);
		}

		// 一个时间片内等 socket 可读/可写:>0 = 就绪,0 = 超时,<0 = 套接字错误。
		int WaitSocket(const SOCKET socket, const bool readable, const bool writable)
		{
			fd_set readSet;
			fd_set writeSet;
			FD_ZERO(&readSet);
			FD_ZERO(&writeSet);
			if (readable)
				FD_SET(socket, &readSet);
			if (writable)
				FD_SET(socket, &writeSet);
			timeval timeout { 0, kPollSliceMicroseconds };
			return select(0, readable ? &readSet : nullptr, writable ? &writeSet : nullptr,
				nullptr, &timeout);
		}

		// 关机后仍允许把"已经算好的响应"送出去的时间片上限。需要这个宽限是因为主线程可能
		// 在会话线程送出 quit 的 "closing" 之前就走到 Stop();上限则保证 join 仍有上界。
		constexpr int kStoppingSendSlices = 2;

		// 分块发送,每块之前用时间片复查关机标志:客户端不读时也不会把会话线程(以及
		// Stop() 的 join)挂死在阻塞 send 上。
		bool SendAll(const SOCKET client, const std::atomic<bool>& running, const std::string& payload)
		{
			size_t sent = 0;
			int stoppingSlices = 0;
			while (sent < payload.size())
			{
				const bool stopping = !running.load();
				if (stopping && stoppingSlices >= kStoppingSendSlices)
					return false;
				const int writable = WaitSocket(client, false, true);
				if (stopping)
					++stoppingSlices;
				if (writable < 0)
					return false;
				if (writable == 0)
					continue;   // 时间片到:回到循环顶(关机时由 stoppingSlices 上限收口)
				const int written = send(client, payload.data() + sent,
					static_cast<int>(payload.size() - sent), 0);
				if (written <= 0)
					return false;
				sent += static_cast<size_t>(written);
			}
			return true;
		}

		// ---- 极简 JSON:只解析我们自己的**扁平**请求对象 ----
		// 支持 {"key":value,...},value 为字符串/数字/true/false/null;不支持嵌套。
		// 这样脚本侧仍然是标准 JSON,而引擎侧不需要引入完整 JSON 库。
		bool ParseFlatJsonImpl(const std::string& text, std::map<std::string, std::string>* out, std::string* error)
		{
			size_t i = 0;
			auto skipSpace = [&]() { while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i; };
			auto parseString = [&](std::string* value) -> bool
			{
				if (i >= text.size() || text[i] != '"')
					return false;
				++i;
				value->clear();
				while (i < text.size())
				{
					const char c = text[i++];
					if (c == '\\')
					{
						if (i >= text.size())
							return false;
						const char escaped = text[i++];
						switch (escaped)
						{
							case 'n': value->push_back('\n'); break;
							case 't': value->push_back('\t'); break;
							case 'r': value->push_back('\r'); break;
							case 'b': value->push_back('\b'); break;
							case 'f': value->push_back('\f'); break;
							case 'u':
							{
								if (i + 4 > text.size())
									return false;
								// 控制通道只用到 ASCII;非 ASCII 转义按 '?' 落库(不影响命令语义)。
								unsigned code = static_cast<unsigned>(std::strtoul(text.substr(i, 4).c_str(), nullptr, 16));
								i += 4;
								value->push_back(code < 0x80 ? static_cast<char>(code) : '?');
								break;
							}
							default: value->push_back(escaped); break;
						}
					}
					else if (c == '"')
						return true;
					else
						value->push_back(c);
				}
				return false;
			};

			skipSpace();
			if (i >= text.size() || text[i] != '{')
			{
				if (error)
					*error = "expected '{'";
				return false;
			}
			++i;
			skipSpace();
			if (i < text.size() && text[i] == '}')
				return true;
			while (i < text.size())
			{
				skipSpace();
				std::string key;
				if (!parseString(&key))
				{
					if (error)
						*error = "expected key string";
					return false;
				}
				skipSpace();
				if (i >= text.size() || text[i] != ':')
				{
					if (error)
						*error = "expected ':'";
					return false;
				}
				++i;
				skipSpace();
				std::string value;
				if (i < text.size() && text[i] == '"')
				{
					if (!parseString(&value))
					{
						if (error)
							*error = "bad string value";
						return false;
					}
				}
				else
				{
					const size_t start = i;
					while (i < text.size() && text[i] != ',' && text[i] != '}')
						++i;
					value = text.substr(start, i - start);
					while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
						value.pop_back();
				}
				(*out)[key] = value;
				skipSpace();
				if (i < text.size() && text[i] == ',')
				{
					++i;
					continue;
				}
				if (i < text.size() && text[i] == '}')
					return true;
				if (error)
					*error = "expected ',' or '}'";
				return false;
			}
			return true;
		}

		std::string EscapeJson(const std::string& text)
		{
			std::string escaped;
			escaped.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': escaped += "\\\""; break;
					case '\\': escaped += "\\\\"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\t': escaped += "\\t"; break;
					default: escaped += c; break;
				}
			}
			return escaped;
		}
	}

	AiControlServer::~AiControlServer()
	{
		Stop();
	}

	bool AiControlServer::ParseFlatJson(const std::string& text, Args* fields, std::string* error)
	{
		return ParseFlatJsonImpl(text, fields, error);
	}

	bool AiControlServer::Start(uint16_t port, Handler handler)
	{
		if (m_Running.load())
			return true;
		EnsureWinsock();
		m_Handler = std::move(handler);
		m_Port = port;

		const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener == INVALID_SOCKET)
		{
			WLD_CORE_ERROR("[ai] socket() failed: {0}", WSAGetLastError());
			return false;
		}
		sockaddr_in address {};
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		// 只监听回环:控制通道默认关闭,且永远不对外网开放。
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			WLD_CORE_ERROR("[ai] bind 127.0.0.1:{0} failed: {1}", port, WSAGetLastError());
			closesocket(listener);
			return false;
		}
		if (listen(listener, 4) != 0)
		{
			WLD_CORE_ERROR("[ai] listen failed: {0}", WSAGetLastError());
			closesocket(listener);
			return false;
		}
		m_ListenSocket = static_cast<uintptr_t>(listener);
		m_Running.store(true);
		m_ListenThread = std::thread(&AiControlServer::ListenLoop, this);
		WLD_CORE_INFO("[ai] control channel listening on 127.0.0.1:{0}", port);
		return true;
	}

	void AiControlServer::Stop()
	{
		if (!m_Running.exchange(false))
			return;
		AiTrace("Stop: entered");

		// 1) 唤醒队列里还没被主线程 Pump 消费的请求:否则会话线程会在 10s 超时里干等,
		//    Stop() 的 join 也就跟着等满(实测另一个挂点)。
		std::deque<std::shared_ptr<Request>> pending;
		{
			std::lock_guard<std::mutex> guard(m_QueueMutex);
			pending.swap(m_Queue);
		}
		for (const std::shared_ptr<Request>& request : pending)
		{
			{
				std::lock_guard<std::mutex> guard(request->Mutex);
				if (request->Done)
					continue;
				request->Done = true;
				request->Response = "{\"seq\":" + std::to_string(request->Seq)
					+ ",\"ok\":false,\"error\":\"server stopping\"}";
			}
			request->Cv.notify_all();
		}

		// 2) 等监听线程自己退出:它和会话线程用 select 的 100ms 时间片轮询 m_Running,
		//    因此一个时间片内必然回到循环顶看到 false 并返回。
		//    这里**不做任何跨线程 socket 操作** —— 实测(WinSock,本机):shutdown(client, SD_BOTH)
		//    返回 0 但不会唤醒另一线程里阻塞中的 recv(),只有 closesocket 才会;而 closesocket
		//    会让会话线程后续再 close 同一个(可能已被复用的)句柄 ⇒ 轮询方案没有这个风险。
		if (m_ListenThread.joinable())
			m_ListenThread.join();
		AiTrace("Stop: listen thread joined");

		// 3) join 之后才关监听套接字:此刻已经没有别的线程会碰这个句柄。
		if (m_ListenSocket != kInvalidSocket)
		{
			closesocket(static_cast<SOCKET>(m_ListenSocket));
			m_ListenSocket = kInvalidSocket;
		}
		AiTrace("Stop: done");
	}

	bool AiControlServer::HasPending() const
	{
		std::lock_guard<std::mutex> guard(m_QueueMutex);
		return !m_Queue.empty();
	}

	void AiControlServer::ListenLoop()
	{
		AiTrace("ListenLoop: begin");
		while (m_Running.load())
		{
			// m_ListenSocket 只由 Start() 写入、由 Stop() 在 join 之后清空,因此这里无需加锁。
			const SOCKET listener = static_cast<SOCKET>(m_ListenSocket);
			if (listener == INVALID_SOCKET)
				break;
			// 等"有新连接"最多一个时间片,而不是阻塞在 accept() 上:一个时间片后本线程会
			// 回到循环顶复查 m_Running,Stop() 的 join 由此有上界。
			const int readable = WaitSocket(listener, true, false);
			if (readable < 0)
			{
				AiTrace("ListenLoop: listener not selectable (%d)", WSAGetLastError());
				break;
			}
			if (readable == 0)
				continue;
			if (!m_Running.load())
				break;   // 关机中:不再接受新连接
			const SOCKET client = accept(listener, nullptr, nullptr);
			if (client == INVALID_SOCKET)
			{
				if (!m_Running.load())
					break;
				continue;   // 待连接在 select 与 accept 之间被放弃:继续听
			}
			AiTrace("ListenLoop: accepted client");
			SessionLoop(reinterpret_cast<void*>(static_cast<uintptr_t>(client)));
			AiTrace("ListenLoop: session finished");
		}
		AiTrace("ListenLoop: exit");
	}

	void AiControlServer::SessionLoop(void* clientSocket)
	{
		const SOCKET client = static_cast<SOCKET>(reinterpret_cast<uintptr_t>(clientSocket));
		std::string buffer;
		char chunk[4096];
		bool alive = true;
		AiTrace("SessionLoop: begin");
		while (alive && m_Running.load())
		{
			// 等"可读"最多一个时间片。不能用"阻塞 recv + 外部 shutdown"的写法:实测本机
			// Windows 上 shutdown(无论 SD_RECEIVE/SD_SEND/SD_BOTH)返回 0 却不会唤醒另一
			// 线程里已经阻塞的 recv,于是 Stop() 的 join 会一直挂到客户端断开。
			const int readable = WaitSocket(client, true, false);
			if (readable < 0)
				break;
			if (readable == 0)
				continue;
			const int received = recv(client, chunk, sizeof(chunk), 0);
			if (received <= 0)
				break;   // 客户端断开或套接字出错
			buffer.append(chunk, static_cast<size_t>(received));
			size_t newline = 0;
			while ((newline = buffer.find('\n')) != std::string::npos)
			{
				std::string line = buffer.substr(0, newline);
				buffer.erase(0, newline + 1);
				while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
					line.pop_back();
				if (line.empty())
					continue;

				std::map<std::string, std::string> fields;
				std::string parseError;
				std::string response;
				if (!ParseFlatJsonImpl(line, &fields, &parseError))
					response = "{\"seq\":0,\"ok\":false,\"error\":\"bad json: " + EscapeJson(parseError) + "\"}\n";
				else
				{
					auto request = std::make_shared<Request>();
					request->Seq = fields.count("seq") ? std::strtoull(fields["seq"].c_str(), nullptr, 10) : 0;
					request->Command = fields.count("cmd") ? fields["cmd"] : std::string();
					request->Arguments = fields;
					// 入队与 Stop() 的清空共用 m_QueueMutex:Stop 先置 m_Running=false 再 swap 队列,
					// 所以这里要么入队后被 Stop 唤醒成 "server stopping",要么直接失败返回,
					// 不会留下"没被 Pump 消费、又等满 10s"的请求。
					bool stopping = false;
					{
						std::lock_guard<std::mutex> guard(m_QueueMutex);
						stopping = !m_Running.load();
						if (!stopping)
							m_Queue.push_back(request);
					}
					if (stopping)
					{
						response = "{\"seq\":" + std::to_string(request->Seq)
							+ ",\"ok\":false,\"error\":\"server stopping\"}\n";
					}
					else
					{
						m_QueueCv.notify_all();
						std::unique_lock<std::mutex> lock(request->Mutex);
						if (!request->Cv.wait_for(lock, std::chrono::seconds(10), [&] { return request->Done; }))
						{
							response = "{\"seq\":" + std::to_string(request->Seq)
								+ ",\"ok\":false,\"error\":\"timeout waiting for main thread\"}\n";
						}
						else
							response = request->Response + "\n";
					}
				}
				if (!SendAll(client, m_Running, response))
				{
					alive = false;
					break;
				}
			}
		}
		closesocket(client);
		AiTrace("SessionLoop: end");
	}

	void AiControlServer::Pump()
	{
		for (;;)
		{
			std::shared_ptr<Request> request;
			{
				std::lock_guard<std::mutex> guard(m_QueueMutex);
				if (m_Queue.empty())
					break;
				request = m_Queue.front();
				m_Queue.pop_front();
			}
			std::string result;
			std::string error;
			bool ok = false;
			if (m_Handler)
			{
				// 命令实现会触碰场景/渲染器,任何异常都必须变成"错误响应"而不是把编辑器带走
				// (实测:Play 期间读场景触发了结构写断言,异常逃逸 → std::terminate)。
				try
				{
					ok = m_Handler(request->Command, request->Arguments, result, error);
				}
				catch (const std::exception& exception)
				{
					ok = false;
					error = std::string("command threw: ") + exception.what();
				}
				catch (...)
				{
					ok = false;
					error = "command threw an unknown exception";
				}
			}
			std::ostringstream out;
			out << "{\"seq\":" << request->Seq << ",\"ok\":" << (ok ? "true" : "false");
			if (ok)
				out << ",\"result\":\"" << EscapeJson(result) << "\"";
			else
				out << ",\"error\":\"" << EscapeJson(error) << "\"";
			out << "}";
			{
				std::lock_guard<std::mutex> guard(request->Mutex);
				request->Response = out.str();
				request->Done = true;
			}
			request->Cv.notify_all();
		}
	}
}
