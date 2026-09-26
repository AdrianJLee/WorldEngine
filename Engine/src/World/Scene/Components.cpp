#include "wldpch.h"
#include "Components.h"

namespace World
{
	// 2026-09-26 重写:组件只带"配置"(脚本引用 + 属性表 + 源指纹);运行态(状态机 / 实例 /
	// 环境 / 回调)一律不克隆 —— 预制体与克隆体各自从 Pending 起跑。
	CppScriptComponent CloneComponentConfiguration(const CppScriptComponent& source)
	{
		CppScriptComponent copy;
		copy.ScriptName = source.ScriptName;
		copy.Properties = source.Properties;
		return copy;
	}

	LuauScriptComponent CloneComponentConfiguration(const LuauScriptComponent& source)
	{
		LuauScriptComponent copy;
		copy.ScriptPath = source.ScriptPath;
		copy.Properties = source.Properties;
		copy.SourceFingerprint = source.SourceFingerprint;
		return copy;
	}

	RigidBody2DComponent CloneComponentConfiguration(const RigidBody2DComponent& source)
	{
		RigidBody2DComponent copy;
		copy.Type = source.Type;
		copy.FixedRotation = source.FixedRotation;
		return copy;
	}

}
