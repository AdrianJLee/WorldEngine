#include "wldpch.h"
#include "World/Scene/SelectionFilter.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace World
{
	namespace
	{
		bool ContainsCaseInsensitive(std::string_view str, std::string_view sub)
		{
			if (sub.empty()) return true;
			if (str.size() < sub.size()) return false;
			auto it = std::search(str.begin(), str.end(), sub.begin(), sub.end(),
				[](char ch1, char ch2) {
					return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
				});
			return it != str.end();
		}
	}

	void SelectionFilter::SetLayerSelectable(uint32_t layerMask, bool selectable)
	{
		if (selectable)
			m_SelectableMask |= layerMask;
		else
			m_SelectableMask &= ~layerMask;
	}

	bool SelectionFilter::IsLayerSelectable(uint32_t layerMask) const
	{
		return (m_SelectableMask & layerMask) == layerMask;
	}

	void SelectionFilter::SetLayerVisible(uint32_t layerMask, bool visible)
	{
		if (visible)
			m_VisibleMask |= layerMask;
		else
			m_VisibleMask &= ~layerMask;
	}

	bool SelectionFilter::IsLayerVisible(uint32_t layerMask) const
	{
		return (m_VisibleMask & layerMask) == layerMask;
	}

	void SelectionFilter::SetLayerLocked(uint32_t layerMask, bool locked)
	{
		if (locked)
			m_LockedMask |= layerMask;
		else
			m_LockedMask &= ~layerMask;
	}

	bool SelectionFilter::IsLayerLocked(uint32_t layerMask) const
	{
		return (m_LockedMask & layerMask) != 0;
	}

	void SelectionFilter::Reset()
	{
		m_SelectableMask = Layer_All;
		m_VisibleMask    = Layer_All;
		m_LockedMask     = 0;
	}

	bool SelectionFilter::CanSelectEntity(Entity entity, const Scene& scene) const
	{
		uint32_t layer = ClassifyEntityLayer(entity, scene);
		if (layer == 0) return false;
		if ((m_VisibleMask & layer) == 0) return false;
		if ((m_LockedMask & layer) != 0) return false;
		if ((m_SelectableMask & layer) == 0) return false;
		return true;
	}

	uint32_t SelectionFilter::ClassifyEntityLayer(Entity entity, const Scene& scene)
	{
		const entt::entity handle = static_cast<entt::entity>(entity);
		const auto& reg = scene.GetRegistry();
		if (!reg.valid(handle)) return 0;

		std::string_view name;
		if (reg.all_of<TagComponent>(handle))
		{
			const auto& tag = reg.get<TagComponent>(handle);
			name = StringPool::Get().NameOf(tag.Tag);
		}

		// 1. UI Check
		if (ContainsCaseInsensitive(name, "UI") || ContainsCaseInsensitive(name, "Canvas") || ContainsCaseInsensitive(name, "HUD"))
			return Layer_UI;

		// 2. Light Check
		if (reg.any_of<DirectionalLightComponent, PointLightComponent, AmbientLightComponent,
		               PointLight2DComponent, GlobalLight2DComponent, FreeformLight2DComponent>(handle) ||
		    ContainsCaseInsensitive(name, "Light"))
		{
			return Layer_Light;
		}

		// 3. Trigger Check
		bool isTrigger = false;
		if (reg.all_of<BoxCollider2DComponent>(handle) && reg.get<BoxCollider2DComponent>(handle).IsSensor)
			isTrigger = true;
		else if (reg.all_of<CircleCollider2DComponent>(handle) && reg.get<CircleCollider2DComponent>(handle).IsSensor)
			isTrigger = true;
		else if (reg.all_of<PolygonCollider2DComponent>(handle) && reg.get<PolygonCollider2DComponent>(handle).IsSensor)
			isTrigger = true;
		else if (reg.all_of<CapsuleCollider2DComponent>(handle) && reg.get<CapsuleCollider2DComponent>(handle).IsSensor)
			isTrigger = true;
		else if (reg.all_of<CompositeCollider2DComponent>(handle) && reg.get<CompositeCollider2DComponent>(handle).IsSensor)
			isTrigger = true;
		else if (reg.any_of<AreaEffector2DComponent, BuoyancyEffector2DComponent, SurfaceEffector2DComponent>(handle))
			isTrigger = true;
		else if (ContainsCaseInsensitive(name, "Trigger"))
			isTrigger = true;

		if (isTrigger)
			return Layer_Trigger;

		// 4. Collider Check
		if (reg.any_of<BoxCollider2DComponent, CircleCollider2DComponent, CapsuleCollider2DComponent,
		               PolygonCollider2DComponent, ChainCollider2DComponent, CompositeCollider2DComponent,
		               BoxCollider3DComponent, SphereCollider3DComponent, CapsuleCollider3DComponent,
		               MeshCollider3DComponent>(handle) ||
		    ContainsCaseInsensitive(name, "Collider"))
		{
			return Layer_Collider;
		}

		// 5. Background Check
		if (reg.all_of<ParallaxLayerComponent>(handle) ||
		    ContainsCaseInsensitive(name, "Background") ||
		    ContainsCaseInsensitive(name, "BG") ||
		    ContainsCaseInsensitive(name, "Backdrop"))
		{
			return Layer_Background;
		}

		// 6. Decor Check
		if (reg.all_of<TilemapComponent>(handle) ||
		    ContainsCaseInsensitive(name, "Decor") ||
		    ContainsCaseInsensitive(name, "Prop") ||
		    ContainsCaseInsensitive(name, "Environment"))
		{
			return Layer_Decor;
		}

		// 7. Actor Check
		if (reg.any_of<CharacterControllerComponent, HealthComponent, SmartObjectComponent,
		               RigidBody2DComponent, RigidBody3DComponent, MeshRendererComponent,
		               SkinnedMeshRendererComponent, SpriteComponent, SpriteAnimatorComponent>(handle) ||
		    ContainsCaseInsensitive(name, "Actor") ||
		    ContainsCaseInsensitive(name, "Player") ||
		    ContainsCaseInsensitive(name, "Enemy") ||
		    ContainsCaseInsensitive(name, "NPC"))
		{
			return Layer_Actor;
		}

		return Layer_Actor;
	}
}
