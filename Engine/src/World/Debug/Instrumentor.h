#pragma once

#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <thread>

namespace World
{
	struct ProfileResult
	{
		std::string Name;
		long long Start, End;
		uint32_t ThreadID;
	};

	struct InstrumentationSession
	{
		std::string Name;
	};

	class Instrumentor
	{
	private:
		InstrumentationSession* m_CurrentSession;
		std::ofstream m_OutputStream;
		int m_ProfileCount;
	public:
		Instrumentor()
			: m_CurrentSession(nullptr), m_ProfileCount(0)
		{}

		void BeginSession(const std::string& name, const std::string& filepath = "results.json")
		{
			WLD_CORE_INFO("##Beginning session '{0}'", name);
			m_OutputStream.open(filepath);
			WriteHeader();
			m_CurrentSession = new InstrumentationSession { name };
		}

		void EndSession()
		{
			WLD_CORE_INFO("##Ending session '{0}'", m_CurrentSession->Name);
			WriteFooter();
			m_OutputStream.close();
			delete m_CurrentSession;
			m_CurrentSession = nullptr;
			m_ProfileCount = 0;
		}

		void WriteProfile(const ProfileResult& result)
		{
			if (m_ProfileCount++ > 0)
				m_OutputStream << ",";

			std::string name = result.Name;
			std::replace(name.begin(), name.end(), '"', '\'');

			m_OutputStream << "{";
			m_OutputStream << "\"cat\":\"function\",";
			m_OutputStream << "\"dur\":" << (result.End - result.Start) << ',';
			m_OutputStream << "\"name\":\"" << name << "\",";
			m_OutputStream << "\"ph\":\"X\",";
			m_OutputStream << "\"pid\":0,";
			m_OutputStream << "\"tid\":" << result.ThreadID << ",";
			m_OutputStream << "\"ts\":" << result.Start;
			m_OutputStream << "}";

			m_OutputStream.flush();
		}

		void WriteHeader()
		{
			m_OutputStream << "{\"otherData\": {},\"traceEvents\":[";
			m_OutputStream.flush();
		}

		void WriteFooter()
		{
			m_OutputStream << "]}";
			m_OutputStream.flush();
		}

		static Instrumentor& Get();
	};

	class InstrumentationTimer
	{
	public:
		InstrumentationTimer(const char* name)
			: m_Name(name), m_Stopped(false)
		{
			m_StartTimepoint = std::chrono::high_resolution_clock::now();
		}

		~InstrumentationTimer()
		{
			if (!m_Stopped)
				Stop();
		}

		void Stop()
		{
			auto endTimepoint = std::chrono::high_resolution_clock::now();

			long long start = std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimepoint).time_since_epoch().count();
			long long end = std::chrono::time_point_cast<std::chrono::microseconds>(endTimepoint).time_since_epoch().count();

			//获取当前运行线程的唯一标识符
			uint32_t threadID = std::hash<std::thread::id> {}(std::this_thread::get_id());

			// 结束时将结果写入文件
			Instrumentor::Get().WriteProfile({ m_Name, start, end, threadID });

			m_Stopped = true;
		}
	private:
		const char* m_Name;
		std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimepoint;
		bool m_Stopped;
	};
}

#define WLD_PROFILE 0
#if WLD_PROFILE

#define WLD_PROFILE_BEGIN_SESSION(name, filepath) ::World::Instrumentor::Get().BeginSession(name, filepath)
#define WLD_PROFILE_END_SESSION() ::World::Instrumentor::Get().EndSession()
#define WLD_PROFILE_SCOPE(name) ::World::InstrumentationTimer timer##__LINE__(name)
// __FUNCTION__:它会自动替换为当前所在的函数名（字符串）
#define WLD_PROFILE_FUNCTION() WLD_PROFILE_SCOPE(__FUNCTION__)

#else

#define WLD_PROFILE_BEGIN_SESSION(name, filepath)
#define WLD_PROFILE_END_SESSION()
#define WLD_PROFILE_SCOPE(name)
#define WLD_PROFILE_FUNCTION()

#endif
