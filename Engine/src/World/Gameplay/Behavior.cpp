#include "wldpch.h"
#include "Behavior.h"

namespace World::Gameplay
{
	// IBehavior 标了 WLD_API:跨 DLL(Game.dll/插件实现行为)必须有导出定义,
	// 只写在头里会让 DLL 消费者链接失败(实测 LNK2019 __imp_??1IBehavior)。
	IBehavior::IBehavior() = default;
	IBehavior::~IBehavior() = default;

	void IBehavior::OnCreate(Entity self) { (void)self; }
	void IBehavior::OnUpdate(Entity self, Timestep dt) { (void)self; (void)dt; }
	void IBehavior::OnFixedUpdate(Entity self, Timestep dt) { (void)self; (void)dt; }
	void IBehavior::OnEvent(Entity self, const EventValue& event) { (void)self; (void)event; }
	void IBehavior::OnDestroy(Entity self) { (void)self; }
}
