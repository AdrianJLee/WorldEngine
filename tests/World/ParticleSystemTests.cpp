#include "World/Renderer/ParticleSystem.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace World;

static void TestPoolInitializationAndCapacity()
{
	ParticlePool pool(500);
	assert(pool.Capacity() == 500);
	assert(pool.ActiveCount() == 0);

	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.Speed = 5.0f;

	pool.Emit(50, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 50);

	pool.Reset();
	assert(pool.ActiveCount() == 0);

	std::cout << "[PASS] TestPoolInitializationAndCapacity\n";
}

static void TestGeometricEmissionAndShapes()
{
	ParticlePool pool(100);
	ParticleSystemComponent comp;
	comp.Lifetime = 1.0f;
	comp.Speed = 10.0f;

	ParticleEmitterType shapes[] = {
		ParticleEmitterType::Point,
		ParticleEmitterType::Line,
		ParticleEmitterType::Box,
		ParticleEmitterType::Sphere,
		ParticleEmitterType::Cone,
		ParticleEmitterType::Circle
	};

	for (auto shape : shapes)
	{
		pool.Reset();
		comp.EmitterType = shape;
		pool.Emit(10, comp, glm::vec3(1.0f, 2.0f, 3.0f));
		assert(pool.ActiveCount() == 10);
	}

	std::cout << "[PASS] TestGeometricEmissionAndShapes\n";
}

static void TestParticleUpdateAndLifetime()
{
	ParticlePool pool(100);
	ParticleSystemComponent comp;
	comp.Lifetime = 1.0f;
	comp.Speed = 0.0f;
	comp.Gravity = glm::vec3(0.0f, -10.0f, 0.0f);
	comp.EmissionRate = 0.0f; // Manual emission
	comp.IsPlaying = 1;
	comp.SizeStart = 1.0f;
	comp.SizeEnd = 0.0f;
	comp.ColorStart = 0xFFFFFFFF;
	comp.ColorEnd = 0x00FFFFFF;

	pool.Emit(10, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 10);

	// Advance 0.5s (half life)
	pool.Update(0.5f, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 10);

	// Check particle color and size at half life
	const auto& particles = pool.GetParticles();
	for (const auto& p : particles)
	{
		if (!p.Active) continue;
		assert(p.Life > 0.0f && p.Life < 1.0f);
		assert(std::abs(p.Size.x - 0.5f) < 0.1f);
	}

	// Advance another 0.6s -> total 1.1s > 1.0s lifetime -> all should expire
	pool.Update(0.6f, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 0);

	std::cout << "[PASS] TestParticleUpdateAndLifetime\n";
}

static void TestLockToXYPlane()
{
	ParticlePool pool(100);
	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.Speed = 5.0f;
	comp.SpeedVariance = 2.0f;
	comp.Gravity = glm::vec3(0.0f, -9.8f, 5.0f); // Non-zero Z gravity
	comp.LockToXYPlane = 1;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 1;

	glm::vec3 emitterPos(10.0f, 20.0f, 7.5f);
	pool.Emit(20, comp, emitterPos);
	assert(pool.ActiveCount() == 20);

	const auto& particles = pool.GetParticles();
	for (const auto& p : particles)
	{
		if (!p.Active) continue;
		assert(std::abs(p.Position.z - emitterPos.z) < 1e-4f);
		assert(std::abs(p.Velocity.z) < 1e-4f);
		assert(std::abs(p.Acceleration.z) < 1e-4f);
	}

	// Advance simulation and verify Z constraint holds
	pool.Update(0.5f, comp, emitterPos);
	for (const auto& p : particles)
	{
		if (!p.Active) continue;
		assert(std::abs(p.Position.z - emitterPos.z) < 1e-4f);
		assert(std::abs(p.Velocity.z) < 1e-4f);
		assert(std::abs(p.Acceleration.z) < 1e-4f);
	}

	std::cout << "[PASS] TestLockToXYPlane\n";
}

