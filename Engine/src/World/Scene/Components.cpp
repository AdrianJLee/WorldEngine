#include "wldpch.h"
#include "Components.h"

namespace World
{
	NativeScriptComponent CloneComponentConfiguration(const NativeScriptComponent& source)
	{
		NativeScriptComponent copy;
		copy.ScriptName = source.ScriptName;
		copy.FieldValues = source.FieldValues;
		copy.InstantiateScript = source.InstantiateScript;
		copy.DestroyScript = source.DestroyScript;
		return copy;
	}

	LuaScriptComponent CloneComponentConfiguration(const LuaScriptComponent& source)
	{
		LuaScriptComponent copy;
		copy.ScriptFilePath = source.ScriptFilePath;
		copy.CachedFields = source.CachedFields;
		copy.LastModifiedTime = source.LastModifiedTime;
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

	ScriptableEntity* NativeScriptComponent::GetOrCreateEditorInstance(bool allowCreate, bool& outOwned)
	{
		outOwned = false;
		if (Instance)
			return Instance;
		if (allowCreate && isFirstDraw && InstantiateScript)
		{
			ScriptableEntity* preview = InstantiateScript();
			if (preview)
			{
				outOwned = true;
				isFirstDraw = false;
				return preview;
			}
		}
		return nullptr;
	}

	void NativeScriptComponent::ReleaseEditorInstance(ScriptableEntity* preview)
	{
		if (!preview || preview == Instance || !DestroyScript)
			return;
		DestroyScript(preview);
	}

	void NativeScriptComponent::ResetEditorFieldState()
	{
		FieldValues.clear();
		isFirstDraw = true;
	}

	Schema::Value NativeScriptComponent::GetErasedFieldValue(const Schema::TypeSchema&, const Schema::FieldSchema& field, ScriptableEntity* instance)
	{
		if (instance)
		{
			Schema::Value value = field.Get(instance);
			FieldValues[field.Name] = value;
			return value;
		}

		auto it = FieldValues.find(field.Name);
		if (it != FieldValues.end())
			return it->second;
		FieldValues[field.Name] = field.Default;
		return field.Default;
	}

	void NativeScriptComponent::SetErasedFieldValue(const Schema::TypeSchema&, const Schema::FieldSchema& field, ScriptableEntity* instance, const Schema::Value& value)
	{
		if (instance)
			field.Set(instance, value);
		else
			FieldValues[field.Name] = value;
	}
}
