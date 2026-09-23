#pragma once

#include "World/Modules/ModuleManager.h"
#include "World/Schema/SchemaRegistry.h"
#include "World/Core/Vfs/Vfs.h"

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
		Modules::ModuleManager& Modules() { return m_Modules; }
		const Modules::ModuleManager& Modules() const { return m_Modules; }
		World::Vfs::Vfs& Vfs() { return m_Vfs; }
		const World::Vfs::Vfs& Vfs() const { return m_Vfs; }

	private:
		World::Vfs::Vfs m_Vfs;
		Schema::SchemaRegistry m_Schemas;
		Modules::ModuleManager m_Modules;
	};
}
