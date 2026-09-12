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
			WE_FIELD(Health, Float);
			WE_FIELD(Speed, Float);
		WE_SCHEMA_END
	};
}
