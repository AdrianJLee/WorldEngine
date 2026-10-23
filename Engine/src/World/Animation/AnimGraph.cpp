#include "wldpch.h"

#include "World/Animation/AnimGraph.h"
#include "World/Animation/AnimGraphCondition.h"
#include "World/Renderer/Skinning.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;

		float SmoothStep(float edge0, float edge1, float x)
		{
			if (std::fabs(edge1 - edge0) <= 1e-8f)
				return x < edge0 ? 0.0f : 1.0f;
			const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		glm::quat SafeNormalize(const glm::quat& value)
		{
			const float length = glm::length(value);
			if (!std::isfinite(length) || length <= 1e-8f)
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			return value / length;
		}

		void WarnOnce(const std::string& key, const std::string& message)
		{
			static std::unordered_set<std::string> warned;
			if (warned.insert(key).second)
				WLD_CORE_WARN("{0}", message);
		}

		bool AlwaysTrueTransitionCondition(const std::unordered_map<std::string, float>&)
		{
			return true;
		}

		const Asset::WModelAnimation* FindClip(const Asset::WModelData& model, const std::string& name)
		{
			for (const Asset::WModelAnimation& animation : model.Animations)
				if (animation.Name == name)
					return &animation;
			return nullptr;
		}

		void FillBindPose(const Asset::WModelData& model, AnimPose& outPose)
		{
			const size_t nodeCount = model.Nodes.size();
			outPose.Joints.resize(nodeCount);
			for (size_t i = 0; i < nodeCount; ++i)
			{
				outPose.Joints[i].Translation = model.Nodes[i].Translation;
				outPose.Joints[i].Rotation = SafeNormalize(model.Nodes[i].Rotation);
				outPose.Joints[i].Scale = model.Nodes[i].Scale;
			}
			outPose.RootMotionDelta = glm::vec3(0.0f);
			outPose.RootRotationDelta = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		}

		void ApplyClip(const Asset::WModelAnimation& clip, float time, AnimPose& pose)
		{
			const float duration = clip.Duration;
			const float sampleTime = (duration > 0.0f) ? std::clamp(time, 0.0f, duration) : 0.0f;
			for (const AnimationSample& sample : SampleAnimation(clip, sampleTime))
			{
				if (static_cast<size_t>(sample.Node) >= pose.Joints.size())
					continue;
				auto& joint = pose.Joints[sample.Node];
				switch (sample.Path)
				{
				case Asset::WModelIO::WModelAnimationPath::Translation:
					joint.Translation = glm::vec3(sample.Value);
					break;
				case Asset::WModelIO::WModelAnimationPath::Rotation:
					joint.Rotation = SafeNormalize(glm::quat(sample.Value.w, sample.Value.x, sample.Value.y, sample.Value.z));
					break;
				case Asset::WModelIO::WModelAnimationPath::Scale:
					joint.Scale = glm::vec3(sample.Value);
					break;
				}
			}
		}

		float ClipSampleTime(const Asset::WModelAnimation& clip, float time, bool loop)
		{
			const float duration = clip.Duration;
			if (!(duration > 0.0f))
				return 0.0f;
			if (loop)
			{
				float wrapped = std::fmod(time, duration);
				if (wrapped < 0.0f)
					wrapped += duration;
				return wrapped;
			}
			return std::clamp(time, 0.0f, duration);
		}

		void SampleClipStep(const Asset::WModelData& model, const std::string& clipName,
			float time, bool loop, AnimPose& pose)
		{
			FillBindPose(model, pose);
			const Asset::WModelAnimation* clip = FindClip(model, clipName);
			if (!clip)
				return;
			ApplyClip(*clip, ClipSampleTime(*clip, time, loop), pose);
		}

		int32_t FindNodeIndex(const Asset::WModelData& model, const std::vector<std::string>& names)
		{
			for (const std::string& name : names)
			{
				for (size_t i = 0; i < model.Nodes.size(); ++i)
					if (model.Nodes[i].Name == name)
						return static_cast<int32_t>(i);
			}
			return -1;
		}

		int32_t RootNodeIndex(const Asset::WModelData& model)
		{
			if (model.Nodes.empty())
				return -1;
			int32_t index = FindNodeIndex(model, { "Root" });
			if (index < 0)
				index = FindNodeIndex(model, { "Hips" });
			return index >= 0 ? index : 0;
		}

		std::vector<glm::mat4> ComputeNodeWorld(const Asset::WModelData& model, const AnimPose& pose)
		{
			const size_t nodeCount = model.Nodes.size();
			std::vector<glm::mat4> world(nodeCount, glm::mat4(1.0f));
			std::vector<uint8_t> state(nodeCount, 0);
			const auto localMatrix = [&](size_t index)
			{
				const JointTransform& jt = (index < pose.Joints.size()) ? pose.Joints[index] : JointTransform{};
				return glm::translate(glm::mat4(1.0f), jt.Translation)
					* glm::mat4_cast(jt.Rotation)
					* glm::scale(glm::mat4(1.0f), jt.Scale);
			};
			std::function<glm::mat4(size_t)> resolve = [&](size_t index) -> glm::mat4
			{
				if (state[index] == 2)
					return world[index];
				if (state[index] == 1)
					return glm::mat4(1.0f);
				state[index] = 1;
				glm::mat4 parentWorld(1.0f);
				const int32_t parent = model.Nodes[index].Parent;
				if (parent >= 0 && static_cast<size_t>(parent) < nodeCount && static_cast<size_t>(parent) != index)
					parentWorld = resolve(static_cast<size_t>(parent));
				world[index] = parentWorld * localMatrix(index);
				state[index] = 2;
				return world[index];
			};
			for (size_t i = 0; i < nodeCount; ++i)
				resolve(i);
			return world;
		}

		std::string Trim(std::string_view value)
		{
			size_t begin = 0;
			size_t end = value.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
				--end;
			return std::string(value.substr(begin, end - begin));
		}

		std::string LowerAscii(std::string value)
		{
			for (char& ch : value)
				ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			return value;
		}

		std::vector<std::string> SplitWhitespace(const std::string& value)
		{
			std::vector<std::string> tokens;
			std::istringstream stream(value);
			std::string token;
			while (stream >> token)
				tokens.push_back(token);
			return tokens;
		}

		std::string FormatFloat(float value)
		{
			if (!std::isfinite(value))
				value = 0.0f;
			if (value == 0.0f)
				return "0";
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
			return buffer;
		}

		bool ParseFloat(const std::string& text, float& out)
		{
			if (text.empty())
				return false;
			char* end = nullptr;
			const float value = std::strtof(text.c_str(), &end);
			if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
				return false;
			out = value;
			return true;
		}

		bool ParseBool(const std::string& text, bool& out)
		{
			if (text == "true" || text == "1")
			{
				out = true;
				return true;
			}
			if (text == "false" || text == "0")
			{
				out = false;
				return true;
			}
			return false;
		}

		const char* KindToString(AnimStateKind kind)
		{
			switch (kind)
			{
			case AnimStateKind::BlendSpace1D: return "blend1d";
			case AnimStateKind::BlendSpace2D: return "blend2d";
			default: return "clip";
			}
		}

		bool KindFromString(const std::string& text, AnimStateKind& out)
		{
			if (text == "clip") { out = AnimStateKind::SingleClip; return true; }
			if (text == "blend1d") { out = AnimStateKind::BlendSpace1D; return true; }
			if (text == "blend2d") { out = AnimStateKind::BlendSpace2D; return true; }
			return false;
		}

		struct BuildTriangle
		{
			int A = 0;
			int B = 0;
			int C = 0;
		};

		double Cross2(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c)
		{
			return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
		}

		bool CircumcircleContains(const std::vector<glm::dvec2>& points, const BuildTriangle& tri, const glm::dvec2& p)
		{
			const glm::dvec2& a = points[static_cast<size_t>(tri.A)];
			const glm::dvec2& b = points[static_cast<size_t>(tri.B)];
			const glm::dvec2& c = points[static_cast<size_t>(tri.C)];
			const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
			if (std::fabs(d) <= 1e-14)
				return false;
			const double a2 = a.x * a.x + a.y * a.y;
			const double b2 = b.x * b.x + b.y * b.y;
			const double c2 = c.x * c.x + c.y * c.y;
			const double ux = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
			const double uy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
			const double dx = p.x - ux;
			const double dy = p.y - uy;
			const double radius2 = (a.x - ux) * (a.x - ux) + (a.y - uy) * (a.y - uy);
			return dx * dx + dy * dy <= radius2 + 1e-10;
		}

		struct BuildEdge
		{
			int A = 0;
			int B = 0;
		};

		bool SameEdge(const BuildEdge& lhs, const BuildEdge& rhs)
		{
			return (lhs.A == rhs.A && lhs.B == rhs.B) || (lhs.A == rhs.B && lhs.B == rhs.A);
		}

		glm::vec2 ClosestPointOnSegment(const glm::vec2& point, const glm::vec2& a, const glm::vec2& b)
		{
			const glm::vec2 delta = b - a;
			const float lengthSq = glm::dot(delta, delta);
			if (lengthSq <= 1e-12f)
				return a;
			const float t = std::clamp(glm::dot(point - a, delta) / lengthSq, 0.0f, 1.0f);
			return a + delta * t;
		}

		bool Barycentric(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
			float& outU, float& outV, float& outW)
		{
			const glm::vec2 v0 = b - a;
			const glm::vec2 v1 = c - a;
			const glm::vec2 v2 = p - a;
			const float d00 = glm::dot(v0, v0);
			const float d01 = glm::dot(v0, v1);
			const float d11 = glm::dot(v1, v1);
			const float d20 = glm::dot(v2, v0);
			const float d21 = glm::dot(v2, v1);
			const float denom = d00 * d11 - d01 * d01;
			if (std::fabs(denom) <= 1e-16f)
				return false;
			outV = (d11 * d20 - d01 * d21) / denom;
			outW = (d00 * d21 - d01 * d20) / denom;
			outU = 1.0f - outV - outW;
			return std::isfinite(outU) && std::isfinite(outV) && std::isfinite(outW);
		}
	}

	glm::mat4 JointTransform::ToMatrix() const
	{
		glm::mat4 m = glm::translate(glm::mat4(1.0f), Translation);
		m *= glm::mat4_cast(Rotation);
		m = glm::scale(m, Scale);
		return m;
	}

	JointTransform JointTransform::Lerp(const JointTransform& a, const JointTransform& b, float factor)
	{
		JointTransform out;
		out.Translation = glm::mix(a.Translation, b.Translation, factor);
		out.Rotation = glm::slerp(a.Rotation, b.Rotation, factor);
		out.Scale = glm::mix(a.Scale, b.Scale, factor);
		return out;
	}

	AnimPose AnimPose::Blend(const AnimPose& a, const AnimPose& b, float factor)
	{
		AnimPose out;
		const size_t count = std::min(a.Joints.size(), b.Joints.size());
		out.Joints.resize(count);
		for (size_t i = 0; i < count; ++i)
			out.Joints[i] = JointTransform::Lerp(a.Joints[i], b.Joints[i], factor);
		out.RootMotionDelta = glm::mix(a.RootMotionDelta, b.RootMotionDelta, factor);
		out.RootRotationDelta = glm::slerp(a.RootRotationDelta, b.RootRotationDelta, factor);
		return out;
	}

	AnimPose AnimPose::LayeredBlend(const AnimPose& basePose, const AnimPose& layerPose,
		const std::vector<float>& boneMask, float weight)
	{
		AnimPose out = basePose;
		const size_t count = std::min(basePose.Joints.size(), layerPose.Joints.size());
		for (size_t i = 0; i < count; ++i)
		{
			const float mask = (i < boneMask.size()) ? boneMask[i] : 0.0f;
			const float finalWeight = std::clamp(mask * weight, 0.0f, 1.0f);
			if (finalWeight > 0.001f)
				out.Joints[i] = JointTransform::Lerp(basePose.Joints[i], layerPose.Joints[i], finalWeight);
		}
		return out;
	}

	bool TwoBoneIK::Solve(
		const glm::vec3& rootPos,
		const glm::vec3& midPos,
		const glm::vec3& endPos,
		const glm::vec3& targetPos,
		const glm::vec3& poleVector,
		glm::quat& outRootRot,
		glm::quat& outMidRot)
	{
		const float d1 = glm::distance(rootPos, midPos);
		const float d2 = glm::distance(midPos, endPos);
		if (d1 <= 0.001f || d2 <= 0.001f)
			return false;

		const float maxReach = d1 + d2 - 0.001f;
		const float targetDist = std::clamp(glm::distance(rootPos, targetPos), 0.001f, maxReach);

		const float cosAlpha = std::clamp((d1 * d1 + targetDist * targetDist - d2 * d2) / (2.0f * d1 * targetDist), -1.0f, 1.0f);
		const float cosBeta = std::clamp((d1 * d1 + d2 * d2 - targetDist * targetDist) / (2.0f * d1 * d2), -1.0f, 1.0f);

		const float angleAlpha = std::acos(cosAlpha);
		const float angleBeta = std::acos(cosBeta);

		const glm::vec3 toTarget = glm::normalize(targetPos - rootPos);
		glm::vec3 bendNormal = glm::cross(toTarget, glm::normalize(poleVector - rootPos));
		if (glm::length(bendNormal) < 0.001f)
			bendNormal = glm::vec3(0.0f, 0.0f, 1.0f);
		else
			bendNormal = glm::normalize(bendNormal);

		outRootRot = glm::angleAxis(angleAlpha, bendNormal);
		outMidRot = glm::angleAxis(kPi - angleBeta, bendNormal);
		return true;
	}

	void FootIK::SolveTerrainPlacement(
		const TerrainData& terrain,
		const glm::vec3& characterPos,
		const glm::vec3& characterForward,
		float footSpacing,
		float& outPelvisYOffset,
		float& outLeftFootYOffset,
		float& outRightFootYOffset,
		glm::vec3& outLeftNormal,
		glm::vec3& outRightNormal)
	{
		glm::vec3 forward = characterForward;
		if (glm::length(forward) < 1e-5f)
			forward = glm::vec3(0.0f, 0.0f, 1.0f);
		const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward));

		const glm::vec3 leftFootPos = characterPos + right * (footSpacing * 0.5f);
		const glm::vec3 rightFootPos = characterPos - right * (footSpacing * 0.5f);

		const float leftH = terrain.SampleHeight(leftFootPos.x, leftFootPos.z);
		const float rightH = terrain.SampleHeight(rightFootPos.x, rightFootPos.z);

		outLeftNormal = terrain.SampleNormal(leftFootPos.x, leftFootPos.z);
		outRightNormal = terrain.SampleNormal(rightFootPos.x, rightFootPos.z);

		const float minH = std::min(leftH, rightH);
		outPelvisYOffset = std::min(0.0f, minH - characterPos.y);
		outLeftFootYOffset = leftH - (characterPos.y + outPelvisYOffset);
		outRightFootYOffset = rightH - (characterPos.y + outPelvisYOffset);
	}

	void BlendSpace1D::AddSample(float value, const std::string& clipName)
	{
		BlendSample1D sample;
		sample.Value = value;
		sample.ClipName = clipName;
		m_Samples.push_back(sample);
		std::sort(m_Samples.begin(), m_Samples.end(),
			[](const BlendSample1D& a, const BlendSample1D& b) { return a.Value < b.Value; });
	}

	void BlendSpace1D::Evaluate(float param, size_t& outIdx0, size_t& outIdx1, float& outFactor) const
	{
		if (m_Samples.empty())
		{
			outIdx0 = outIdx1 = 0;
			outFactor = 0.0f;
			return;
		}
		if (!std::isfinite(param))
			param = m_Samples.front().Value;
		if (m_Samples.size() == 1 || param <= m_Samples.front().Value)
		{
			outIdx0 = outIdx1 = 0;
			outFactor = 0.0f;
			return;
		}
		if (param >= m_Samples.back().Value)
		{
			outIdx0 = outIdx1 = m_Samples.size() - 1;
			outFactor = 0.0f;
			return;
		}
		for (size_t i = 0; i + 1 < m_Samples.size(); ++i)
		{
			if (param >= m_Samples[i].Value && param <= m_Samples[i + 1].Value)
			{
				outIdx0 = i;
				outIdx1 = i + 1;
				const float range = m_Samples[i + 1].Value - m_Samples[i].Value;
				outFactor = (range > 1e-4f) ? ((param - m_Samples[i].Value) / range) : 0.0f;
				return;
			}
		}
		outIdx0 = outIdx1 = 0;
		outFactor = 0.0f;
	}

	void BlendSpace2D::AddSample(const glm::vec2& point, const std::string& clipName)
	{
		BlendSample2D sample;
		sample.Point = point;
		sample.ClipName = clipName;
		m_Samples.push_back(sample);
		m_TrianglesDirty = true;
	}

	const std::vector<BlendTriangle2D>& BlendSpace2D::GetTriangles() const
	{
		if (m_TrianglesDirty)
			RebuildTriangulation();
		return m_Triangles;
	}

	void BlendSpace2D::RebuildTriangulation() const
	{
		m_Triangles.clear();
		m_TrianglesDirty = false;

		const size_t count = m_Samples.size();
		if (count < 3)
			return;

		std::vector<size_t> unique;
		unique.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			bool duplicate = false;
			for (size_t j : unique)
			{
				const glm::vec2 d = m_Samples[i].Point - m_Samples[j].Point;
				if (glm::dot(d, d) <= 1e-12f)
				{
					duplicate = true;
					break;
				}
			}
			if (!duplicate)
				unique.push_back(i);
		}
		if (unique.size() < 3)
			return;

		glm::vec2 minPoint = m_Samples[unique[0]].Point;
		glm::vec2 maxPoint = minPoint;
		for (size_t i : unique)
		{
			minPoint = glm::min(minPoint, m_Samples[i].Point);
			maxPoint = glm::max(maxPoint, m_Samples[i].Point);
		}

		bool collinear = true;
		for (size_t k = 2; k < unique.size(); ++k)
		{
			const glm::vec2 a = m_Samples[unique[0]].Point;
			const glm::vec2 b = m_Samples[unique[1]].Point;
			const glm::vec2 c = m_Samples[unique[k]].Point;
			const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
			if (std::fabs(area) > 1e-8f)
			{
				collinear = false;
				break;
			}
		}
		if (collinear)
			return;

		std::vector<glm::dvec2> points;
		points.reserve(unique.size() + 3);
		for (size_t i : unique)
			points.emplace_back(static_cast<double>(m_Samples[i].Point.x), static_cast<double>(m_Samples[i].Point.y));

		const double extent = static_cast<double>(std::max(maxPoint.x - minPoint.x, maxPoint.y - minPoint.y));
		const double radius = std::max(1.0, extent * 10.0);
		const glm::dvec2 center(static_cast<double>(minPoint.x + maxPoint.x) * 0.5,
			static_cast<double>(minPoint.y + maxPoint.y) * 0.5);
		const int baseCount = static_cast<int>(points.size());
		points.push_back(center + glm::dvec2(0.0, radius * 3.0));
		points.push_back(center + glm::dvec2(-radius * 2.6, -radius * 1.5));
		points.push_back(center + glm::dvec2(radius * 2.6, -radius * 1.5));

		std::vector<BuildTriangle> triangles;
		triangles.push_back({ baseCount, baseCount + 1, baseCount + 2 });

		for (int index = 0; index < baseCount; ++index)
		{
			const glm::dvec2& point = points[static_cast<size_t>(index)];
			std::vector<BuildTriangle> kept;
			std::vector<BuildEdge> boundary;
			kept.reserve(triangles.size());
			for (const BuildTriangle& tri : triangles)
			{
				if (CircumcircleContains(points, tri, point))
				{
					boundary.push_back({ tri.A, tri.B });
					boundary.push_back({ tri.B, tri.C });
					boundary.push_back({ tri.C, tri.A });
				}
				else
				{
					kept.push_back(tri);
				}
			}

			std::vector<BuildEdge> hullEdges;
			hullEdges.reserve(boundary.size());
			std::vector<bool> consumed(boundary.size(), false);
			for (size_t i = 0; i < boundary.size(); ++i)
			{
				if (consumed[i])
					continue;
				bool shared = false;
				for (size_t j = i + 1; j < boundary.size(); ++j)
				{
					if (!consumed[j] && SameEdge(boundary[i], boundary[j]))
					{
						consumed[i] = true;
						consumed[j] = true;
						shared = true;
						break;
					}
				}
				if (!shared)
					hullEdges.push_back(boundary[i]);
			}

			for (const BuildEdge& edge : hullEdges)
			{
				const glm::dvec2& a = points[static_cast<size_t>(edge.A)];
				const glm::dvec2& b = points[static_cast<size_t>(edge.B)];
				if (std::fabs(Cross2(a, b, point)) <= 1e-14)
					continue;
				kept.push_back({ edge.A, edge.B, index });
			}
			triangles.swap(kept);
		}

		std::set<std::tuple<int, int, int>> dedupe;
		for (const BuildTriangle& tri : triangles)
		{
			if (tri.A >= baseCount || tri.B >= baseCount || tri.C >= baseCount)
				continue;
			int a = tri.A;
			int b = tri.B;
			int c = tri.C;
			if (a > b) std::swap(a, b);
			if (b > c) std::swap(b, c);
			if (a > b) std::swap(a, b);

			const glm::dvec2& pa = points[static_cast<size_t>(tri.A)];
			const glm::dvec2& pb = points[static_cast<size_t>(tri.B)];
			const glm::dvec2& pc = points[static_cast<size_t>(tri.C)];
			if (std::fabs(Cross2(pa, pb, pc)) <= 1e-12)
				continue;
			if (!dedupe.insert({ a, b, c }).second)
				continue;
			m_Triangles.push_back({ static_cast<uint32_t>(unique[static_cast<size_t>(tri.A)]),
				static_cast<uint32_t>(unique[static_cast<size_t>(tri.B)]),
				static_cast<uint32_t>(unique[static_cast<size_t>(tri.C)]) });
		}
	}

	void BlendSpace2D::Evaluate(const glm::vec2& param, std::vector<std::pair<size_t, float>>& outWeights) const
	{
		outWeights.clear();
		if (m_Samples.empty())
			return;
		if (m_Samples.size() == 1)
		{
			outWeights.push_back({ 0, 1.0f });
			return;
		}

		glm::vec2 query = param;
		if (!std::isfinite(query.x) || !std::isfinite(query.y))
			query = m_Samples.front().Point;

		if (m_TrianglesDirty)
			RebuildTriangulation();

		const auto pushNormalized = [&](std::vector<std::pair<size_t, float>>& weights)
		{
			float sum = 0.0f;
			for (auto& entry : weights)
			{
				if (!std::isfinite(entry.second) || entry.second < 0.0f)
					entry.second = 0.0f;
				sum += entry.second;
			}
			if (sum <= 1e-8f || !std::isfinite(sum))
			{
				weights.clear();
				size_t nearest = 0;
				float best = std::numeric_limits<float>::max();
				for (size_t i = 0; i < m_Samples.size(); ++i)
				{
					const float d = glm::distance(query, m_Samples[i].Point);
					if (d < best)
					{
						best = d;
						nearest = i;
					}
				}
				weights.push_back({ nearest, 1.0f });
				return;
			}
			for (auto& entry : weights)
				entry.second /= sum;
			weights.erase(std::remove_if(weights.begin(), weights.end(),
				[](const std::pair<size_t, float>& entry) { return entry.second <= 1e-6f; }), weights.end());
		};

		if (m_Triangles.empty())
		{
			size_t nearestEdgeA = 0;
			size_t nearestEdgeB = 1;
			float bestDistance = std::numeric_limits<float>::max();
			glm::vec2 bestPoint = m_Samples[0].Point;
			bool found = false;
			for (size_t i = 0; i < m_Samples.size(); ++i)
			{
				for (size_t j = i + 1; j < m_Samples.size(); ++j)
				{
					const glm::vec2 a = m_Samples[i].Point;
					const glm::vec2 b = m_Samples[j].Point;
					const glm::vec2 q = ClosestPointOnSegment(query, a, b);
					const float d = glm::distance(query, q);
					if (d < bestDistance)
					{
						bestDistance = d;
						nearestEdgeA = i;
						nearestEdgeB = j;
						bestPoint = q;
						found = true;
					}
				}
			}
			if (!found)
			{
				outWeights.push_back({ 0, 1.0f });
				return;
			}
			const glm::vec2 a = m_Samples[nearestEdgeA].Point;
			const glm::vec2 b = m_Samples[nearestEdgeB].Point;
			const glm::vec2 delta = b - a;
			const float lengthSq = glm::dot(delta, delta);
			const float t = (lengthSq > 1e-12f) ? std::clamp(glm::dot(bestPoint - a, delta) / lengthSq, 0.0f, 1.0f) : 0.0f;
			outWeights.push_back({ nearestEdgeA, 1.0f - t });
			outWeights.push_back({ nearestEdgeB, t });
			pushNormalized(outWeights);
			return;
		}

		for (const BlendTriangle2D& tri : m_Triangles)
		{
			const glm::vec2& a = m_Samples[tri.A].Point;
			const glm::vec2& b = m_Samples[tri.B].Point;
			const glm::vec2& c = m_Samples[tri.C].Point;
			float u = 0.0f, v = 0.0f, w = 0.0f;
			if (!Barycentric(query, a, b, c, u, v, w))
				continue;
			if (u >= -1e-5f && v >= -1e-5f && w >= -1e-5f)
			{
				outWeights.push_back({ tri.A, std::max(0.0f, u) });
				outWeights.push_back({ tri.B, std::max(0.0f, v) });
				outWeights.push_back({ tri.C, std::max(0.0f, w) });
				pushNormalized(outWeights);
				return;
			}
		}

		float bestDistance = std::numeric_limits<float>::max();
		size_t bestEdgeA = 0;
		size_t bestEdgeB = 0;
		glm::vec2 bestPoint = query;
		for (const BlendTriangle2D& tri : m_Triangles)
		{
			const uint32_t indices[3][2] = { { tri.A, tri.B }, { tri.B, tri.C }, { tri.C, tri.A } };
			for (const auto& edge : indices)
			{
				const glm::vec2& a = m_Samples[edge[0]].Point;
				const glm::vec2& b = m_Samples[edge[1]].Point;
				const glm::vec2 q = ClosestPointOnSegment(query, a, b);
				const float d = glm::distance(query, q);
				if (d < bestDistance)
				{
					bestDistance = d;
					bestEdgeA = edge[0];
					bestEdgeB = edge[1];
					bestPoint = q;
				}
			}
		}

		const glm::vec2 a = m_Samples[bestEdgeA].Point;
		const glm::vec2 b = m_Samples[bestEdgeB].Point;
		const glm::vec2 delta = b - a;
		const float lengthSq = glm::dot(delta, delta);
		const float t = (lengthSq > 1e-12f) ? std::clamp(glm::dot(bestPoint - a, delta) / lengthSq, 0.0f, 1.0f) : 0.0f;
		outWeights.push_back({ bestEdgeA, 1.0f - t });
		outWeights.push_back({ bestEdgeB, t });
		pushNormalized(outWeights);
	}

	AnimGraph::AnimGraph() = default;

	void AnimGraph::Clear()
	{
		m_States.clear();
		m_CurrentStateName.clear();
		m_InitialStateName.clear();
		m_TransitionTargetState.clear();
		m_StateTime = 0.0f;
		m_TransitionTargetTime = 0.0f;
		m_TransitionProgress = 0.0f;
		m_InTransition = false;
		m_Layers.clear();
		m_ModelHint.clear();
		m_GraphName.clear();
		m_ParameterDefaults.clear();
		m_ParameterDefinitions.clear();
		m_Parameters.clear();
		m_LastRootPos = glm::vec3(0.0f);
		m_LastRootRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		m_HaveRootSample = false;
	}

	void AnimGraph::EnsureTransitionConditions()
	{
		for (auto& entry : m_States)
		{
			for (AnimTransition& transition : entry.second.Transitions)
			{
				const std::string expression = Trim(transition.ConditionExpression);
				if (expression.empty())
				{
					// 空条件在编辑器中表示“始终触发”，必须有可调用谓词才能参与状态切换。
					if (!transition.Condition)
						transition.Condition = AlwaysTrueTransitionCondition;
					continue;
				}
				if (transition.Condition)
					continue;
				std::string error;
				transition.Condition = AnimGraphCondition::Parse(expression, &error);
				if (!transition.Condition)
				{
					WarnOnce("animgraph-condition:" + expression,
						"AnimGraph 条件解析失败（转换永不触发）: '" + expression + "' — " + error);
				}
			}
		}
	}

	void AnimGraph::AddState(const AnimState& state)
	{
		m_States[state.Name] = state;
		if (m_CurrentStateName.empty())
			m_CurrentStateName = state.Name;
		if (m_InitialStateName.empty())
			m_InitialStateName = state.Name;
		EnsureTransitionConditions();
	}

	void AnimGraph::SetInitialState(const std::string& stateName)
	{
		m_InitialStateName = stateName;
		m_CurrentStateName = stateName;
		m_StateTime = 0.0f;
		m_TransitionTargetTime = 0.0f;
		m_TransitionProgress = 0.0f;
		m_InTransition = false;
		m_TransitionTargetState.clear();
		m_HaveRootSample = false;
	}

	const AnimState* AnimGraph::FindState(const std::string& name) const
	{
		const auto it = m_States.find(name);
		return it == m_States.end() ? nullptr : &it->second;
	}

	bool AnimGraph::RemoveState(const std::string& name)
	{
		const bool erased = m_States.erase(name) > 0;
		if (erased && m_CurrentStateName == name)
		{
			m_CurrentStateName.clear();
			m_InTransition = false;
			m_TransitionTargetState.clear();
		}
		if (erased && m_TransitionTargetState == name)
		{
			m_InTransition = false;
			m_TransitionTargetState.clear();
		}
		return erased;
	}

	void AnimGraph::SetFloat(const std::string& name, float value)
	{
		m_Parameters[name] = std::isfinite(value) ? value : 0.0f;
	}

	float AnimGraph::GetFloat(const std::string& name, float fallback) const
	{
		const auto it = m_Parameters.find(name);
		return (it != m_Parameters.end()) ? it->second : fallback;
	}

	void AnimGraph::SetBool(const std::string& name, bool value)
	{
		m_Parameters[name] = value ? 1.0f : 0.0f;
	}

	bool AnimGraph::GetBool(const std::string& name, bool fallback) const
	{
		const auto it = m_Parameters.find(name);
		return (it != m_Parameters.end()) ? (it->second > 0.5f) : fallback;
	}

	void AnimGraph::SetParameterDefault(const std::string& name, float value)
	{
		const std::string trimmed = Trim(name);
		if (trimmed.empty())
			return;

		AnimParameterDefinition definition;
		const auto it = m_ParameterDefinitions.find(trimmed);
		if (it != m_ParameterDefinitions.end())
		{
			definition = it->second;
		}
		else
		{
			definition.Name = trimmed;
			definition.Type = AnimParameterType::Float;
			definition.MinValue = 0.0f;
			definition.MaxValue = 1.0f;
		}
		definition.DefaultValue = value;
		SetParameterDefinition(definition);
	}

	void AnimGraph::SetParameterDefinition(const AnimParameterDefinition& definition)
	{
		const std::string name = Trim(definition.Name);
		if (name.empty())
			return;

		AnimParameterDefinition normalized = definition;
		normalized.Name = name;
		normalized.Unit = Trim(normalized.Unit);
		if (normalized.Type != AnimParameterType::Bool)
			normalized.Type = AnimParameterType::Float;

		if (normalized.Type == AnimParameterType::Bool)
		{
			normalized.DefaultValue = (std::isfinite(normalized.DefaultValue) && normalized.DefaultValue > 0.5f) ? 1.0f : 0.0f;
			normalized.MinValue = 0.0f;
			normalized.MaxValue = 1.0f;
			normalized.Unit.clear();
		}
		else
		{
			if (!std::isfinite(normalized.MinValue) || !std::isfinite(normalized.MaxValue))
			{
				normalized.MinValue = 0.0f;
				normalized.MaxValue = 1.0f;
			}
			else if (normalized.MinValue > normalized.MaxValue)
			{
				std::swap(normalized.MinValue, normalized.MaxValue);
			}
			if (!std::isfinite(normalized.DefaultValue))
				normalized.DefaultValue = 0.0f;
			normalized.DefaultValue = std::clamp(normalized.DefaultValue, normalized.MinValue, normalized.MaxValue);
		}

		m_ParameterDefinitions[name] = normalized;
		m_ParameterDefaults[name] = normalized.DefaultValue;
		m_Parameters[name] = normalized.DefaultValue;
	}

	bool AnimGraph::RemoveParameterDefinition(const std::string& name)
	{
		const std::string trimmed = Trim(name);
		if (trimmed.empty())
			return false;

		const bool removed = m_ParameterDefinitions.erase(trimmed) > 0;
		m_ParameterDefaults.erase(trimmed);
		m_Parameters.erase(trimmed);
		return removed;
	}

	const AnimGraph::AnimParameterDefinition* AnimGraph::FindParameterDefinition(const std::string& name) const
	{
		const std::string trimmed = Trim(name);
		const auto it = m_ParameterDefinitions.find(trimmed);
		return it == m_ParameterDefinitions.end() ? nullptr : &it->second;
	}

	AnimGraph::AnimParameterType AnimGraph::GetParameterType(const std::string& name,
		AnimParameterType fallback) const
	{
		const AnimParameterDefinition* definition = FindParameterDefinition(name);
		return definition ? definition->Type : fallback;
	}

	void AnimGraph::Update(float deltaSeconds)
	{
		if (m_CurrentStateName.empty())
			return;
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			deltaSeconds = 0.0f;

		EnsureTransitionConditions();

		const auto itState = m_States.find(m_CurrentStateName);
		if (itState == m_States.end())
			return;

		m_StateTime += deltaSeconds * itState->second.Speed;

		if (m_InTransition)
		{
			m_TransitionElapsed += deltaSeconds;
			const auto itTarget = m_States.find(m_TransitionTargetState);
			const float targetSpeed = (itTarget != m_States.end()) ? itTarget->second.Speed : 1.0f;
			m_TransitionTargetTime += deltaSeconds * targetSpeed;
			m_TransitionProgress = std::clamp(m_TransitionElapsed / std::max(m_TransitionDuration, 0.001f), 0.0f, 1.0f);
			if (m_TransitionProgress >= 1.0f)
			{
				m_CurrentStateName = m_TransitionTargetState;
				m_StateTime = m_TransitionTargetTime;
				m_InTransition = false;
				m_TransitionProgress = 0.0f;
				m_TransitionElapsed = 0.0f;
				m_TransitionTargetState.clear();
			}
		}
		else
		{
			for (const AnimTransition& transition : itState->second.Transitions)
			{
				if (!transition.Condition || !transition.Condition(m_Parameters))
					continue;
				if (transition.TargetState.empty() || m_States.find(transition.TargetState) == m_States.end())
					continue;
				const auto itTarget = m_States.find(transition.TargetState);
				const float targetSpeed = (itTarget != m_States.end()) ? itTarget->second.Speed : 1.0f;
				m_InTransition = true;
				m_TransitionTargetState = transition.TargetState;
				m_TransitionDuration = std::max(transition.Duration, 0.001f);
				// 起始过渡的这一帧也算进度(否则面板在触发帧显示 0%，与实际混合姿态不一致)。
				m_TransitionElapsed = deltaSeconds;
				m_TransitionTargetTime = deltaSeconds * targetSpeed;
				m_TransitionProgress = std::clamp(m_TransitionElapsed / m_TransitionDuration, 0.0f, 1.0f);
				break;
			}
		}
	}
	void AnimGraph::SampleStatePose(const AnimState& state, float time, const Asset::WModelData& model, AnimPose& outPose) const
	{
		FillBindPose(model, outPose);

		const bool hasBlend1D = state.Blend1D.GetSampleCount() > 0;
		const bool hasBlend2D = state.Blend2D.GetSampleCount() > 0;
		if (state.Kind == AnimStateKind::SingleClip || (!hasBlend1D && !hasBlend2D))
		{
			const Asset::WModelAnimation* clip = FindClip(model, state.ClipName);
			if (!clip && !model.Animations.empty() && state.ClipName.empty())
				clip = &model.Animations.front();
			if (clip)
				ApplyClip(*clip, ClipSampleTime(*clip, time, state.Loop), outPose);
			return;
		}

		std::vector<std::pair<size_t, float>> weights;
		if (state.Kind == AnimStateKind::BlendSpace1D)
		{
			size_t idx0 = 0;
			size_t idx1 = 0;
			float factor = 0.0f;
			state.Blend1D.Evaluate(GetFloat(state.ParameterX, 0.0f), idx0, idx1, factor);
			const auto& samples = state.Blend1D.GetSamples();
			if (samples.empty())
				return;
			if (idx0 >= samples.size())
				idx0 = samples.size() - 1;
			if (idx1 >= samples.size())
				idx1 = samples.size() - 1;
			weights.push_back({ idx0, 1.0f - factor });
			if (idx1 != idx0 && factor > 1e-6f)
				weights.push_back({ idx1, factor });
		}
		else
		{
			state.Blend2D.Evaluate(glm::vec2(GetFloat(state.ParameterX, 0.0f), GetFloat(state.ParameterY, 0.0f)), weights);
		}

		if (weights.empty())
			return;

		const size_t nodeCount = model.Nodes.size();
		std::vector<JointTransform> accumulator(nodeCount);
		for (JointTransform& joint : accumulator)
		{
			joint.Translation = glm::vec3(0.0f);
			joint.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			joint.Scale = glm::vec3(0.0f);
		}
		float totalWeight = 0.0f;
		bool first = true;

		for (const auto& entry : weights)
		{
			const size_t sampleIndex = entry.first;
			const float weight = entry.second;
			if (!(weight > 1e-6f))
				continue;
			const std::string* clipName = nullptr;
			if (state.Kind == AnimStateKind::BlendSpace1D)
			{
				const auto& samples = state.Blend1D.GetSamples();
				if (sampleIndex < samples.size())
					clipName = &samples[sampleIndex].ClipName;
			}
			else
			{
				const auto& samples = state.Blend2D.GetSamples();
				if (sampleIndex < samples.size())
					clipName = &samples[sampleIndex].ClipName;
			}
			if (!clipName)
				continue;
			const Asset::WModelAnimation* clip = FindClip(model, *clipName);
			if (!clip)
			{
				WarnOnce("animgraph-clip:" + *clipName, "AnimGraph 找不到动画片段 '" + *clipName + "'（按绑定姿态混合）");
				continue;
			}

			AnimPose samplePose;
			FillBindPose(model, samplePose);
			ApplyClip(*clip, ClipSampleTime(*clip, time, state.Loop), samplePose);

			const float denom = totalWeight + weight;
			const float blendFactor = denom > 1e-8f ? weight / denom : 0.0f;
			for (size_t i = 0; i < nodeCount; ++i)
			{
				accumulator[i].Translation += samplePose.Joints[i].Translation * weight;
				accumulator[i].Scale += samplePose.Joints[i].Scale * weight;
				accumulator[i].Rotation = first
					? samplePose.Joints[i].Rotation
					: SafeNormalize(glm::slerp(accumulator[i].Rotation, samplePose.Joints[i].Rotation, blendFactor));
			}
			first = false;
			totalWeight += weight;
		}

		if (totalWeight <= 1e-6f)
			return;

		for (size_t i = 0; i < nodeCount; ++i)
		{
			outPose.Joints[i].Translation = accumulator[i].Translation / totalWeight;
			outPose.Joints[i].Scale = accumulator[i].Scale / totalWeight;
			outPose.Joints[i].Rotation = SafeNormalize(accumulator[i].Rotation);
		}
	}

	bool AnimGraph::EvaluatePose(const Asset::WModelData& model, AnimPose& outPose)
	{
		outPose.Joints.clear();
		outPose.RootMotionDelta = glm::vec3(0.0f);
		outPose.RootRotationDelta = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		if (m_CurrentStateName.empty() || model.Nodes.empty())
			return false;
		const AnimState* state = FindState(m_CurrentStateName);
		if (!state)
			return false;

		AnimPose basePose;
		SampleStatePose(*state, m_StateTime, model, basePose);

		AnimPose finalPose = basePose;
		if (m_InTransition)
		{
			if (const AnimState* target = FindState(m_TransitionTargetState))
			{
				AnimPose targetPose;
				SampleStatePose(*target, m_TransitionTargetTime, model, targetPose);
				const float smoothT = SmoothStep(0.0f, 1.0f, m_TransitionProgress);
				finalPose = AnimPose::Blend(basePose, targetPose, smoothT);
			}
		}

		for (const AnimLayer& layer : m_Layers)
		{
			// 未声明权重参数 = 常开（常量 1）；声明了参数则由参数（AnimGraphSystem 可驱动）控制。
			const float weight = layer.WeightParameter.empty()
				? 1.0f
				: std::clamp(GetFloat(layer.WeightParameter, 0.0f), 0.0f, 1.0f);
			if (weight <= 0.0f)
				continue;
			AnimPose layerPose;
			SampleClipStep(model, layer.ClipName, m_StateTime, true, layerPose);
			std::vector<float> mask(finalPose.Joints.size(), 0.0f);
			for (const BoneMaskEntry& entry : layer.BoneMask)
			{
				const int32_t index = FindNodeIndex(model, { entry.BoneName });
				if (index >= 0 && static_cast<size_t>(index) < mask.size())
					mask[static_cast<size_t>(index)] = std::clamp(entry.Weight, 0.0f, 1.0f);
			}
			finalPose = AnimPose::LayeredBlend(finalPose, layerPose, mask, weight);
		}

		outPose = std::move(finalPose);

		const int32_t rootIndex = RootNodeIndex(model);
		if (rootIndex >= 0 && static_cast<size_t>(rootIndex) < outPose.Joints.size())
		{
			const glm::vec3 currentPos = outPose.Joints[static_cast<size_t>(rootIndex)].Translation;
			const glm::quat currentRot = SafeNormalize(outPose.Joints[static_cast<size_t>(rootIndex)].Rotation);
			if (m_HaveRootSample)
			{
				glm::vec3 delta = currentPos - m_LastRootPos;
				const bool verticalExempt =
					(state->ClipName == "Jump" || state->ClipName == "Fall" || state->Name == "Jump" || state->Name == "Fall") ||
					(m_InTransition && (m_TransitionTargetState == "Jump" || m_TransitionTargetState == "Fall"));
				if (verticalExempt)
					delta.y = 0.0f;
				if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z))
					delta = glm::vec3(0.0f);
				outPose.RootMotionDelta = delta;
				outPose.RootRotationDelta = SafeNormalize(currentRot * glm::conjugate(m_LastRootRot));
			}
			m_LastRootPos = currentPos;
			m_LastRootRot = currentRot;
			m_HaveRootSample = true;
		}

		return true;
	}

	bool AnimGraph::BuildPalette(const Asset::WModelData& model, uint32_t skinIndex,
		const AnimPose& pose, std::vector<glm::mat4>& outPalette) const
	{
		outPalette.clear();
		if (static_cast<size_t>(skinIndex) >= model.Skins.size())
			return false;
		const Asset::WModelSkin& skin = model.Skins[skinIndex];
		if (skin.JointNodes.empty() || skin.InverseBindMatrices.size() < skin.JointNodes.size())
			return false;

		const size_t nodeCount = model.Nodes.size();
		const std::vector<glm::mat4> nodeWorld = ComputeNodeWorld(model, pose);

		glm::mat4 meshWorld(1.0f);
		for (size_t index = 0; index < nodeCount; ++index)
		{
			const int32_t meshIndex = model.Nodes[index].MeshIndex;
			if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= model.Meshes.size())
				continue;
			if (model.Meshes[static_cast<size_t>(meshIndex)].SkinIndex != static_cast<int32_t>(skinIndex))
				continue;
			meshWorld = nodeWorld[index];
			break;
		}

		outPalette = BuildJointMatrices(skin, nodeWorld, meshWorld);
		return !outPalette.empty();
	}

	bool AnimGraph::EvaluatePalette(const Asset::WModelData& model, uint32_t skinIndex,
		std::vector<glm::mat4>& outPalette, glm::vec3& outRootMotionDelta)
	{
		outPalette.clear();
		outRootMotionDelta = glm::vec3(0.0f);

		AnimPose pose;
		if (!EvaluatePose(model, pose))
			return false;
		outRootMotionDelta = pose.RootMotionDelta;
		return BuildPalette(model, skinIndex, pose, outPalette);
	}

	// =========================================================================
	// .wanimgraph v1 读写
	// =========================================================================

	bool AnimGraph::SaveToFile(const std::string& path) const
	{
		return SaveToFile(path, nullptr);
	}

	bool AnimGraph::SaveToFile(const std::string& path, std::string* error) const
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
		{
			if (error)
				*error = "无法写入 '" + path + "'";
			return false;
		}

		stream << "version: 1\n";
		if (!m_GraphName.empty())
			stream << "name: " << m_GraphName << "\n";
		if (!m_ModelHint.empty())
			stream << "model: " << m_ModelHint << "\n";
		const std::string& initialState = m_InitialStateName.empty() ? m_CurrentStateName : m_InitialStateName;
		if (!initialState.empty())
			stream << "initial: " << initialState << "\n";
		for (const auto& entry : m_ParameterDefinitions)
		{
			const AnimParameterDefinition& definition = entry.second;
			stream << "parameter: " << entry.first << " "
				<< (definition.Type == AnimParameterType::Bool ? "bool" : "float") << " "
				<< FormatFloat(definition.DefaultValue) << " "
				<< FormatFloat(definition.MinValue) << " "
				<< FormatFloat(definition.MaxValue);
			if (!definition.Unit.empty())
				stream << " " << definition.Unit;
			stream << "\n";
		}

		for (const auto& stateEntry : m_States)
		{
			const AnimState& state = stateEntry.second;
			stream << "\nstate " << state.Name << "\n";
			stream << "  kind: " << KindToString(state.Kind) << "\n";
			if (state.Kind == AnimStateKind::SingleClip)
				stream << "  clip: " << state.ClipName << "\n";
			if (state.Kind != AnimStateKind::SingleClip)
				stream << "  x: " << state.ParameterX << "\n";
			if (state.Kind == AnimStateKind::BlendSpace2D)
				stream << "  y: " << state.ParameterY << "\n";
			if (state.Kind == AnimStateKind::SingleClip)
			{
				stream << "  loop: " << (state.Loop ? "true" : "false") << "\n";
				stream << "  speed: " << FormatFloat(state.Speed) << "\n";
			}
			else if (state.Kind == AnimStateKind::BlendSpace1D)
			{
				stream << "  speed: " << FormatFloat(state.Speed) << "\n";
				for (const BlendSample1D& sample : state.Blend1D.GetSamples())
					stream << "  sample: " << FormatFloat(sample.Value) << " " << sample.ClipName << "\n";
			}
			else
			{
				stream << "  speed: " << FormatFloat(state.Speed) << "\n";
				for (const BlendSample2D& sample : state.Blend2D.GetSamples())
					stream << "  sample: " << FormatFloat(sample.Point.x) << " " << FormatFloat(sample.Point.y)
						<< " " << sample.ClipName << "\n";
			}
			for (const AnimTransition& transition : state.Transitions)
			{
				const std::string expression = Trim(transition.ConditionExpression);
				stream << "  transition: " << transition.TargetState << " " << FormatFloat(transition.Duration)
					<< (expression.empty() ? "\n" : " " + expression + "\n");
			}
		}

		std::vector<const AnimLayer*> sortedLayers;
		sortedLayers.reserve(m_Layers.size());
		for (const AnimLayer& layer : m_Layers)
			sortedLayers.push_back(&layer);
		std::stable_sort(sortedLayers.begin(), sortedLayers.end(),
			[](const AnimLayer* lhs, const AnimLayer* rhs) { return lhs->Name < rhs->Name; });

		for (const AnimLayer* layerPtr : sortedLayers)
		{
			const AnimLayer& layer = *layerPtr;
			stream << "\nlayer " << layer.Name << "\n";
			stream << "  clip: " << layer.ClipName << "\n";
			stream << "  weight: " << layer.WeightParameter << "\n";
			stream << "  mask:";
			for (size_t i = 0; i < layer.BoneMask.size(); ++i)
			{
				if (i > 0)
					stream << ",";
				stream << " " << layer.BoneMask[i].BoneName << " " << FormatFloat(layer.BoneMask[i].Weight);
			}
			if (layer.BoneMask.empty())
				stream << " ";
			stream << "\n";
		}

		if (!stream.good())
		{
			if (error)
				*error = "写入 '" + path + "' 失败";
			return false;
		}
		if (error)
			error->clear();
		return true;
	}

	bool AnimGraph::LoadFromFile(const std::string& path)
	{
		return LoadFromFile(path, nullptr);
	}

	bool AnimGraph::LoadFromFile(const std::string& path, std::string* error)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
		{
			if (error)
				*error = "无法读取 '" + path + "'";
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
			&& static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
			text.erase(0, 3);

		Clear();

		AnimState* currentState = nullptr;
		AnimLayer* currentLayer = nullptr;
		std::string currentStateName;

		int lineNumber = 0;
		bool versionSeen = false;

		std::istringstream lines(text);
		std::string rawLine;
		while (std::getline(lines, rawLine))
		{
			++lineNumber;
			const size_t comment = rawLine.find('#');
			if (comment != std::string::npos)
				rawLine.erase(comment);
			const std::string line = Trim(rawLine);
			if (line.empty())
				continue;

			if (line.rfind("state ", 0) == 0)
			{
				currentStateName = Trim(line.substr(6));
				if (currentStateName.empty())
				{
					if (error)
						*error = "第 " + std::to_string(lineNumber) + " 行: state 缺少名称";
					return false;
				}
				m_States[currentStateName] = AnimState{};
				m_States[currentStateName].Name = currentStateName;
				currentState = &m_States[currentStateName];
				currentLayer = nullptr;
				continue;
			}
			if (line.rfind("layer ", 0) == 0)
			{
				const std::string name = Trim(line.substr(6));
				m_Layers.push_back(AnimLayer{});
				m_Layers.back().Name = name;
				currentLayer = &m_Layers.back();
				currentState = nullptr;
				continue;
			}

			const size_t colon = line.find(':');
			if (colon == std::string::npos)
			{
				WarnOnce("animgraph-unknown-line:" + line,
					"AnimGraph 第 " + std::to_string(lineNumber) + " 行无法识别，已忽略: " + line);
				continue;
			}
			const std::string key = Trim(std::string_view(line).substr(0, colon));
			const std::string value = Trim(std::string_view(line).substr(colon + 1));

			if (currentState)
			{
				if (key == "kind")
				{
					AnimStateKind kind = AnimStateKind::SingleClip;
					if (!KindFromString(value, kind))
					{
						if (error)
							*error = "第 " + std::to_string(lineNumber) + " 行: 未知 kind '" + value + "'";
						return false;
					}
					currentState->Kind = kind;
				}
				else if (key == "clip")
					currentState->ClipName = value;
				else if (key == "loop")
				{
					bool loop = true;
					if (!ParseBool(value, loop))
						WarnOnce("animgraph-bad-loop:" + value, "AnimGraph loop 值无效: " + value);
					else
						currentState->Loop = loop;
				}
				else if (key == "speed")
				{
					float speed = 1.0f;
					if (ParseFloat(value, speed))
						currentState->Speed = speed;
					else
						WarnOnce("animgraph-bad-speed:" + value, "AnimGraph speed 值无效: " + value);
				}
				else if (key == "x")
					currentState->ParameterX = value;
				else if (key == "y")
					currentState->ParameterY = value;
				else if (key == "sample")
				{
					const std::vector<std::string> tokens = SplitWhitespace(value);
					if (tokens.size() == 2)
					{
						float sampleValue = 0.0f;
						if (ParseFloat(tokens[0], sampleValue))
							currentState->Blend1D.AddSample(sampleValue, tokens[1]);
						else
							WarnOnce("animgraph-bad-sample:" + value, "AnimGraph 1D sample 无效: " + value);
						currentState->Kind = AnimStateKind::BlendSpace1D;
					}
					else if (tokens.size() == 3)
					{
						float x = 0.0f;
						float y = 0.0f;
						if (ParseFloat(tokens[0], x) && ParseFloat(tokens[1], y))
							currentState->Blend2D.AddSample(glm::vec2(x, y), tokens[2]);
						else
							WarnOnce("animgraph-bad-sample:" + value, "AnimGraph 2D sample 无效: " + value);
						currentState->Kind = AnimStateKind::BlendSpace2D;
					}
					else
					{
						WarnOnce("animgraph-bad-sample:" + value, "AnimGraph sample 字段数错误: " + value);
					}
				}
				else if (key == "transition")
				{
					const size_t firstSpace = value.find_first_of(" \t");
					const size_t secondSpace = (firstSpace == std::string::npos)
						? std::string::npos : value.find_first_of(" \t", firstSpace + 1);
					if (firstSpace == std::string::npos)
					{
						WarnOnce("animgraph-bad-transition:" + value, "AnimGraph transition 缺少时长: " + value);
					}
					else
					{
						AnimTransition transition;
						transition.TargetState = value.substr(0, firstSpace);
						const std::string durationText = value.substr(firstSpace + 1,
							secondSpace == std::string::npos ? std::string::npos : secondSpace - firstSpace - 1);
						float duration = 0.2f;
						if (!ParseFloat(durationText, duration))
							duration = 0.2f;
						transition.Duration = duration;
						if (secondSpace != std::string::npos)
							transition.ConditionExpression = Trim(value.substr(secondSpace + 1));
						currentState->Transitions.push_back(transition);
					}
				}
				else
				{
					WarnOnce("animgraph-unknown-state-key:" + key, "AnimGraph 未知 state 键 '" + key + "'，已忽略");
				}
				continue;
			}

			if (currentLayer)
			{
				if (key == "clip")
					currentLayer->ClipName = value;
				else if (key == "weight")
					currentLayer->WeightParameter = value;
				else if (key == "mask")
				{
					std::istringstream maskStream(value);
					std::string entry;
					while (std::getline(maskStream, entry, ','))
					{
						const std::vector<std::string> tokens = SplitWhitespace(entry);
						if (tokens.empty())
							continue;
						BoneMaskEntry bone;
						bone.BoneName = tokens[0];
						if (tokens.size() > 1)
						{
							if (!ParseFloat(tokens[1], bone.Weight))
							{
								WarnOnce("animgraph-bad-mask:" + entry, "AnimGraph mask 权重无效: " + entry);
								continue;
							}
						}
						currentLayer->BoneMask.push_back(bone);
					}
				}
				else
				{
					WarnOnce("animgraph-unknown-layer-key:" + key, "AnimGraph 未知 layer 键 '" + key + "'，已忽略");
				}
				continue;
			}

			if (key == "version")
			{
				float version = 0.0f;
				versionSeen = true;
				if (!ParseFloat(value, version) || std::fabs(version - 1.0f) >= 1e-6f)
				{
					if (error)
						*error = "不支持的 AnimGraph 版本 '" + value + "'（只接受 1）";
					return false;
				}
			}
			else if (key == "name")
				SetGraphName(value);
			else if (key == "model")
				SetModelHint(value);
			else if (key == "initial")
				SetInitialState(value);
			else if (key == "parameter")
			{
				const std::vector<std::string> tokens = SplitWhitespace(value);
				if (tokens.size() < 2)
				{
					WarnOnce("animgraph-bad-parameter:" + value, "AnimGraph parameter 无效: " + value);
					continue;
				}

				const std::string typeText = LowerAscii(tokens[1]);
				const bool hasExplicitType = (typeText == "float" || typeText == "bool");
				if (tokens.size() == 2 && !hasExplicitType)
				{
					float defaultValue = 0.0f;
					if (!ParseFloat(tokens[1], defaultValue))
					{
						WarnOnce("animgraph-bad-parameter:" + value, "AnimGraph parameter 无效: " + value);
						continue;
					}
					AnimParameterDefinition definition;
					definition.Name = tokens[0];
					definition.Type = AnimParameterType::Float;
					definition.DefaultValue = defaultValue;
					definition.MinValue = 0.0f;
					definition.MaxValue = 1.0f;
					SetParameterDefinition(definition);
					continue;
				}

				AnimParameterDefinition definition;
				definition.Name = tokens[0];
				if (typeText == "bool")
					definition.Type = AnimParameterType::Bool;
				else
				{
					definition.Type = AnimParameterType::Float;
					if (!hasExplicitType)
					{
						WarnOnce("animgraph-parameter-type:" + value,
							"AnimGraph parameter 类型无效，按 float 处理: " + value);
					}
				}

				float parsed = 0.0f;
				if (tokens.size() > 2)
				{
					if (ParseFloat(tokens[2], parsed))
						definition.DefaultValue = parsed;
					else
					{
						WarnOnce("animgraph-parameter-default:" + value,
							"AnimGraph parameter 默认值无效，按 0 处理: " + value);
					}
				}
				if (tokens.size() > 3)
				{
					if (ParseFloat(tokens[3], parsed))
						definition.MinValue = parsed;
					else
					{
						WarnOnce("animgraph-parameter-min:" + value,
							"AnimGraph parameter 最小值无效，按 0 处理: " + value);
					}
				}
				if (tokens.size() > 4)
				{
					if (ParseFloat(tokens[4], parsed))
						definition.MaxValue = parsed;
					else
					{
						WarnOnce("animgraph-parameter-max:" + value,
							"AnimGraph parameter 最大值无效，按 1 处理: " + value);
					}
				}
				if (tokens.size() > 5)
				{
					std::ostringstream unit;
					for (size_t i = 5; i < tokens.size(); ++i)
					{
						if (i > 5)
							unit << ' ';
						unit << tokens[i];
					}
					definition.Unit = unit.str();
				}
				SetParameterDefinition(definition);
			}
			else
			{
				WarnOnce("animgraph-unknown-header-key:" + key, "AnimGraph 未知头部键 '" + key + "'，已忽略");
			}
		}

		if (!versionSeen)
		{
			if (error)
				*error = "AnimGraph 缺少 version 字段";
			return false;
		}

		if (m_CurrentStateName.empty() && !m_States.empty())
			SetInitialState(m_States.begin()->first);
		else if (!m_InitialStateName.empty())
			SetInitialState(m_InitialStateName);

		EnsureTransitionConditions();
		if (error)
			error->clear();
		return true;
	}
}

