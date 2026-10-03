#include "wldpch.h"
#include "World/Debug/Instrumentor.h"

namespace World
{
	Instrumentor& Instrumentor::Get()
	{
		static Instrumentor instance;
		return instance;
	}
}
