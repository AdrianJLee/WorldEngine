#pragma once
#include "World.h"

namespace World
{

	class ExampleScript : public ScriptableEntity
	{
		REFLECT_BODY(ExampleScript, TypeCategory::Script);
	public:

		virtual void OnCreate() override
		{}
		virtual void OnUpdate(Timestep ts) override
		{

		}
		virtual void OnDestroy() override
		{

		}

		// 原生脚本字段 schema 样例：这些 PROPERTY 会进入反射，供属性面板编辑并可随场景保存/重载。
		PROPERTY(Health);
		float Health = 100.0f;

		PROPERTY(Speed);
		float Speed = 1.0f;
	};
}
