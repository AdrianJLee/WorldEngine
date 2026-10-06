#include "wldpch.h"
#include "World/UI/UiAnimator.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <utility>

namespace World::UI
{
	namespace
	{
		float LinearCurve(float t)
		{
			return std::clamp(t, 0.0f, 1.0f);
		}

		// smoothstep:两端一阶导为 0 的平滑进出。
		float EaseInOutCurve(float t)
		{
			const float x = std::clamp(t, 0.0f, 1.0f);
			return x * x * (3.0f - 2.0f * x);
		}

		using CurveEntry = std::pair<std::string, UiCurveFn>;

		std::vector<CurveEntry>& CurveTable()
		{
			static std::vector<CurveEntry> table;
			return table;
		}

		std::once_flag& CurveOnce()
		{
			static std::once_flag flag;
			return flag;
		}

		void EnsureBuiltinCurves()
		{
			std::call_once(CurveOnce(), [] {
				CurveTable().push_back(CurveEntry { kUiCurveLinear, &LinearCurve });
				CurveTable().push_back(CurveEntry { kUiCurveEaseInOut, &EaseInOutCurve });
			});
		}

		void SetError(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
		}

		// 覆盖值文本:2 位小数、去尾零/尾点(同属性文本协议;见 UiPainter 的数值读取)。
		std::string FormatAnimValue(float value)
		{
			char buffer[32] = {};
			std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
			std::string out = buffer;
			while (!out.empty() && out.back() == '0')
				out.pop_back();
			if (!out.empty() && out.back() == '.')
				out.pop_back();
			return out.empty() ? std::string("0") : out;
		}
	}

	bool UiAnimator::RegisterCurve(std::string_view name, UiCurveFn fn)
	{
		if (name.empty() || fn == nullptr)
			return false;
		EnsureBuiltinCurves();
		std::vector<CurveEntry>& table = CurveTable();
		for (CurveEntry& entry : table)
		{
			if (entry.first == name)
			{
				entry.second = fn;
				return true;
			}
		}
		table.push_back(CurveEntry { std::string(name), fn });
		return true;
	}

	UiCurveFn UiAnimator::FindCurve(std::string_view name)
	{
		EnsureBuiltinCurves();
		for (const CurveEntry& entry : CurveTable())
		{
			if (entry.first == name)
				return entry.second;
		}
		return nullptr;
	}

	// ---- 节点 ----

	bool UiAnimator::RegisterNode(std::string_view nodeId)
	{
		if (nodeId.empty())
			return false;
		if (FindNode(nodeId) != nullptr)
			return true;   // 幂等
		Node node;
		node.Id = std::string(nodeId);
		m_Nodes.push_back(std::move(node));
		return true;
	}

	bool UiAnimator::HasNode(std::string_view nodeId) const
	{
		return FindNode(nodeId) != nullptr;
	}

	UiAnimator::Node* UiAnimator::FindNodeMut(std::string_view nodeId)
	{
		for (Node& node : m_Nodes)
		{
			if (node.Id == nodeId)
				return &node;
		}
		return nullptr;
	}

	const UiAnimator::Node* UiAnimator::FindNode(std::string_view nodeId) const
	{
		for (const Node& node : m_Nodes)
		{
			if (node.Id == nodeId)
				return &node;
		}
		return nullptr;
	}

	UiAnimator::Track* UiAnimator::FindTrack(Node& node, std::string_view property)
	{
		for (Track& track : node.Tracks)
		{
			if (track.Property == property)
				return &track;
		}
		return nullptr;
	}

	const UiAnimator::Track* UiAnimator::FindTrack(const Node& node, std::string_view property)
	{
		for (const Track& track : node.Tracks)
		{
			if (track.Property == property)
				return &track;
		}
		return nullptr;
	}

	// ---- 补间 ----

	bool UiAnimator::Tween(std::string_view nodeId, std::string_view property,
		float from, float to, float duration, std::string_view curve, std::string* error)
	{
		Node* node = FindNodeMut(nodeId);
		if (node == nullptr)
		{
			SetError(error, "unknown node '" + std::string(nodeId) + "'");
			return false;
		}
		if (property.empty())
		{
			SetError(error, "property must not be empty");
			return false;
		}
		const UiCurveFn fn = FindCurve(curve);
		if (fn == nullptr)
		{
			SetError(error, "unknown curve '" + std::string(curve) + "'");
			return false;
		}

		Track* track = FindTrack(*node, property);
		if (track == nullptr)
		{
			node->Tracks.push_back(Track {});
			track = &node->Tracks.back();
			track->Property = std::string(property);
		}
		track->From = from;
		track->To = to;
		track->Duration = duration;
		track->Elapsed = 0.0f;
		track->Curve = fn;
		track->CurveName = std::string(curve);
		track->Active = duration > 0.0f;
		track->Value = duration > 0.0f ? from : to;
		return true;
	}

