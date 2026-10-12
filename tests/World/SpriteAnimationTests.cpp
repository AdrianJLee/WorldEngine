#include "World/Renderer/SpriteAnimation.h"
#include "World/Scene/Components.h"
#include "World/Scene/Systems/SpriteAnimationSystem.h"
#include "World/Core/Log.h"
#include "World/Core/StringPool.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static void TestAnimationSerialization()
{
	SpriteAnimation anim("knight_walk", 10.0f, AnimationLoopMode::Loop);
	anim.SetAtlasPath("assets/textures/characters/knight_atlas.watlas");
	anim.AddFrame(SpriteAnimationFrame("walk_0", 0.1f));
	anim.AddFrame(SpriteAnimationFrame("walk_1", 0.1f, "footstep"));
	anim.AddFrame(SpriteAnimationFrame("walk_2", 0.1f));
	anim.AddFrame(SpriteAnimationFrame("walk_3", 0.1f, "footstep"));

	assert(anim.GetFrameCount() == 4);
	assert(std::abs(anim.GetTotalDuration() - 0.4f) < 1e-5f);

	std::string yaml = anim.SerializeYaml();
	assert(!yaml.empty());
	assert(yaml.find("knight_walk") != std::string::npos);
	assert(yaml.find("footstep") != std::string::npos);

	SpriteAnimation loaded;
	bool ok = loaded.DeserializeYaml(yaml);
	assert(ok);
	assert(loaded.GetName() == "knight_walk");
	assert(loaded.GetAtlasPath() == "assets/textures/characters/knight_atlas.watlas");
	assert(loaded.GetFrameCount() == 4);
	assert(loaded.GetLoopMode() == AnimationLoopMode::Loop);

	const auto* f1 = loaded.GetFrame(1);
	assert(f1 != nullptr);
	assert(f1->NameString == "walk_1");
	assert(f1->EventTag == "footstep");

	std::cout << "[PASS] TestAnimationSerialization\n";
}

static void TestAnimationSampling()
{
	SpriteAnimation anim("hero_attack", 10.0f, AnimationLoopMode::Once);
	anim.AddFrame(SpriteAnimationFrame("atk_0", 0.1f));
	anim.AddFrame(SpriteAnimationFrame("atk_1", 0.1f));
	anim.AddFrame(SpriteAnimationFrame("atk_2", 0.1f));

	bool finished = false;
	assert(anim.SampleFrame(0.05f, &finished) == 0 && !finished);
	assert(anim.SampleFrame(0.15f, &finished) == 1 && !finished);
	assert(anim.SampleFrame(0.25f, &finished) == 2 && !finished);
	assert(anim.SampleFrame(0.35f, &finished) == 2 && finished);

	// Test PingPong
	anim.SetLoopMode(AnimationLoopMode::PingPong);
	// Total duration = 0.3s. Cycle = 0.6s.
	// At t = 0.05s -> frame 0
	// At t = 0.25s -> frame 2
	// At t = 0.35s -> t_effective = 0.6 - 0.35 = 0.25s -> frame 2
	// At t = 0.45s -> t_effective = 0.6 - 0.45 = 0.15s -> frame 1
	// At t = 0.55s -> t_effective = 0.6 - 0.55 = 0.05s -> frame 0
	assert(anim.SampleFrame(0.05f) == 0);
	assert(anim.SampleFrame(0.25f) == 2);
	assert(anim.SampleFrame(0.35f) == 2);
	assert(anim.SampleFrame(0.45f) == 1);
	assert(anim.SampleFrame(0.55f) == 0);

	std::cout << "[PASS] TestAnimationSampling\n";
}

static void TestAnimationSystemRuntime()
{
	entt::registry registry;
	auto entity = registry.create();

	auto& sprite = registry.emplace<SpriteComponent>(entity);
	auto& animator = registry.emplace<SpriteAnimatorComponent>(entity);

	// Save test animation to temp file
	SpriteAnimation anim("test_cycle", 10.0f, AnimationLoopMode::Loop);
	anim.AddFrame(SpriteAnimationFrame("frame_a", 0.1f));
	anim.AddFrame(SpriteAnimationFrame("frame_b", 0.1f));

	const std::string animPath = "build/temp_test_cycle.wanim";
	anim.SaveToFile(animPath);

	animator.Animation.Path = StringPool::Get().InternPath(animPath);
	animator.Playing = true;
	animator.Speed = 1.0f;

	// Initial update at t = 0.05s -> frame 0 ("frame_a")
	SpriteAnimationSystem::Update(registry, Timestep(0.05f));
	assert(animator.CurrentFrame == 0);
	assert(sprite.SubTextureName == StringPool::Get().InternName("frame_a"));

	// Step forward by 0.1s (total 0.15s) -> frame 1 ("frame_b")
	SpriteAnimationSystem::Update(registry, Timestep(0.10f));
	assert(animator.CurrentFrame == 1);
	assert(sprite.SubTextureName == StringPool::Get().InternName("frame_b"));

	// Step forward by 0.1s (total 0.25s, wrapped) -> frame 0 ("frame_a")
	SpriteAnimationSystem::Update(registry, Timestep(0.10f));
	assert(animator.CurrentFrame == 0);
	assert(sprite.SubTextureName == StringPool::Get().InternName("frame_a"));

	std::cout << "[PASS] TestAnimationSystemRuntime\n";
}

int main()
{
	Log::Init();
	std::cout << "Running 2D Sprite Animation Tests...\n";

	TestAnimationSerialization();
	TestAnimationSampling();
	TestAnimationSystemRuntime();

	std::cout << "All 2D Sprite Animation tests passed.\n";
	return 0;
}
