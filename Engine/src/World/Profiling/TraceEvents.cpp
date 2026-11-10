#include "World/Profiling/TraceEvents.h"

namespace World::Profiling
{
	const char* TraceEventTypeName(TraceEventType type)
	{
		switch (type)
		{
			case TraceEventType::FrameBegin: return "FrameBegin";
			case TraceEventType::FrameEnd:   return "FrameEnd";
			case TraceEventType::ScopeBegin: return "ScopeBegin";
			case TraceEventType::ScopeEnd:   return "ScopeEnd";
			case TraceEventType::Counter:    return "Counter";
		}
		return "Unknown";
	}
}
