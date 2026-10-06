#include "wldpch.h"
#include "World/Profiling/TraceWriter.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace World::Profiling
{
	namespace
	{
		// 导出期的小工具:先把 JSON 拼到 1MB 暂存串,满了再落盘 —— 避免为每个事件
		// 调一次 ofstream(20 万事件下差距是数量级的),同时不需要把整个文件驻留内存。
		constexpr size_t kFlushThreshold = 1u << 20;

		class JsonSink
		{
		public:
			explicit JsonSink(std::ofstream& stream) : m_Stream(stream) {}

			void Append(const char* text, size_t length)
			{
				m_Scratch.append(text, length);
				MaybeFlush();
			}

			void Append(const std::string& text) { Append(text.data(), text.size()); }

			void AppendRaw(const char* text) { Append(text, std::strlen(text)); }

			// 名字来自代码字面量(标识符/路径),只需处理 JSON 里真正非法的两个字符。
			void AppendEscaped(const char* text)
			{
				if (!text)
					return;
				for (const char* cursor = text; *cursor; ++cursor)
				{
					switch (*cursor)
					{
						case '"':  AppendRaw("\\\""); break;
						case '\\': AppendRaw("\\\\"); break;
						case '\n': AppendRaw("\\n"); break;
						case '\r': AppendRaw("\\r"); break;
						case '\t': AppendRaw("\\t"); break;
						default:   Append(cursor, 1); break;
					}
				}
			}

			bool Flush()
			{
				if (!m_Scratch.empty())
				{
					m_Stream.write(m_Scratch.data(), static_cast<std::streamsize>(m_Scratch.size()));
					m_Scratch.clear();
				}
				return m_Stream.good();
			}

			bool Ok() const { return m_Stream.good() && m_Failed == false; }

		private:
			void MaybeFlush()
			{
				if (m_Scratch.size() >= kFlushThreshold)
				{
					if (!Flush())
						m_Failed = true;
				}
			}

			std::ofstream& m_Stream;
			std::string m_Scratch;
			bool m_Failed = false;
		};

		void AppendUint(std::string& out, uint64_t value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
			out.append(buffer);
		}

		std::string UintToText(uint64_t value)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
			return buffer;
		}

		// 一个线程上尚未闭合的作用域(导出期配对 Begin/End 求 dur)。
		struct OpenScope
		{
			const TraceEvent* Event = nullptr;
		};

		void WriteCompleteEvent(JsonSink& sink, const char* name, const char* category,
			uint64_t startUs, uint64_t durationUs, uint16_t threadSlot,
			uint64_t frameIndex, bool hasFrame)
		{
			sink.AppendRaw("{\"name\":\"");
			sink.AppendEscaped(name ? name : "<unnamed>");
			sink.AppendRaw("\",\"cat\":\"");
			sink.AppendEscaped(category);
			sink.AppendRaw("\",\"ph\":\"X\",\"pid\":1,\"tid\":");
			std::string number = UintToText(threadSlot);
			sink.Append(number);
			sink.AppendRaw(",\"ts\":");
			number = UintToText(startUs);
			sink.Append(number);
			sink.AppendRaw(",\"dur\":");
			number = UintToText(durationUs);
			sink.Append(number);
			sink.AppendRaw(",\"args\":{");
			if (hasFrame)
			{
				sink.AppendRaw("\"frame\":");
				sink.Append(UintToText(frameIndex));
			}
			sink.AppendRaw("}}");
		}

		void WriteCounterEvent(JsonSink& sink, const char* name, uint64_t tsUs,
			int64_t value, uint16_t threadSlot, uint64_t frameIndex)
		{
			sink.AppendRaw("{\"name\":\"");
			sink.AppendEscaped(name ? name : "<unnamed>");
			sink.AppendRaw("\",\"ph\":\"C\",\"pid\":1,\"tid\":");
			sink.Append(UintToText(threadSlot));
			sink.AppendRaw(",\"ts\":");
			sink.Append(UintToText(tsUs));
			sink.AppendRaw(",\"args\":{\"value\":");
			sink.Append(UintToText(static_cast<uint64_t>(value)));
			sink.AppendRaw(",\"frame\":");
			sink.Append(UintToText(frameIndex));
			sink.AppendRaw("}}");
		}

		void WriteMetadata(JsonSink& sink, const char* key, const char* value)
		{
			sink.AppendRaw("{\"name\":\"");
			sink.AppendEscaped(key);
			sink.AppendRaw("\",\"ph\":\"M\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"");
			sink.AppendEscaped(value);
			sink.AppendRaw("\"}}");
		}
	}

	bool WriteChromeTrace(const std::string& path, const TraceWriteRequest& request, std::string* error)
	{
		const auto fail = [error](const std::string& message)
		{
			if (error)
				*error = message;
			return false;
		};

		if (path.empty())
			return fail("trace output path is empty");

		std::error_code ec;
		const std::filesystem::path target(path);
		if (target.has_parent_path())
			std::filesystem::create_directories(target.parent_path(), ec);

		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream)
			return fail("cannot open trace file for writing: " + path);

		JsonSink sink(stream);

		sink.AppendRaw("{\"displayTimeUnit\":\"ms\",\"otherData\":{\"schemaVersion\":");
		sink.Append(UintToText(request.SchemaVersion));
		sink.AppendRaw(",\"droppedEvents\":");
		sink.Append(UintToText(request.DroppedEvents));
		sink.AppendRaw(",\"eventCount\":");
		sink.Append(UintToText(request.EventCount));
		sink.AppendRaw("},\"traceEvents\":[");

		bool first = true;
		const auto separator = [&sink, &first]()
		{
			if (!first)
				sink.AppendRaw(",");
			first = false;
		};

		// 1) 元数据:进程名 + 线程名。**没有这些,trace 在多线程下无法解读**。
		if (request.MetaCount > 0)
		{
			for (uint32_t i = 0; i < request.MetaCount; ++i)
			{
				if (!request.Meta[i].Key || !request.Meta[i].Value)
					continue;
				if (request.Meta[i].Key[0] == '\0')
					continue;
				separator();
				WriteMetadata(sink, request.Meta[i].Key, request.Meta[i].Value);
			}
		}
		for (uint32_t slot = 0; slot < request.ThreadNameCount && slot < kMaxThreads; ++slot)
		{
			if (!request.ThreadNames[slot])
				continue;
			separator();
			sink.AppendRaw("{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":");
			sink.Append(UintToText(slot));
			sink.AppendRaw(",\"args\":{\"name\":\"");
			sink.AppendEscaped(request.ThreadNames[slot]);
			sink.AppendRaw("\"}}");
		}

		// 2) 事件:按线程维护未闭合作用域栈,把 Begin/End 配对成 dur。
		std::vector<OpenScope> stacks[kMaxThreads];
		for (uint32_t slot = 0; slot < kMaxThreads; ++slot)
			stacks[slot].reserve(32);
		const TraceEvent* pendingFrame[kMaxThreads] = {};

		for (uint32_t i = 0; i < request.EventCount; ++i)
		{
			const TraceEvent& event = request.Events[i];
			const uint16_t slot = event.ThreadSlot < kMaxThreads ? event.ThreadSlot : 0;
			std::vector<OpenScope>& stack = stacks[slot];

			switch (event.Type)
			{
				case TraceEventType::FrameBegin:
				{
					// 帧本身也是一条 dur 事件 ⇒ FrameEnd 时闭合。缺 End(截断)时按零时长补。
					pendingFrame[slot] = &event;
					break;
				}
				case TraceEventType::FrameEnd:
				{
					const TraceEvent* begin = pendingFrame[slot];
					const uint64_t start = begin ? begin->Timestamp : event.Timestamp;
					const uint64_t duration = (begin && event.Timestamp > start)
						? event.Timestamp - start : 0;
					separator();
					WriteCompleteEvent(sink, "Frame", "frame", start, duration, slot,
						event.FrameIndex, true);
					// 帧时长同时作为计数器轨道:Perfetto 里直接能看到帧时间曲线。
					separator();
					// 单位是**微秒**(FrameEnd.Timestamp - FrameBegin.Timestamp)。
					// 名字必须与单位一致:叫 "frameMs" 而存微秒会让所有消费者差 1000 倍。
					WriteCounterEvent(sink, "frameUs", event.Timestamp,
						static_cast<int64_t>(duration), slot, event.FrameIndex);
					pendingFrame[slot] = nullptr;
					break;
				}
				case TraceEventType::ScopeBegin:
				{
					OpenScope open;
					open.Event = &event;
					stack.push_back(open);
					break;
				}
				case TraceEventType::ScopeEnd:
				{
					// 与最近一个**同名**的未闭合作用域配对(容忍截断导致的缺 End)。
					OpenScope matched {};
					bool found = false;
					for (size_t index = stack.size(); index-- > 0;)
					{
						if (stack[index].Event && stack[index].Event->Name == event.Name)
						{
							matched = stack[index];
							stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(index));
							found = true;
							break;
						}
					}
					separator();
					if (found && matched.Event->Timestamp <= event.Timestamp)
					{
						WriteCompleteEvent(sink, event.Name, "scope",
							matched.Event->Timestamp, event.Timestamp - matched.Event->Timestamp,
							slot, event.FrameIndex, true);
					}
					else
					{
						WriteCompleteEvent(sink, event.Name, "scope", event.Timestamp, 0,
							slot, event.FrameIndex, true);
					}
					break;
				}
				case TraceEventType::Counter:
				{
					separator();
					WriteCounterEvent(sink, event.Name, event.Timestamp, event.Value, slot,
						event.FrameIndex);
					break;
				}
			}

			if (!sink.Ok())
				return fail("trace write failed (disk full or stream error): " + path);
		}

		// 3) 收尾:未闭合的作用域(采集在作用域中途结束)按零时长补充,保证层级可读。
		for (uint32_t slot = 0; slot < kMaxThreads; ++slot)
		{
			for (const OpenScope& open : stacks[slot])
			{
				if (!open.Event)
					continue;
				separator();
				WriteCompleteEvent(sink, open.Event->Name, "scope", open.Event->Timestamp, 0,
					static_cast<uint16_t>(slot), open.Event->FrameIndex, true);
			}
		}

		sink.AppendRaw("]}");
		if (!sink.Flush() || !sink.Ok())
			return fail("trace flush failed: " + path);
		stream.close();
		if (!stream)
			return fail("trace close failed: " + path);
		return true;
	}
}
