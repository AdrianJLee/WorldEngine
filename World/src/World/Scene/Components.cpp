#include "wldpch.h"
#include "Components.h"

namespace World
{
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

	std::any NativeScriptComponent::GetErasedFieldValue(const TypeDesc& typeDesc, const PropertyDesc& prop, ScriptableEntity* instance)
	{
		if (instance)
		{
			std::any value = typeDesc.GetValueErased(instance, prop);
			FieldValues[prop.Name] = value;
			return value;
		}

		auto it = FieldValues.find(prop.Name);
		if (it != FieldValues.end())
			return it->second;

		switch (prop.Type)
		{
			case DataType::Int32: return std::any(int32_t(0));
			case DataType::Float: return std::any(float(0.0f));
			case DataType::Bool: return std::any(false);
			case DataType::Vec2: return std::any(glm::vec2(0.0f));
			case DataType::Vec3: return std::any(glm::vec3(0.0f));
			case DataType::String: return std::any(std::string(""));
			case DataType::Enum: return std::any(int32_t(0));
			default: return std::any();
		}
	}

	void NativeScriptComponent::SetErasedFieldValue(const TypeDesc& typeDesc, const PropertyDesc& prop, ScriptableEntity* instance, const std::any& value)
	{
		if (instance)
			typeDesc.SetValueErased(instance, prop, value);
		else
			FieldValues[prop.Name] = value;
	}
}
