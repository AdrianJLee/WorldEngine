#include "wldpch.h"
#include "World/Physics/CharacterController.h"
#include "World/Physics/PhysicsQuery.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace World
{
	namespace
	{
		Physics::ShapeCastHit SweepCharacter(Scene& scene, entt::entity entity, const CharacterControllerComponent& kcc,
		                                     const glm::vec3& startPos, const glm::vec3& dir, float distance)
		{
			auto& reg = scene.GetRegistry();
			uint32_t savedKccLayer = kcc.CollisionLayer;
			const_cast<CharacterControllerComponent&>(kcc).CollisionLayer = 0;

			auto* rb3d = reg.try_get<RigidBody3DComponent>(entity);
			uint32_t savedRb3dLayer = rb3d ? rb3d->Layer : 0;
			if (rb3d) rb3d->Layer = 0;

			auto* rb2d = reg.try_get<RigidBody2DComponent>(entity);
			uint32_t savedRb2dLayer = rb2d ? rb2d->Layer : 0;
			if (rb2d) rb2d->Layer = 0;

			Physics::ShapeCastHit hit;

			if (const auto* capsule = reg.try_get<CapsuleCollider3DComponent>(entity))
			{
				hit = Physics::PhysicsQuerySystem::CapsuleCast(scene, startPos + capsule->Offset, capsule->Radius,
				                                              capsule->HalfHeight, dir, distance, kcc.CollisionMask);
			}
			else if (const auto* sphere = reg.try_get<SphereCollider3DComponent>(entity))
			{
				hit = Physics::PhysicsQuerySystem::SphereCast(scene, startPos + sphere->Offset, sphere->Radius,
				                                             dir, distance, kcc.CollisionMask);
			}
			else if (const auto* box = reg.try_get<BoxCollider3DComponent>(entity))
			{
				hit = Physics::PhysicsQuerySystem::BoxCast(scene, startPos + box->Offset, box->HalfExtents,
				                                          dir, distance, kcc.CollisionMask);
			}
			else if (const auto* box2d = reg.try_get<BoxCollider2DComponent>(entity))
			{
				hit = Physics::PhysicsQuerySystem::BoxCast(scene, startPos + glm::vec3(box2d->Offset, 0.0f),
				                                          glm::vec3(box2d->Size, 1.0f), dir, distance, kcc.CollisionMask);
			}
			else if (const auto* circle2d = reg.try_get<CircleCollider2DComponent>(entity))
			{
				hit = Physics::PhysicsQuerySystem::SphereCast(scene, startPos + glm::vec3(circle2d->Offset, 0.0f),
				                                             circle2d->Radius, dir, distance, kcc.CollisionMask);
			}
			else
			{
				hit = Physics::PhysicsQuerySystem::CapsuleCast(scene, startPos, 0.4f, 0.6f, dir, distance, kcc.CollisionMask);
			}

			const_cast<CharacterControllerComponent&>(kcc).CollisionLayer = savedKccLayer;
			if (rb3d) rb3d->Layer = savedRb3dLayer;
			if (rb2d) rb2d->Layer = savedRb2dLayer;

			return hit;
		}
	}

	void CharacterControllerSystem::Update(Scene& scene, float dt)
	{
		auto view = scene.GetRegistry().view<CharacterControllerComponent>();
		for (auto entity : view)
		{
			auto& kcc = view.get<CharacterControllerComponent>(entity);
			if (kcc.Grounded == 0)
			{
				kcc.CoyoteTimer = std::max(0.0f, kcc.CoyoteTimer - dt);
				kcc.Velocity.y += (-9.81f * kcc.GravityScale) * dt;
			}
			else
			{
				kcc.CoyoteTimer = kcc.CoyoteDuration;
			}

			if (kcc.JumpBufferTimer > 0.0f)
			{
				kcc.JumpBufferTimer = std::max(0.0f, kcc.JumpBufferTimer - dt);
			}

			if (kcc.LockZPlane != 0)
			{
				kcc.Velocity.z = 0.0f;
			}
		}
	}

	glm::vec3 CharacterControllerSystem::MoveAndSlide(Scene& scene, entt::entity entity, const glm::vec3& displacement, float dt)
	{
		auto& reg = scene.GetRegistry();
		auto* kcc = reg.try_get<CharacterControllerComponent>(entity);
		auto* tr = reg.try_get<TransformComponent>(entity);
		if (!kcc || !tr) return glm::vec3(0.0f);

		glm::vec3 disp = displacement;
		if (kcc->LockZPlane != 0)
		{
			disp.z = 0.0f;
		}

		uint8_t wasGrounded = kcc->Grounded;
		kcc->CollisionFlags = CharacterCollisionFlags::None;
		kcc->Grounded = 0;
		kcc->IsSliding = 0;

		glm::vec3 currentPos = tr->Location;
		glm::vec3 totalMoved(0.0f);
		glm::vec3 remaining = disp;

		std::vector<glm::vec3> contactNormals;

		for (int bounce = 0; bounce < 4; ++bounce)
		{
			float len = glm::length(remaining);
			if (len < 1e-5f) break;

			glm::vec3 dir = remaining / len;
			float castDist = len + kcc->SkinWidth;

			Physics::ShapeCastHit hit = SweepCharacter(scene, entity, *kcc, currentPos, dir, castDist);

			if (!hit.Hit || hit.Distance > len + kcc->SkinWidth)
			{
				currentPos += remaining;
				totalMoved += remaining;
				break;
			}

			float travelDist = std::max(0.0f, hit.Distance - kcc->SkinWidth);
			if (travelDist > len) travelDist = len;
			glm::vec3 travel = dir * travelDist;
			currentPos += travel;
			totalMoved += travel;
			remaining -= dir * travelDist;

			glm::vec3 normal = hit.Normal;
			contactNormals.push_back(normal);

			float cosSlope = glm::clamp(normal.y, -1.0f, 1.0f);
			float slopeAngleDeg = glm::degrees(std::acos(cosSlope));

			if (normal.y > 0.0f && slopeAngleDeg <= kcc->SlopeLimitDeg)
			{
				kcc->CollisionFlags |= CharacterCollisionFlags::Below;
				kcc->Grounded = 1;
			}
			else if (normal.y > 0.0f && slopeAngleDeg > kcc->SlopeLimitDeg)
			{
				kcc->IsSliding = 1;
				kcc->Grounded = 0;
				glm::vec3 downhill = ProjectTangent(glm::vec3(0.0f, -1.0f, 0.0f), normal);
				float downhillLen = glm::length(downhill);
				if (downhillLen > 1e-4f)
				{
					downhill /= downhillLen;
					float slideSpeed = 9.81f * kcc->GravityScale * (dt > 0.0f ? dt : 0.016f);
					glm::vec3 slideDelta = downhill * slideSpeed;
					currentPos += slideDelta;
					totalMoved += slideDelta;
				}
			}
			else if (normal.y < -0.7f)
			{
				kcc->CollisionFlags |= CharacterCollisionFlags::Above;
			}
			else
			{
				kcc->CollisionFlags |= CharacterCollisionFlags::Sides;
			}

			bool steppedUp = false;
			if ((kcc->CollisionFlags & CharacterCollisionFlags::Sides) && kcc->StepOffset > 0.0f)
			{
				float stepHeight = kcc->StepOffset;
				Physics::ShapeCastHit upHit = SweepCharacter(scene, entity, *kcc, currentPos, glm::vec3(0, 1, 0), stepHeight);
				float actualUp = (!upHit.Hit) ? stepHeight : std::max(0.0f, upHit.Distance - kcc->SkinWidth);

				if (actualUp > 0.02f)
				{
					glm::vec3 elevatedPos = currentPos + glm::vec3(0, actualUp, 0);
					glm::vec3 horizDisp = glm::vec3(remaining.x, 0.0f, remaining.z);
					float horizLen = glm::length(horizDisp);
					if (horizLen > 1e-4f)
					{
						glm::vec3 horizDir = horizDisp / horizLen;
						Physics::ShapeCastHit forwardHit = SweepCharacter(scene, entity, *kcc, elevatedPos, horizDir, horizLen + kcc->SkinWidth);
						float forwardDist = (!forwardHit.Hit) ? horizLen : std::max(0.0f, forwardHit.Distance - kcc->SkinWidth);
						glm::vec3 forwardPos = elevatedPos + horizDir * forwardDist;

						Physics::ShapeCastHit downHit = SweepCharacter(scene, entity, *kcc, forwardPos, glm::vec3(0, -1, 0), actualUp + stepHeight);
						if (downHit.Hit)
						{
							float downDist = std::max(0.0f, downHit.Distance - kcc->SkinWidth);
							glm::vec3 landPos = forwardPos - glm::vec3(0, downDist, 0);
							float landCos = glm::clamp(downHit.Normal.y, -1.0f, 1.0f);
							float landSlopeDeg = glm::degrees(std::acos(landCos));
							if (landSlopeDeg <= kcc->SlopeLimitDeg && landPos.y >= currentPos.y)
							{
								glm::vec3 stepMove = landPos - currentPos;
								currentPos = landPos;
								totalMoved += stepMove;
								remaining = glm::vec3(0.0f);
								kcc->Grounded = 1;
								kcc->CollisionFlags |= CharacterCollisionFlags::Below;
								steppedUp = true;
								break;
							}
						}
					}
				}
			}

			if (steppedUp) break;

			if (contactNormals.size() >= 2)
			{
				glm::vec3 n1 = contactNormals[contactNormals.size() - 2];
				glm::vec3 n2 = contactNormals[contactNormals.size() - 1];
				remaining = ProjectCrease(remaining, n1, n2);
			}
			else
			{
				remaining = ProjectTangent(remaining, normal);
			}
		}

		if (wasGrounded && kcc->Grounded == 0 && disp.y <= 1e-4f && kcc->StepOffset > 0.0f)
		{
			float checkDownDist = kcc->StepOffset + kcc->SkinWidth;
			Physics::ShapeCastHit downHit = SweepCharacter(scene, entity, *kcc, currentPos, glm::vec3(0, -1, 0), checkDownDist);
			if (downHit.Hit)
			{
				float landCos = glm::clamp(downHit.Normal.y, -1.0f, 1.0f);
				float landSlopeDeg = glm::degrees(std::acos(landCos));
				if (landSlopeDeg <= kcc->SlopeLimitDeg)
				{
					float snapDist = std::max(0.0f, downHit.Distance - kcc->SkinWidth);
					currentPos.y -= snapDist;
					totalMoved.y -= snapDist;
					kcc->Grounded = 1;
					kcc->CollisionFlags |= CharacterCollisionFlags::Below;
				}
			}
		}

		if (kcc->LockZPlane != 0)
		{
			currentPos.z = tr->Location.z;
		}

		tr->SetLocation(currentPos);
		return totalMoved;
	}

	bool CharacterControllerSystem::TryJump(CharacterControllerComponent& kcc, float jumpSpeed)
	{
		if (CanJump(kcc))
		{
			ForceJump(kcc, jumpSpeed);
			return true;
		}
		kcc.JumpBufferTimer = kcc.JumpBufferDuration;
		return false;
	}

	bool CharacterControllerSystem::CanJump(const CharacterControllerComponent& kcc)
	{
		return (kcc.Grounded != 0) || (kcc.CoyoteTimer > 0.0f);
	}

	void CharacterControllerSystem::ForceJump(CharacterControllerComponent& kcc, float jumpSpeed)
	{
		kcc.Velocity.y = jumpSpeed;
		kcc.Grounded = 0;
		kcc.CoyoteTimer = 0.0f;
		kcc.JumpBufferTimer = 0.0f;
	}

	glm::vec3 CharacterControllerSystem::ProjectTangent(const glm::vec3& vector, const glm::vec3& normal)
	{
		float nLenSq = glm::dot(normal, normal);
		if (nLenSq < 1e-6f)
			return vector;
		glm::vec3 n = normal / std::sqrt(nLenSq);
		return vector - glm::dot(vector, n) * n;
	}

	glm::vec3 CharacterControllerSystem::ProjectCrease(const glm::vec3& vector, const glm::vec3& n1, const glm::vec3& n2)
	{
		glm::vec3 crease = glm::cross(n1, n2);
		float lenSq = glm::dot(crease, crease);
		if (lenSq > 1e-6f)
		{
			glm::vec3 d = crease / std::sqrt(lenSq);
			return glm::dot(vector, d) * d;
		}
		if (glm::dot(n1, n2) < -0.9f)
			return glm::vec3(0.0f);
		return ProjectTangent(vector, n1);
	}
}
