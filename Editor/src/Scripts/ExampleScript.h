#pragma once
#include "World.h"

namespace World
{

	class ExampleScript : public ScriptableEntity
	{
		REGISTER_SCRIPT(ExampleScript);
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