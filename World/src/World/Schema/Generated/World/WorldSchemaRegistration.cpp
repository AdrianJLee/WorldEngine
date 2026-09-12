#include "World/Schema/Generated/World/WorldSchemaRegistration.h"

namespace World::Schema
{
	bool RegisterWorldSchemaModule(SchemaRegistry&)
	{
		// P0-M1 占位:尚未迁移任何 World 组件;M2 由 schema-compiler 重新生成填充。
		return true;
	}

	void UnregisterWorldSchemaModule(SchemaRegistry&)
	{
	}
}
