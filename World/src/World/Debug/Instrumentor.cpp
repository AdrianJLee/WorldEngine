#include "wldpch.h"
#include "Instrumentor.h"

namespace World
{
	Instrumentor& Instrumentor::Get()
	{
		static Instrumentor instance;
		return instance;
	}
}