static void TestVertexGeneration()
{
	ParticlePool pool(50);
	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.Speed = 0.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 1;

	pool.Emit(5, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 5);

	std::vector<ParticleVertex> quadVerts;
	pool.GenerateQuadVertices(quadVerts);
	assert(quadVerts.size() == 5 * 6);

	std::vector<ParticleVertex> billboardVerts;
	pool.GenerateBillboardVertices(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 1.0f, 0.0f), billboardVerts);
	assert(billboardVerts.size() == 5 * 6);

	std::cout << "[PASS] TestVertexGeneration\n";
}

static void TestParticleSystemManager()
{
	ParticleSystem system;
	assert(system.PoolCount() == 0);

	ParticlePool& pool1 = system.GetPoolForEntity(42, 200);
	assert(system.PoolCount() == 1);
	assert(system.HasPoolForEntity(42));
	assert(pool1.Capacity() == 200);

	ParticlePool& pool2 = system.GetPoolForEntity(100, 300);
	assert(system.PoolCount() == 2);
	assert(system.HasPoolForEntity(100));
	assert(pool2.Capacity() == 300);

	system.RemoveEntity(42);
	assert(!system.HasPoolForEntity(42));
	assert(system.PoolCount() == 1);

	system.ClearAll();
	assert(system.PoolCount() == 0);

	std::cout << "[PASS] TestParticleSystemManager\n";
}

