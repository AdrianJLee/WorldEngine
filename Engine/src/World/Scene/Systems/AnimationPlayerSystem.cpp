#include "wldpch.h"
#include "World/Scene/Systems/AnimationPlayerSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/Skinning.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Asset/WAnimationClipIO.h"
#include "World/Asset/WSkeletonIO.h"
#include "World/Asset/WModelIO.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace World
{
	void AnimationPlayerSystem::Update(Scene& scene, float deltaSeconds)
	{
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
			deltaSeconds = 0.0f;

		const entt::registry& registry = static_cast<const Scene&>(scene).GetRegistry();
		const auto view = registry.view<AnimationPlayerComponent, SkinnedMeshRendererComponent>();

		for (const entt::entity entity : view)
		{
			// 严格互斥：AnimGraphComponent 实体由 AnimGraphSystem 驱动，此处绝不重复处理
			if (registry.all_of<AnimGraphComponent>(entity))
				continue;

			const AnimationPlayerComponent* playerProbe = registry.try_get<AnimationPlayerComponent>(entity);
			const SkinnedMeshRendererComponent* skinnedProbe = registry.try_get<SkinnedMeshRendererComponent>(entity);
			if (!playerProbe || !skinnedProbe)
				continue;

			AnimationPlayerComponent& player = const_cast<AnimationPlayerComponent&>(*playerProbe);
			SkinnedMeshRendererComponent& skinned = const_cast<SkinnedMeshRendererComponent&>(*skinnedProbe);

			if (!skinned.Mesh.HasPath())
			{
				AnimationSystem::SetPalette(entity, {});
				continue;
			}

			Ref<Mesh> mesh = Mesh::LoadWModel(skinned.Mesh.Path);
			const Asset::WModelData* modelPtr = mesh ? mesh->GetSkinSourceData() : nullptr;
			if (!modelPtr || modelPtr->Skins.empty())
			{
				AnimationSystem::SetPalette(entity, {});
				continue;
			}

			const Asset::WModelData& model = *modelPtr;
			const uint32_t meshIndex = (skinned.MeshIndex >= 0 && static_cast<size_t>(skinned.MeshIndex) < model.Meshes.size())
				? static_cast<uint32_t>(skinned.MeshIndex) : 0;
			const int32_t skinIndex = meshIndex < model.Meshes.size() ? model.Meshes[meshIndex].SkinIndex : -1;
			if (skinIndex < 0 || static_cast<size_t>(skinIndex) >= model.Skins.size())
			{
				AnimationSystem::SetPalette(entity, {});
				continue;
			}
			const Asset::WModelSkin& skin = model.Skins[static_cast<size_t>(skinIndex)];

			Asset::WAnimationClipData clipData;
			bool haveWClip = false;
			const Asset::WModelAnimation* modelAnim = nullptr;

			if (player.Clip.HasPath())
			{
				const std::string clipPath = StringPool::Get().PathOf(player.Clip.Path);
				if (Asset::WAnimationClipIO::ReadFile(clipPath, clipData))
					haveWClip = true;
			}

			if (!haveWClip && !model.Animations.empty())
			{
				modelAnim = &model.Animations.front();
			}

			const float duration = haveWClip ? clipData.Duration : (modelAnim ? modelAnim->Duration : 0.0f);
			if (player.Playing)
			{
				player.CurrentTime = AnimationSystem::AdvanceTime(
					player.CurrentTime, deltaSeconds, player.PlaybackSpeed,
					player.Playing != 0, player.Loop != 0, duration);
			}

			if (haveWClip)
			{
				Asset::WSkeletonData skeleton;
				if (!clipData.SkeletonPath.empty())
					Asset::WSkeletonIO::ReadFile(clipData.SkeletonPath, skeleton);

				if (skeleton.Bones.empty())
				{
					std::vector<Asset::WAnimationClipData> dummy;
					Asset::WSkeletonIO::ExtractSkeletonAndClips(model, skeleton, dummy);
				}

				std::vector<JointTransform> localPose;
				Asset::WAnimationClipIO::SampleAnimationClip(
					clipData, player.CurrentTime, player.Loop != 0, skeleton, localPose);

				const size_t nodeCount = model.Nodes.size();
				std::vector<JointTransform> poseJoints(nodeCount);
				for (size_t n = 0; n < nodeCount; ++n)
				{
					poseJoints[n].Translation = model.Nodes[n].Translation;
					poseJoints[n].Rotation = model.Nodes[n].Rotation;
					poseJoints[n].Scale = model.Nodes[n].Scale;
				}

				for (size_t j = 0; j < skeleton.Bones.size() && j < localPose.size(); ++j)
				{
					uint32_t nodeIdx = UINT32_MAX;
					if (j < skin.JointNodes.size())
						nodeIdx = skin.JointNodes[j];
					else
					{
						for (size_t n = 0; n < nodeCount; ++n)
						{
							if (model.Nodes[n].Name == skeleton.Bones[j].NameStr)
							{
								nodeIdx = static_cast<uint32_t>(n);
								break;
							}
						}
					}
					if (nodeIdx < nodeCount)
						poseJoints[nodeIdx] = localPose[j];
				}

				std::vector<glm::mat4> nodeWorld(nodeCount, glm::mat4(1.0f));
				std::vector<uint8_t> state(nodeCount, 0);

				const auto localMatrix = [&](size_t idx)
				{
					const JointTransform& jt = poseJoints[idx];
					return glm::translate(glm::mat4(1.0f), jt.Translation)
						* glm::mat4_cast(jt.Rotation)
						* glm::scale(glm::mat4(1.0f), jt.Scale);
				};

				std::function<glm::mat4(size_t)> resolve = [&](size_t idx) -> glm::mat4
				{
					if (state[idx] == 2) return nodeWorld[idx];
					if (state[idx] == 1) return glm::mat4(1.0f);
					state[idx] = 1;
					glm::mat4 parentWorld(1.0f);
					const int32_t parent = model.Nodes[idx].Parent;
					if (parent >= 0 && static_cast<size_t>(parent) < nodeCount && static_cast<size_t>(parent) != idx)
						parentWorld = resolve(static_cast<size_t>(parent));
					nodeWorld[idx] = parentWorld * localMatrix(idx);
					state[idx] = 2;
					return nodeWorld[idx];
				};

				for (size_t i = 0; i < nodeCount; ++i)
					resolve(i);

				glm::mat4 meshWorld(1.0f);
				for (size_t idx = 0; idx < nodeCount; ++idx)
				{
					const int32_t mIdx = model.Nodes[idx].MeshIndex;
					if (mIdx >= 0 && static_cast<size_t>(mIdx) < model.Meshes.size()
						&& model.Meshes[static_cast<size_t>(mIdx)].SkinIndex == static_cast<int32_t>(skinIndex))
					{
						meshWorld = nodeWorld[idx];
						break;
					}
				}

				AnimationSystem::SetPalette(entity, BuildJointMatrices(skin, nodeWorld, meshWorld));
			}
			else if (modelAnim)
			{
				AnimationSystem::SetPalette(entity, AnimationSystem::ComputePalette(
					model, static_cast<uint32_t>(skinIndex), *modelAnim, player.CurrentTime));
			}
			else
			{
				static const Asset::WModelAnimation kBindPoseAnim;
				AnimationSystem::SetPalette(entity, AnimationSystem::ComputePalette(
					model, static_cast<uint32_t>(skinIndex), kBindPoseAnim, 0.0f));
			}
		}
	}
}
