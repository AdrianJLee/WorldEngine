// D5c-3a:CPU 蒙皮/动画数学 headless 回归(SampleAnimation / BuildJointMatrices,纯函数无设备)。
//
// 覆盖:
//   1. SampleAnimation:关键帧线性插值 + t clamp 到 [0, duration];
//   2. 旋转通道 slerp(0° → 90° 绕 Y,t=0.5 → 45°);
//   3. 边界:空动画 → 空结果;单关键帧 → 恒定值;无关键帧通道 → 该路径单位值;
//   4. BuildJointMatrices:palette[j] = inverse(meshWorld) * jointWorld[JointNodes[j]] * inverseBind[j];
//   5. JointNodes 间接寻址、逆绑定按下标 j、越界/不完整输入 → 空 vector(调用方按"无蒙皮"处理)。
#include "World/Renderer/Skinning.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Nearly(float left, float right, float epsilon = 1e-5f)
	{
		return std::fabs(left - right) <= epsilon;
	}

	bool NearlyVec3(const glm::vec3& left, const glm::vec3& right, float epsilon = 1e-5f)
	{
		return Nearly(left.x, right.x, epsilon) && Nearly(left.y, right.y, epsilon)
			&& Nearly(left.z, right.z, epsilon);
	}

	bool NearlyMat4(const glm::mat4& left, const glm::mat4& right, float epsilon = 1e-5f)
	{
		for (int column = 0; column < 4; ++column)
			for (int row = 0; row < 4; ++row)
				if (!Nearly(left[column][row], right[column][row], epsilon))
					return false;
		return true;
	}

	// 构造单通道剪辑的辅助(只在测试里用,顺序 = 传入顺序)。
	World::Asset::WModelAnimationChannel MakeChannel(uint32_t node,
		World::Asset::WModelIO::WModelAnimationPath path,
		std::vector<World::Asset::WModelAnimationKey> keys)
	{
		World::Asset::WModelAnimationChannel channel;
		channel.TargetNode = node;
		channel.Path = path;
		channel.Keys = std::move(keys);
		return channel;
	}

	// 1.SampleAnimation:3 帧线性 (0,0,0) → (0,0.5,0) → (0,1,0);t=0/0.5/1/1.5(clamp)→ 0/0.5/1/1。
	void SampleAnimationInterpolatesTranslationKeys()
	{
		using namespace World;
		Asset::WModelAnimation clip;
		clip.Name = "Move";
		clip.Duration = 1.0f;
		clip.Channels = {
			MakeChannel(7u, Asset::WModelIO::WModelAnimationPath::Translation, {
				{ 0.0f, { 0.0f, 0.0f, 0.0f, 0.0f } },
				{ 0.5f, { 0.0f, 0.5f, 0.0f, 0.0f } },
				{ 1.0f, { 0.0f, 1.0f, 0.0f, 0.0f } },
			}),
		};

		const std::vector<AnimationSample> at0 = SampleAnimation(clip, 0.0f);
		CHECK(at0.size() == 1u);
		CHECK(at0[0].Node == 7u);
		CHECK(at0[0].Path == Asset::WModelIO::WModelAnimationPath::Translation);
		CHECK(Nearly(at0[0].Value.y, 0.0f));
		CHECK(Nearly(SampleAnimation(clip, 0.5f)[0].Value.y, 0.5f));
		CHECK(Nearly(SampleAnimation(clip, 1.0f)[0].Value.y, 1.0f));
		CHECK(Nearly(SampleAnimation(clip, 1.5f)[0].Value.y, 1.0f));   // clamp 到 duration
		CHECK(Nearly(SampleAnimation(clip, -1.0f)[0].Value.y, 0.0f));  // 负时间 clamp 到 0
		// 区间内部真线性(不是只在关键帧上取值)。
		CHECK(Nearly(SampleAnimation(clip, 0.25f)[0].Value.y, 0.25f));
		CHECK(Nearly(SampleAnimation(clip, 0.75f)[0].Value.y, 0.75f));
	}

	// 2.SampleAnimation:旋转通道用 slerp(0° → 90° 绕 Y,t=0.5 → 45°,w = cos22.5°)。
	void SampleAnimationSlerpsRotationKeys()
	{
		using namespace World;
		const glm::quat ninetyAboutY = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));

		Asset::WModelAnimation clip;
		clip.Duration = 1.0f;
		clip.Channels = {
			MakeChannel(3u, Asset::WModelIO::WModelAnimationPath::Rotation, {
				{ 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f } },
				{ 1.0f, { ninetyAboutY.x, ninetyAboutY.y, ninetyAboutY.z, ninetyAboutY.w } },
			}),
		};

		const AnimationSample mid = SampleAnimation(clip, 0.5f)[0];
		CHECK(mid.Node == 3u);
		CHECK(mid.Path == Asset::WModelIO::WModelAnimationPath::Rotation);
		CHECK(Nearly(mid.Value.w, std::cos(glm::radians(22.5f)), 1e-4f));
		CHECK(Nearly(mid.Value.y, std::sin(glm::radians(22.5f)), 1e-4f));
		CHECK(Nearly(mid.Value.x, 0.0f, 1e-4f));
		CHECK(Nearly(mid.Value.z, 0.0f, 1e-4f));
	}

	// 3.SampleAnimation 边界:空动画 → 空结果;单关键帧 → 恒定值;无关键帧通道 → 该路径单位值。
	void SampleAnimationHandlesEdgeCases()
	{
		using namespace World;

		// 空动画(无通道)→ 空结果。
		const Asset::WModelAnimation empty;
		CHECK(SampleAnimation(empty, 0.5f).empty());

		// 单关键帧 → t 落在关键帧前/后都取恒定值(clamp 后也不会越界)。
		Asset::WModelAnimation hold;
		hold.Duration = 2.0f;
		hold.Channels = {
			MakeChannel(1u, Asset::WModelIO::WModelAnimationPath::Scale, {
				{ 1.0f, { 2.0f, 2.0f, 2.0f, 0.0f } },
			}),
		};
		const std::vector<AnimationSample> before = SampleAnimation(hold, 0.0f);
		const std::vector<AnimationSample> after = SampleAnimation(hold, 5.0f);
		CHECK(before.size() == 1u && after.size() == 1u);
		CHECK(before[0].Path == Asset::WModelIO::WModelAnimationPath::Scale);
		CHECK(NearlyVec3(glm::vec3(before[0].Value), glm::vec3(2.0f)));
		CHECK(NearlyVec3(glm::vec3(after[0].Value), glm::vec3(2.0f)));

		// 无关键帧的通道:上报该路径的单位值(平移 0 / 旋转 identity),不是越界也不是跳过。
		Asset::WModelAnimation keyless;
		keyless.Duration = 1.0f;
		keyless.Channels = {
			MakeChannel(2u, Asset::WModelIO::WModelAnimationPath::Translation, {}),
			MakeChannel(3u, Asset::WModelIO::WModelAnimationPath::Rotation, {}),
		};
		const std::vector<AnimationSample> samples = SampleAnimation(keyless, 0.25f);
		CHECK(samples.size() == 2u);
		CHECK(Nearly(samples[0].Value.x, 0.0f) && Nearly(samples[0].Value.w, 0.0f));
		CHECK(Nearly(samples[1].Value.w, 1.0f));
		CHECK(Nearly(samples[1].Value.x, 0.0f) && Nearly(samples[1].Value.y, 0.0f)
			&& Nearly(samples[1].Value.z, 0.0f));
	}

	// 4.BuildJointMatrices:单位情形 + 关节世界平移 + meshWorld 走逆。
	void BuildJointMatricesAppliesMeshInverseAndBindPose()
	{
		using namespace World;
		const glm::mat4 identity(1.0f);
		Asset::WModelSkin skin;
		skin.JointNames = { "a", "b" };
		skin.JointNodes = { 0u, 1u };
		skin.JointParents = { -1, 0 };
		skin.InverseBindMatrices = { identity, identity };
		skin.BindTranslations = { glm::vec3(0.0f), glm::vec3(0.0f) };
		skin.BindRotations = { glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) };
		skin.BindScales = { glm::vec3(1.0f), glm::vec3(1.0f) };

		// 单位情形:meshWorld / jointWorld / inverseBind 全单位 → 全单位阵。
		const std::vector<glm::mat4> identityPalette = BuildJointMatrices(skin,
			{ identity, identity }, identity);
		CHECK(identityPalette.size() == 2u);
		for (const glm::mat4& matrix : identityPalette)
			CHECK(NearlyMat4(matrix, identity));

		// 关节世界变换的平移出现在 palette 的平移列(各关节独立)。
		const glm::mat4 movedJoint = glm::translate(identity, glm::vec3(0.0f, 2.0f, 0.0f));
		const std::vector<glm::mat4> movedPalette = BuildJointMatrices(skin,
			{ movedJoint, identity }, identity);
		CHECK(movedPalette.size() == 2u);
		CHECK(NearlyVec3(glm::vec3(movedPalette[0][3]), glm::vec3(0.0f, 2.0f, 0.0f)));
		CHECK(NearlyVec3(glm::vec3(movedPalette[1][3]), glm::vec3(0.0f)));

		// meshWorld 走的是**逆**:meshWorld 的平移把关节世界平移抵消回原点。
		const std::vector<glm::mat4> cancelling = BuildJointMatrices(skin,
			{ movedJoint, movedJoint }, movedJoint);
		CHECK(cancelling.size() == 2u);
		CHECK(NearlyMat4(cancelling[0], identity));
		CHECK(NearlyVec3(glm::vec3(cancelling[0][3]), glm::vec3(0.0f)));
	}

	// 5.BuildJointMatrices:JointNodes 间接寻址、逆绑定按下标 j、越界/不完整输入返回空。
	void BuildJointMatricesUsesJointNodesIndirection()
	{
		using namespace World;
		const glm::mat4 identity(1.0f);

		// JointNodes 把关节集合下标映射到 jointWorld 的任意位置(这里关节 1 → jointWorld[2])。
		Asset::WModelSkin skin;
		skin.JointNodes = { 0u, 2u };
		skin.InverseBindMatrices = { identity, identity };
		const glm::mat4 moved = glm::translate(identity, glm::vec3(0.0f, 3.0f, 0.0f));
		const std::vector<glm::mat4> palette = BuildJointMatrices(skin,
			{ identity, identity, moved }, identity);
		CHECK(palette.size() == 2u);
		CHECK(NearlyVec3(glm::vec3(palette[0][3]), glm::vec3(0.0f)));
		CHECK(NearlyVec3(glm::vec3(palette[1][3]), glm::vec3(0.0f, 3.0f, 0.0f)));

		// 逆绑定矩阵按**关节下标 j** 取(JointNodes[0] = 1,但用的是 InverseBindMatrices[0])。
		Asset::WModelSkin single;
		single.JointNodes = { 1u };
		single.InverseBindMatrices = { glm::translate(identity, glm::vec3(0.0f, 5.0f, 0.0f)) };
		const std::vector<glm::mat4> singlePalette = BuildJointMatrices(single,
			{ identity, identity }, identity);
		CHECK(singlePalette.size() == 1u);
		CHECK(NearlyVec3(glm::vec3(singlePalette[0][3]), glm::vec3(0.0f, 5.0f, 0.0f)));

		// 越界引用:JointNodes[1] = 4 超出 jointWorld.size() = 3 → 空 vector(调用方按"无蒙皮"处理)。
		skin.JointNodes[1] = 4u;
		CHECK(BuildJointMatrices(skin, { identity, identity, moved }, identity).empty());

		// 逆绑定矩阵不足(容器不变量之外的手工数据)→ 同样返回空,不越界读。
		Asset::WModelSkin incomplete;
		incomplete.JointNodes = { 0u, 1u };
		incomplete.InverseBindMatrices = { identity };
		CHECK(BuildJointMatrices(incomplete, { identity, identity }, identity).empty());

		// 关节数为 0 → 空调色板。
		Asset::WModelSkin noJoints;
		CHECK(BuildJointMatrices(noJoints, {}, identity).empty());
	}
}

int main()
{
	try
	{
		const std::pair<const char*, void(*)()> tests[] = {
			{ "SampleAnimation interpolates translation keys (clamped)",
				SampleAnimationInterpolatesTranslationKeys },
			{ "SampleAnimation slerps rotation keys", SampleAnimationSlerpsRotationKeys },
			{ "SampleAnimation handles empty clips / single keys / keyless channels",
				SampleAnimationHandlesEdgeCases },
			{ "BuildJointMatrices applies mesh inverse + inverse bind",
				BuildJointMatricesAppliesMeshInverseAndBindPose },
			{ "BuildJointMatrices follows JointNodes indirection + rejects out-of-range",
				BuildJointMatricesUsesJointNodesIndirection },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what());
			}
			catch (...)
			{
				++failures;
				std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name);
			}
		}
		if (failures == 0)
			std::printf("World.Skinning: all checks passed\n");
		else
			std::fprintf(stderr, "World.Skinning: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Skinning: fatal: %s\n", error.what());
		return 1;
	}
}
