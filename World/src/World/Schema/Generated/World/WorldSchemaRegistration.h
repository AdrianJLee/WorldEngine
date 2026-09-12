#pragma once

#include "World/Schema/SchemaRegistry.h"

namespace World::Schema
{
	bool RegisterWorldSchemaModule(SchemaRegistry& registry);
	void UnregisterWorldSchemaModule(SchemaRegistry& registry);
}
