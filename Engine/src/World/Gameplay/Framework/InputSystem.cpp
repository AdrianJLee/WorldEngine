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
		struct SceneInputState
		{
			std::unordered_map<std::string, bool> Pressed;
			std::unordered_map<std::string, bool> Released;
			std::unordered_map<uint32_t, bool> PressedById;
			std::unordered_map<uint32_t, bool> ReleasedById;
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

	const SceneSnapshot& InputSystem::Sample(Scene& scene, const InputService& service, glm::vec2 mousePosition,
		glm::vec2 scrollDelta)
	{
		SceneInputState& state = Ensure(scene);
		const Gameplay::InputSnapshot& source = service.GetSnapshot();

		const bool firstSample = !scene.GetInputSnapshot().Valid;

		SceneSnapshot snapshot;
		snapshot.Buttons = source.Down;
		snapshot.Axes = source.Axes;
		snapshot.ButtonsById = source.DownById;
		snapshot.AxesById = source.AxesById;
		snapshot.MousePosition = mousePosition;
		snapshot.MouseDelta = (firstSample || !state.HasPreviousMouse)
			? glm::vec2(0.0f)
			: (mousePosition - state.PreviousMouse);
		snapshot.ScrollDelta = scrollDelta.y;
		snapshot.SampledFrame = scene.GetTime().FrameCount;
		snapshot.Valid = true;
		scene.SetInputSnapshot(std::move(snapshot));

		state.PreviousMouse = mousePosition;
		state.HasPreviousMouse = true;
		state.Pressed = source.Pressed;
		state.Released = source.Released;
		state.PressedById = source.PressedById;
		state.ReleasedById = source.ReleasedById;
		++state.SampleCount;
		return scene.GetInputSnapshot();
	}

	void InputSystem::Forget(Scene& scene)
	{
		States().erase(&scene);
		scene.SetInputSnapshot(Scene::InputSnapshot{});
	}

	bool InputSystem::IsDown(const Scene& scene, NameId action)
	{
		const auto it = scene.GetInputSnapshot().ButtonsById.find(action.Value);
		return it != scene.GetInputSnapshot().ButtonsById.end() && it->second;
	}

	bool InputSystem::IsDown(const Scene& scene, const std::string& action)
	{
		const auto it = scene.GetInputSnapshot().Buttons.find(action);
		return it != scene.GetInputSnapshot().Buttons.end() && it->second;
	}

	bool InputSystem::WasPressed(const Scene& scene, NameId action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->PressedById.find(action.Value);
		return it != state->PressedById.end() && it->second;
	}

	bool InputSystem::WasPressed(const Scene& scene, const std::string& action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->Pressed.find(action);
		return it != state->Pressed.end() && it->second;
	}

	bool InputSystem::WasReleased(const Scene& scene, NameId action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->ReleasedById.find(action.Value);
		return it != state->ReleasedById.end() && it->second;
	}

	bool InputSystem::WasReleased(const Scene& scene, const std::string& action)
	{
		const SceneInputState* state = Find(scene);
		if (!state)
			return false;
		const auto it = state->Released.find(action);
		return it != state->Released.end() && it->second;
	}

	float InputSystem::GetAxis(const Scene& scene, NameId axis)
	{
		const auto it = scene.GetInputSnapshot().AxesById.find(axis.Value);
		return it != scene.GetInputSnapshot().AxesById.end() ? it->second : 0.0f;
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
