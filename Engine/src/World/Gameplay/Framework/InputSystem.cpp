#include "wldpch.h"
#include "World/Gameplay/Framework/InputSystem.h"

#include <unordered_map>
#include <utility>

namespace World::Gameplay
{
	namespace
	{
		using SceneSnapshot = Scene::InputSnapshot;

		// 每个场景的"上一可变帧"边沿与鼠标状态(引擎主线程使用)。
		// 场景销毁后条目允许残留:查询/采样前按 slot/generation 判活,地址复用不串味。
		struct SceneInputState
		{
			std::unordered_map<std::string, bool> Pressed;
			std::unordered_map<std::string, bool> Released;
			glm::vec2 PreviousMouse{ 0.0f };
			bool HasPreviousMouse = false;
			uint64_t SampleCount = 0;
			uint16_t SceneSlot = 0;
			uint16_t SceneGeneration = 0;
		};

		std::unordered_map<const Scene*, SceneInputState>& States()
		{
			static std::unordered_map<const Scene*, SceneInputState> states;
			return states;
		}

		SceneInputState* Find(const Scene& scene)
		{
			auto& states = States();
			auto it = states.find(&scene);
			if (it == states.end())
				return nullptr;
			if (!Scene::IsSceneAlive(&scene, it->second.SceneSlot, it->second.SceneGeneration))
			{
				states.erase(it);
				return nullptr;
			}
			return &it->second;
		}

		SceneInputState& Ensure(const Scene& scene)
		{
			if (SceneInputState* existing = Find(scene))
				return *existing;
			SceneInputState& created = States()[&scene];
			created.SceneSlot = scene.GetSceneSlot();
			created.SceneGeneration = scene.GetSceneGeneration();
			return created;
		}
	}

	const SceneSnapshot& InputSystem::Sample(Scene& scene, const InputService& service, glm::vec2 mousePosition)
	{
		SceneInputState& state = Ensure(scene);
		const Gameplay::InputSnapshot& source = service.GetSnapshot();

		// 第一帧(或场景刚复用)没有"上一帧鼠标"可比 ⇒ 增量记 0,不做假位移。
		const bool firstSample = !scene.GetInputSnapshot().Valid;

		SceneSnapshot snapshot;
		snapshot.Buttons = source.Down;
		snapshot.Axes = source.Axes;
		snapshot.MousePosition = mousePosition;
		snapshot.MouseDelta = (firstSample || !state.HasPreviousMouse)
			? glm::vec2(0.0f)
			: (mousePosition - state.PreviousMouse);
		// 平台层(World::Input)目前没有滚轮读数;字段保留给宿主/平台扩展,固定为 0。
		snapshot.ScrollDelta = 0.0f;
		snapshot.SampledFrame = scene.GetTime().FrameCount;
		snapshot.Valid = true;
		scene.SetInputSnapshot(std::move(snapshot));

		state.PreviousMouse = mousePosition;
		state.HasPreviousMouse = true;
		state.Pressed = source.Pressed;
		state.Released = source.Released;
		++state.SampleCount;
		return scene.GetInputSnapshot();
	}

	void InputSystem::Forget(Scene& scene)
	{
		States().erase(&scene);
		// 清空快照:运行停止后读到的必须是"没有输入"(Valid=false),不能是上一场运行的旧值。
		scene.SetInputSnapshot(Scene::InputSnapshot{});
	}

	bool InputSystem::IsDown(const Scene& scene, const std::string& action)
	{
		const auto it = scene.GetInputSnapshot().Buttons.find(action);
		return it != scene.GetInputSnapshot().Buttons.end() && it->second;
	}

	bool InputSystem::WasPressed(const Scene& scene, const std::string& action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->Pressed.find(action);
		return it != state->Pressed.end() && it->second;
	}

	bool InputSystem::WasReleased(const Scene& scene, const std::string& action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->Released.find(action);
		return it != state->Released.end() && it->second;
	}

	float InputSystem::GetAxis(const Scene& scene, const std::string& axis)
	{
		const auto it = scene.GetInputSnapshot().Axes.find(axis);
		return it != scene.GetInputSnapshot().Axes.end() ? it->second : 0.0f;
	}

	uint64_t InputSystem::GetLastSampledFrame(const Scene& scene)
	{
		return scene.GetInputSnapshot().SampledFrame;
	}

	uint64_t InputSystem::GetSampleCount(const Scene& scene)
	{
		const SceneInputState* state = Find(scene);
		return state ? state->SampleCount : 0;
	}
}