static void TestSubUVFlipbookAndRect()
{
	// 4x4 grid check
	const glm::vec4 rect0 = ComputeSubUVRect(0, 4, 4);
	assert(std::abs(rect0.x - 0.0f) < 1e-5f);
	assert(std::abs(rect0.y - 0.0f) < 1e-5f);
	assert(std::abs(rect0.z - 0.25f) < 1e-5f);
	assert(std::abs(rect0.w - 0.25f) < 1e-5f);

	const glm::vec4 rect5 = ComputeSubUVRect(5, 4, 4); // row 1, col 1
	assert(std::abs(rect5.x - 0.25f) < 1e-5f);
	assert(std::abs(rect5.y - 0.25f) < 1e-5f);
	assert(std::abs(rect5.z - 0.50f) < 1e-5f);
	assert(std::abs(rect5.w - 0.50f) < 1e-5f);

	// Lifecycle Sub-UV progression & vertex UV check
	ParticlePool pool(16);
	ParticleMaterialSettings& mat = pool.GetMaterialSettings();
	mat.SubUVTilesX = 2;
	mat.SubUVTilesY = 2;
	mat.SubUVFrameRate = 0.0f; // map over normalized lifetime
	mat.SubUVRandomStartFrame = false;

	ParticleSystemComponent comp;
	comp.Lifetime = 1.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 0;
	comp.Speed = 0.0f;
	comp.SpeedVariance = 0.0f;

	pool.Emit(1, comp, glm::vec3(0.0f), glm::vec3(0.0f));
	assert(pool.GetParticles()[0].SubUVFrameIndex == 0u);

	// Advance past 60% of lifetime -> frame 2 (out of 4 frames: 0, 1, 2, 3)
	pool.Update(0.60f, comp, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	assert(pool.GetParticles()[0].SubUVFrameIndex == 2u);

	std::vector<ParticleVertex> verts;
	pool.GenerateQuadVertices(verts);
	assert(verts.size() == 6);
	// Frame 2 in 2x2 grid is col=0, row=1 -> u in [0, 0.5], v in [0.5, 1.0]
	assert(std::abs(verts[0].UV.x - 0.0f) < 1e-5f);
	assert(std::abs(verts[0].UV.y - 0.5f) < 1e-5f);
	assert(std::abs(verts[2].UV.x - 0.5f) < 1e-5f);
	assert(std::abs(verts[2].UV.y - 1.0f) < 1e-5f);

	std::cout << "[PASS] TestSubUVFlipbookAndRect\n";
}

static void TestSoftParticleDepthFadeAndEmissive()
{
	// Soft particle depth fade math
	assert(std::abs(ComputeSoftParticleFade(10.0f, 10.0f, 1.0f) - 0.0f) < 1e-5f);
	assert(std::abs(ComputeSoftParticleFade(10.5f, 10.0f, 1.0f) - 0.5f) < 1e-5f);
	assert(std::abs(ComputeSoftParticleFade(12.0f, 10.0f, 1.0f) - 1.0f) < 1e-5f);

	ParticlePool pool(8);
	ParticleMaterialSettings& mat = pool.GetMaterialSettings();
	mat.EmissiveBoost = 3.5f;
	mat.SoftParticlesEnabled = true;
	mat.SoftParticleFadeDistance = 2.0f;

	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 0;
	comp.Speed = 0.0f;
	comp.SpeedVariance = 0.0f;
	comp.ColorStart = 0xFFFFFFFFu;
	comp.ColorEnd = 0xFFFFFFFFu;

	pool.Emit(1, comp, glm::vec3(0.0f, 0.0f, -5.0f), glm::vec3(0.0f));
	pool.Update(0.1f, comp, glm::vec3(0.0f, 0.0f, -5.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	// HDR Emissive boost: RGB > 1.0f while Alpha stays <= 1.0f
	const Particle& p = pool.GetParticles()[0];
	assert(p.Color.r > 3.0f && p.Color.g > 3.0f && p.Color.b > 3.0f);
	assert(std::abs(p.Color.a - 1.0f) < 1e-4f);

	// Camera at origin looking down -Z: particleDepth = 5.0, sceneDepth = 5.5, fadeDist = 2.0 -> fade = 0.25
	std::vector<ParticleVertex> verts;
	pool.GenerateBillboardVerticesWithDepth(
		glm::vec3(0.0f),
		glm::vec3(0.0f, 1.0f, 0.0f),
		glm::vec3(0.0f, 0.0f, -1.0f),
		5.5f,
		verts);
	assert(verts.size() == 6);
	assert(std::abs(verts[0].SoftFadeFactor - 0.25f) < 1e-4f);
	assert(std::abs(verts[0].Color.a - 0.25f) < 1e-4f);

	std::cout << "[PASS] TestSoftParticleDepthFadeAndEmissive\n";
}

static void TestDeterministicSeedReplay()
{
	ParticleSystemComponent comp;
	comp.EmitterType = ParticleEmitterType::Cone;
	comp.Speed = 8.0f;
	comp.SpeedVariance = 3.0f;
	comp.Lifetime = 2.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 0;
	comp.VfxFlags = ParticleVfxFlag_SubUVRandomStart;

	ParticlePool poolA(32);
	poolA.GetMaterialSettings().SubUVTilesX = 4;
	poolA.GetMaterialSettings().SubUVTilesY = 4;
	poolA.SetSeed(20261017u);
	poolA.Emit(16, comp, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f));
	poolA.Update(0.16f, comp, glm::vec3(1.0f, 2.0f, 3.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	ParticlePool poolB(32);
	poolB.GetMaterialSettings().SubUVTilesX = 4;
	poolB.GetMaterialSettings().SubUVTilesY = 4;
	poolB.SetSeed(20261017u);
	poolB.Emit(16, comp, glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(0.0f));
	poolB.Update(0.16f, comp, glm::vec3(1.0f, 2.0f, 3.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	for (size_t i = 0; i < 16; ++i)
	{
		assert(poolA.GetParticles()[i].Position == poolB.GetParticles()[i].Position);
		assert(poolA.GetParticles()[i].Velocity == poolB.GetParticles()[i].Velocity);
		assert(poolA.GetParticles()[i].SubUVFrameIndex == poolB.GetParticles()[i].SubUVFrameIndex);
	}

	std::cout << "[PASS] TestDeterministicSeedReplay\n";
}


static void TestParticlePlaneCollisionAndBounce()
{
	ParticlePool pool(16);
	ParticleCollisionSettings col;
	col.Mode = ParticleCollisionMode::Plane;
	col.PlaneOrigin = glm::vec3(0.0f);
	col.PlaneNormal = glm::vec3(0.0f, 1.0f, 0.0f);
	col.Restitution = 0.8f;
	col.Friction = 0.2f;
	col.KillOnHit = false;
	col.EnableEvents = true; // explicitly enable event queue capture for this test
	pool.SetCollisionSettings(col);

	ParticleSystemComponent comp;
	comp.Lifetime = 5.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 0;
	comp.Speed = 0.0f;
	comp.SpeedVariance = 0.0f;
	comp.Gravity = glm::vec3(0.0f);

	// Emit 1 particle directly downwards towards plane at Y=0
	pool.Emit(1, comp, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f));
	pool.GetParticlesMut()[0].Velocity = glm::vec3(2.0f, -10.0f, 0.0f);

	// Update 0.6s -> moves to Y = 5 - 6 = -1 (hits plane at Y=0)
	pool.Update(0.6f, comp, glm::vec3(0.0f));

	assert(pool.GetEventQueue().HasHits());
	assert(pool.GetEventQueue().Size() == 1);
	const auto& hit = pool.GetEventQueue().GetHits()[0];
	assert(std::abs(hit.Position.y) < 1e-4f);
	assert(std::abs(hit.Normal.y - 1.0f) < 1e-4f);

	// Particle should have bounced upwards with restitution 0.8: Vy = 10 * 0.8 = 8.0
	const auto& p = pool.GetParticles()[0];
	assert(p.Active);
	assert(p.Velocity.y > 7.5f && p.Velocity.y < 8.5f);
	// Horizontal velocity attenuated by friction 0.2: Vx = 2.0 * 0.8 = 1.6
	assert(p.Velocity.x > 1.5f && p.Velocity.x < 1.7f);

	// Test KillOnHit
	ParticlePool killPool(16);
	col.KillOnHit = true;
	killPool.SetCollisionSettings(col);
	killPool.Emit(1, comp, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f));
	killPool.GetParticlesMut()[0].Velocity = glm::vec3(0.0f, -10.0f, 0.0f);
	killPool.Update(0.6f, comp, glm::vec3(0.0f));
	assert(killPool.GetEventQueue().HasHits());
	assert(!killPool.GetParticles()[0].Active);
	assert(killPool.ActiveCount() == 0);

	
	// Test Zero-Overhead Opt-In: when EnableEvents == false, bounce occurs but event queue stays completely empty
	ParticlePool zeroQueuePool(16);
	col.KillOnHit = false;
	col.EnableEvents = false;
	zeroQueuePool.SetCollisionSettings(col);
	zeroQueuePool.Emit(1, comp, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f));
	zeroQueuePool.GetParticlesMut()[0].Velocity = glm::vec3(0.0f, -10.0f, 0.0f);
	zeroQueuePool.Update(0.6f, comp, glm::vec3(0.0f));
	assert(!zeroQueuePool.GetEventQueue().HasHits());
	assert(zeroQueuePool.GetEventQueue().Size() == 0);
	assert(zeroQueuePool.GetParticles()[0].Velocity.y > 7.5f); // Bounced properly with 0 queue overhead!
	std::cout << "[PASS] TestParticlePlaneCollisionAndBounce\n";
}

static void TestParticleSubEmitters()
{
	ParticlePool pool(16);
	ParticleCollisionSettings col;
	col.Mode = ParticleCollisionMode::Plane;
	col.KillOnHit = true;
	pool.SetCollisionSettings(col);

	SubEmitterConfig sub;
	sub.Trigger = SubEmitterTrigger::OnCollision;
	sub.EmitCount = 6;
	sub.SpawnTemplate.Lifetime = 1.0f;
	sub.SpawnTemplate.Speed = 5.0f;
	pool.AddSubEmitter(sub);

	ParticleSystemComponent comp;
	comp.Lifetime = 5.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 0;
	comp.Speed = 0.0f;
	comp.Gravity = glm::vec3(0.0f);

	pool.Emit(1, comp, glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f));
	pool.GetParticlesMut()[0].Velocity = glm::vec3(0.0f, -5.0f, 0.0f);

	// Advance 0.5s -> hits ground and triggers OnCollision sub-emitter
	pool.Update(0.5f, comp, glm::vec3(0.0f));
	assert(pool.GetSecondaryPool() != nullptr);
	assert(pool.GetSecondaryPool()->ActiveCount() == 6);

	std::cout << "[PASS] TestParticleSubEmitters\n";
}

static void TestParticleOneShotSpawn()
{
	ParticleSystem sys;
	ParticleSystemComponent comp;
	comp.MaxParticles = 20;
	comp.Lifetime = 0.5f;
	comp.Speed = 4.0f;

	ParticleMaterialSettings mat;
	sys.SpawnOneShot(comp, mat, glm::vec3(1.0f, 2.0f, 3.0f));
	assert(sys.GetOneShots().size() == 1);
	assert(sys.ActiveOneShotCount() > 0);

	// Advance past lifetime -> one-shot should automatically clean up
	sys.UpdateOneShots(0.6f);
	assert(sys.GetOneShots().empty());
	assert(sys.ActiveOneShotCount() == 0);

	std::cout << "[PASS] TestParticleOneShotSpawn\n";
}

static void TestForceDynamicsAndBursts()
{
	ParticlePool pool(100);
	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.Speed = 0.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 1;

	pool.AddBurst(0.0f, 15);
	pool.AddBurst(0.5f, 25);

	pool.Update(0.016f, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 15);

	pool.Update(0.384f, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 15);

	pool.Update(0.15f, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 40);

	// Linear Drag
	ParticlePool dragPool(10);
	dragPool.SetLinearDrag(1.0f);
	comp.Speed = 0.0f;
	dragPool.Emit(1, comp, glm::vec3(0.0f));
	dragPool.GetParticlesMut()[0].Velocity = glm::vec3(10.0f, 0.0f, 0.0f);

	dragPool.Update(0.5f, comp, glm::vec3(0.0f));
	assert(dragPool.GetParticles()[0].Velocity.x > 4.5f && dragPool.GetParticles()[0].Velocity.x < 5.5f);

	// Vortex Swirl
	ParticlePool vortexPool(10);
	vortexPool.SetVortex(4.0f, glm::vec3(0.0f, 1.0f, 0.0f));
	vortexPool.Emit(1, comp, glm::vec3(0.0f));
	vortexPool.GetParticlesMut()[0].Position = glm::vec3(2.0f, 0.0f, 0.0f);
	vortexPool.GetParticlesMut()[0].Velocity = glm::vec3(0.0f);

	vortexPool.Update(0.5f, comp, glm::vec3(0.0f));
	assert(vortexPool.GetParticles()[0].Velocity.z < -0.5f);

	std::cout << "[PASS] TestForceDynamicsAndBursts\n";
}

static void TestFacingModes()
{
	ParticlePool pool(10);
	ParticleSystemComponent comp;
	comp.Lifetime = 2.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 1;

	ParticleMaterialSettings mat;
	mat.FacingMode = ParticleRenderMode::StretchedVelocity;
	pool.SetMaterialSettings(mat);

	pool.Emit(1, comp, glm::vec3(0.0f));
	pool.GetParticlesMut()[0].Velocity = glm::vec3(0.0f, 10.0f, 0.0f);

	std::vector<ParticleVertex> verts;
	pool.GenerateBillboardVertices(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 1.0f, 0.0f), verts);
	assert(verts.size() == 6);

	mat.FacingMode = ParticleRenderMode::HorizontalQuad;
	pool.SetMaterialSettings(mat);
	verts.clear();
	pool.GenerateBillboardVertices(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), verts);
	assert(verts.size() == 6);
	for (const auto& v : verts)
	{
		assert(std::abs(v.Position.y) < 1e-4f);
	}

	std::cout << "[PASS] TestFacingModes\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldParticleSystemTests...\n";

	TestPoolInitializationAndCapacity();
	TestGeometricEmissionAndShapes();
	TestParticleUpdateAndLifetime();
	TestLockToXYPlane();
	TestVertexGeneration();
	TestParticleSystemManager();
	TestSubUVFlipbookAndRect();
	TestSoftParticleDepthFadeAndEmissive();
	TestDeterministicSeedReplay();
	TestParticlePlaneCollisionAndBounce();
	TestParticleSubEmitters();
	TestParticleOneShotSpawn();
	TestForceDynamicsAndBursts();
	TestFacingModes();

	std::cout << "All WorldParticleSystemTests passed successfully.\n";
	return 0;
}
