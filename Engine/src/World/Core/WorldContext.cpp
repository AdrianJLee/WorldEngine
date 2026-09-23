#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Schema/Generated/World/WorldSchemaRegistration.h"
#include "World/Utils/DynamicLibrary.h"

namespace World
{
	WorldContext::WorldContext()
	{
		// 内建模块显式注册;不存在静态初始化期注册。
		if (!Schema::RegisterWorldSchemaModule(m_Schemas))
			WLD_CORE_ERROR("WorldContext: built-in World schema module reported a registration error");
	}

	WorldContext::~WorldContext()
	{
		m_Modules.UnloadAll(*this);
		Schema::UnregisterWorldSchemaModule(m_Schemas);
	}
}
