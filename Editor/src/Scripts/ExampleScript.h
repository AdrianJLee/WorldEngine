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
	};
}