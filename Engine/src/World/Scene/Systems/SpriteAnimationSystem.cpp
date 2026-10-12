#include "SpriteAnimationSystem.h"
#include "World/Scene/Components.h"
#include "World/Renderer/SpriteAnimation.h"
#include "World/Core/StringPool.h"

namespace World
{
	void SpriteAnimationSystem::Update(entt::registry& registry, Timestep ts)
	{
		auto view = registry.view<SpriteComponent, SpriteAnimatorComponent>();
		for (auto entity : view)
		{
			auto [sprite, animator] = view.get<SpriteComponent, SpriteAnimatorComponent>(entity);
			if (!animator.Playing || animator.Finished) continue;
			if (!animator.Animation.HasPath()) continue;

			const std::string& animPath = StringPool::Get().PathOf(animator.Animation.Path);
			if (animPath.empty()) continue;

			auto anim = SpriteAnimation::GetOrLoad(animPath);
			if (!anim || anim->GetFrameCount() == 0) continue;

			animator.CurrentTime += ts.GetSeconds() * animator.Speed;
			bool finished = false;
			uint32_t frameIdx = anim->SampleFrame(animator.CurrentTime, &finished);
			animator.CurrentFrame = frameIdx;
			animator.Finished = finished;

			const auto* frame = anim->GetFrame(frameIdx);
			if (frame && frame->SubTextureName.IsValid())
			{
				sprite.SubTextureName = frame->SubTextureName;
			}
		}
	}
}
