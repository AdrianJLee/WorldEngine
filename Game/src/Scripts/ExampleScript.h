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

		// CPPT-2(FIX1):IVec* 只读摘要样例 —— 面板没有行控件,检视器只画一行只读摘要
		// (ReadOnly=true),值不参与编辑也不进存档;成员本身仍归脚本实例所有,Play 时原样保留。
		glm::ivec3 GridCell { 0, 0, 0 };

		WE_SCHEMA_BODY(Game, ExampleScript, Script)
			WE_SCHEMA_META(Category("Scripting/Examples"),
				Doc("Sample C++ behavior: shows declared fields, defaults, units and edit metadata in the inspector."))
			WE_FIELD(Health, Float, Default(100.0f), Range(0.0f, 1000.0f), Unit("hp"), Step(1.0f),
				Doc("Hit points used by the example; the declared default is 100."));
			WE_FIELD(Speed, Float, Default(1.0f), Range(0.0f, 100.0f), Unit("m/s"), Step(0.1f),
				Doc("Movement speed in metres per second; the declared default is 1."));
			WE_FIELD(GridCell, IVec3,
				Doc("Read-only summary sample: an integer grid cell; the inspector shows a non-editable summary row and the value never enters the scene file."));
		WE_SCHEMA_END
	};
}
