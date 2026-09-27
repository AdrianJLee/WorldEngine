#pragma once
#include "World.h"

namespace World
{

	class ExampleScript : public ScriptableEntity
	{
	public:

		virtual void OnCreate() override
		{}
		virtual void OnUpdate(Timestep ts) override
		{

		}
		virtual void OnDestroy() override
		{

		}

		// 原生脚本字段 schema 样例:供属性面板编辑并可随场景保存/重载。
		float Health = 100.0f;

		float Speed = 1.0f;

		WE_SCHEMA_BODY(Game, ExampleScript, Script)
			WE_SCHEMA_META(Category("Scripting/Examples"),
				Doc("Sample C++ behavior: shows declared fields, defaults, units and edit metadata in the inspector."))
			WE_FIELD(Health, Float, Default(100.0f), Range(0.0f, 1000.0f), Unit("hp"), Step(1.0f),
				Doc("Hit points used by the example; the declared default is 100."));
			WE_FIELD(Speed, Float, Default(1.0f), Range(0.0f, 100.0f), Unit("m/s"), Step(0.1f),
				Doc("Movement speed in metres per second; the declared default is 1."));
		WE_SCHEMA_END
	};
}
