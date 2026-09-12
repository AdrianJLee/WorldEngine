#pragma once

#include "World/Schema/SchemaRegistry.h"

namespace World
{
	// 引擎所有可扩展状态的显式所有者。宿主在 main 中创建,模块只拿到引用。
	class WorldContext
	{
	public:
		WorldContext();
		~WorldContext();
		WorldContext(const WorldContext&) = delete;
		WorldContext& operator=(const WorldContext&) = delete;

		Schema::SchemaRegistry& Schemas() { return m_Schemas; }
		const Schema::SchemaRegistry& Schemas() const { return m_Schemas; }

	private:
		Schema::SchemaRegistry m_Schemas;
	};
}