	// ---- 状态机 ----

	bool UiAnimator::DefineState(std::string_view nodeId, std::string_view state, std::string_view property,
		float target, float duration, std::string_view curve, std::string* error)
	{
		Node* node = FindNodeMut(nodeId);
		if (node == nullptr)
		{
			SetError(error, "unknown node '" + std::string(nodeId) + "'");
			return false;
		}
		if (state.empty())
		{
			SetError(error, "state must not be empty");
			return false;
		}
		if (property.empty())
		{
			SetError(error, "property must not be empty");
			return false;
		}
		const UiCurveFn fn = FindCurve(curve);
		if (fn == nullptr)
		{
			SetError(error, "unknown curve '" + std::string(curve) + "'");
			return false;
		}

		for (StateTarget& target0 : node->StateTargets)
		{
			if (target0.State == state && target0.Property == property)
			{
				target0.Target = target;
				target0.Duration = duration;
				target0.Curve = fn;
				target0.CurveName = std::string(curve);
				return true;   // 覆盖
			}
		}

		StateTarget entry;
		entry.State = std::string(state);
		entry.Property = std::string(property);
		entry.Target = target;
		entry.Duration = duration;
		entry.Curve = fn;
		entry.CurveName = std::string(curve);
		node->StateTargets.push_back(std::move(entry));
		return true;
	}

	bool UiAnimator::SetState(std::string_view nodeId, std::string_view state, std::string* error)
	{
		Node* node = FindNodeMut(nodeId);
		if (node == nullptr)
		{
			SetError(error, "unknown node '" + std::string(nodeId) + "'");
			return false;
		}
		if (state.empty())
		{
			SetError(error, "state must not be empty");
			return false;
		}
		node->State = std::string(state);

		for (const StateTarget& target : node->StateTargets)
		{
			if (target.State != state)
				continue;
			Track* track = FindTrack(*node, target.Property);
			if (track == nullptr)
			{
				node->Tracks.push_back(Track {});
				track = &node->Tracks.back();
				track->Property = target.Property;
				track->Value = target.Target;   // 无当前值 ⇒ 以目标为起点(不伪造运动)
			}
			track->From = track->Value;
			track->To = target.Target;
			track->Duration = target.Duration;
			track->Elapsed = 0.0f;
			track->Curve = target.Curve;
			track->CurveName = target.CurveName;
			track->Active = target.Duration > 0.0f;
			if (target.Duration <= 0.0f)
				track->Value = target.Target;
		}
		return true;
	}

	std::string_view UiAnimator::State(std::string_view nodeId) const
	{
		const Node* node = FindNode(nodeId);
		return node != nullptr ? std::string_view(node->State) : std::string_view();
	}

	// ---- 推进 ----

	void UiAnimator::Update(float dtSeconds)
	{
		const float dt = dtSeconds > 0.0f ? dtSeconds : 0.0f;   // 负 dt 不回退
		for (Node& node : m_Nodes)
		{
			for (Track& track : node.Tracks)
			{
				if (!track.Active)
					continue;
				if (track.Duration <= 0.0f)
				{
					track.Value = track.To;
					track.Active = false;
					continue;
				}
				track.Elapsed += dt;
				float t = track.Elapsed / track.Duration;
				if (t >= 1.0f)
				{
					track.Value = track.To;
					track.Active = false;
					continue;
				}
				if (t < 0.0f)
					t = 0.0f;
				const float progress = track.Curve != nullptr ? track.Curve(t) : t;
				track.Value = track.From + (track.To - track.From) * progress;
			}
		}
	}

	// ---- 查询 ----

	bool UiAnimator::Current(std::string_view nodeId, std::string_view property, float& out, std::string* error) const
	{
		const Node* node = FindNode(nodeId);
		if (node == nullptr)
		{
			SetError(error, "unknown node '" + std::string(nodeId) + "'");
			return false;
		}
		const Track* track = FindTrack(*node, property);
		if (track == nullptr)
		{
			SetError(error, "unknown property '" + std::string(property) + "' on node '" + std::string(nodeId) + "'");
			return false;
		}
		out = track->Value;
		return true;
	}

	bool UiAnimator::CurrentText(std::string_view nodeId, std::string_view property, std::string& out, std::string* error) const
	{
		float value = 0.0f;
		if (!Current(nodeId, property, value, error))
			return false;
		out = FormatAnimValue(value);
		return true;
	}

	bool UiAnimator::IsAnimating(std::string_view nodeId, std::string_view property) const
	{
		const Node* node = FindNode(nodeId);
		if (node == nullptr)
			return false;
		const Track* track = FindTrack(*node, property);
		return track != nullptr && track->Active;
	}

	void UiAnimator::Clear()
	{
		m_Nodes.clear();
	}
}
