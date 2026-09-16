// winsock2 必须在 Windows.h(经 wldpch.h 间接引入)之前包含,否则类型重定义。
#include <winsock2.h>
#include <ws2tcpip.h>

#include "wldpch.h"
#include "AiControlServer.h"

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

		// ---- 极简 JSON:只解析我们自己的**扁平**请求对象 ----
		// 支持 {"key":value,...},value 为字符串/数字/true/false/null;不支持嵌套。
		// 这样脚本侧仍然是标准 JSON,而引擎侧不需要引入完整 JSON 库。
		bool ParseFlatJson(const std::string& text, std::map<std::string, std::string>* out, std::string* error)
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

	bool AiControlServer::Start(uint16_t port, Handler handler)
	{
		if (m_Running)
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
		m_Running = true;
		m_ListenThread = std::thread(&AiControlServer::ListenLoop, this);
		WLD_CORE_INFO("[ai] control channel listening on 127.0.0.1:{0}", port);
		return true;
	}

	void AiControlServer::Stop()
	{
		if (!m_Running)
			return;
		m_Running = false;
		if (m_ListenSocket != static_cast<uintptr_t>(~0ull))
		{
			closesocket(static_cast<SOCKET>(m_ListenSocket));
			m_ListenSocket = static_cast<uintptr_t>(~0ull);
		}
		if (m_ListenThread.joinable())
			m_ListenThread.join();
	}

	bool AiControlServer::HasPending() const
	{
		std::lock_guard<std::mutex> guard(m_QueueMutex);
		return !m_Queue.empty();
	}

	void AiControlServer::ListenLoop()
	{
		while (m_Running)
		{
			const SOCKET listener = static_cast<SOCKET>(m_ListenSocket);
			if (listener == INVALID_SOCKET)
				break;
			const SOCKET client = accept(listener, nullptr, nullptr);
			if (client == INVALID_SOCKET)
				break;   // Stop() 关闭监听套接字后会走到这里
			SessionLoop(reinterpret_cast<void*>(static_cast<uintptr_t>(client)));
		}
	}

	void AiControlServer::SessionLoop(void* clientSocket)
	{
		const SOCKET client = static_cast<SOCKET>(reinterpret_cast<uintptr_t>(clientSocket));
		std::string buffer;
		char chunk[4096];
		while (m_Running)
		{
			const int received = recv(client, chunk, sizeof(chunk), 0);
			if (received <= 0)
				break;
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
				if (!ParseFlatJson(line, &fields, &parseError))
					response = "{\"seq\":0,\"ok\":false,\"error\":\"bad json: " + EscapeJson(parseError) + "\"}\n";
				else
				{
					auto request = std::make_shared<Request>();
					request->Seq = fields.count("seq") ? std::strtoull(fields["seq"].c_str(), nullptr, 10) : 0;
					request->Command = fields.count("cmd") ? fields["cmd"] : std::string();
					request->Arguments = fields;
					{
						std::lock_guard<std::mutex> guard(m_QueueMutex);
						m_Queue.push_back(request);
					}
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
				send(client, response.c_str(), static_cast<int>(response.size()), 0);
			}
		}
		closesocket(client);
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
				ok = m_Handler(request->Command, request->Arguments, result, error);
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
