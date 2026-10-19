#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleBuffer.h"
#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Math/Curve/ColorGradient.h"
#include "World/Math/Simd/ParticleSimdBatch.h"
#include "World/Math/Simd/CurlNoise.h"
#include "World/Renderer/GpuParticleSystem.h"
#include "World/Renderer/ParticleRibbon.h"
#include "World/Renderer/ParticleMesh.h"
#include "World/Renderer/ParticleLight.h"
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

	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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

	ParticleEmitterConfig comp;
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

	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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

	ParticleEmitterConfig comp;
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

	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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
	ParticleEmitterConfig comp;
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

static void TestColorGradientLut()
{
	ColorGradient grad;
	grad.AddColorStop(0.0f, glm::vec3(1.0f, 0.0f, 0.0f));
	grad.AddColorStop(0.5f, glm::vec3(0.0f, 1.0f, 0.0f));
	grad.AddColorStop(1.0f, glm::vec3(0.0f, 0.0f, 1.0f));
	grad.AddAlphaStop(0.0f, 1.0f);
	grad.AddAlphaStop(1.0f, 0.0f);

	assert(grad.ColorStopCount() == 3);
	assert(grad.AlphaStopCount() == 2);

	grad.PrecomputeLut(256);
	assert(grad.GetLut().size() == 256);

	const glm::vec4 c0 = grad.EvaluateFastLut(0.0f);
	assert(std::abs(c0.r - 1.0f) < 1e-4f);
	assert(std::abs(c0.g - 0.0f) < 1e-4f);
	assert(std::abs(c0.b - 0.0f) < 1e-4f);
	assert(std::abs(c0.a - 1.0f) < 1e-4f);

	const glm::vec4 cMid = grad.EvaluateFastLut(0.5f);
	assert(std::abs(cMid.r - 0.0f) < 1e-2f);
	assert(std::abs(cMid.g - 1.0f) < 1e-2f);
	assert(std::abs(cMid.b - 0.0f) < 1e-2f);
	assert(std::abs(cMid.a - 0.5f) < 1e-2f);

	const glm::vec4 c1 = grad.EvaluateFastLut(1.0f);
	assert(std::abs(c1.r - 0.0f) < 1e-4f);
	assert(std::abs(c1.g - 0.0f) < 1e-4f);
	assert(std::abs(c1.b - 1.0f) < 1e-4f);
	assert(std::abs(c1.a - 0.0f) < 1e-4f);

	// Presets
	ColorGradient fire = ColorGradient::DefaultFire();
	assert(fire.ColorStopCount() >= 4);
	assert(fire.AlphaStopCount() >= 4);
	assert(fire.GetLut().size() == 256);

	ColorGradient smoke = ColorGradient::DefaultSmoke();
	assert(smoke.ColorStopCount() >= 4);
	assert(smoke.AlphaStopCount() >= 4);

	// YAML roundtrip
	const std::string yaml = grad.SerializeToYaml();
	assert(!yaml.empty());
	ColorGradient deserialized;
	const bool loadOk = deserialized.DeserializeFromYaml(yaml);
	assert(loadOk);
	assert(deserialized.ColorStopCount() == 3);
	assert(deserialized.AlphaStopCount() == 2);

	const glm::vec4 cDeserMid = deserialized.EvaluateFastLut(0.5f);
	assert(std::abs(cDeserMid.g - 1.0f) < 1e-2f);
	assert(std::abs(cDeserMid.a - 0.5f) < 1e-2f);

	std::cout << "[PASS] TestColorGradientLut\n";
}

static void TestParticleSoABatchSimd()
{
	// 1. Direct SIMD 8-batch verification against analytical step
	float px[8], py[8], pz[8];
	float vx[8], vy[8], vz[8];
	float life[8];

	for (size_t i = 0; i < 8; ++i)
	{
		px[i] = static_cast<float>(i * 2);
		py[i] = 10.0f;
		pz[i] = 0.0f;
		vx[i] = 1.0f;
		vy[i] = 0.0f;
		vz[i] = -0.5f;
		life[i] = 2.0f;
	}

	World::Math::Simd::ParticleBatchParams params;
	params.Dt = 0.1f;
	params.DragFactor = 0.95f;
	params.GravityX = 0.0f;
	params.GravityY = -9.8f;
	params.GravityZ = 0.0f;

	World::Math::Simd::StepParticlesSimd8(px, py, pz, vx, vy, vz, life, params);

	for (size_t i = 0; i < 8; ++i)
	{
		const float expectedLife = 2.0f - 0.1f;
		const float expectedVy = 0.0f * 0.95f + (-9.8f) * 0.1f;
		const float expectedPy = 10.0f + expectedVy * 0.1f;
		const float expectedVx = 1.0f * 0.95f;
		const float expectedPx = static_cast<float>(i * 2) + expectedVx * 0.1f;

		assert(std::abs(life[i] - expectedLife) < 1e-4f);
		assert(std::abs(vy[i] - expectedVy) < 1e-4f);
		assert(std::abs(py[i] - expectedPy) < 1e-4f);
		assert(std::abs(vx[i] - expectedVx) < 1e-4f);
		assert(std::abs(px[i] - expectedPx) < 1e-4f);
	}

	// 2. High-level ParticleBuffer and SoA lifecycle & batch update
	ParticleBuffer buffer(32);
	assert(buffer.Capacity() == 32);
	assert(buffer.ActiveCount() == 0);

	for (size_t i = 0; i < 16; ++i)
	{
		size_t idx = buffer.Allocate();
		assert(idx == i);
		buffer.GetSoA().PosX[idx] = static_cast<float>(i);
		buffer.GetSoA().PosY[idx] = 5.0f;
		buffer.GetSoA().PosZ[idx] = 0.0f;
		buffer.GetSoA().VelX[idx] = 0.0f;
		buffer.GetSoA().VelY[idx] = 2.0f;
		buffer.GetSoA().VelZ[idx] = 0.0f;
		buffer.GetSoA().Life[idx] = 1.0f;
		buffer.GetSoA().MaxLife[idx] = 1.0f;
	}
	assert(buffer.ActiveCount() == 16);

	ParticleBufferUpdateParams bParams;
	bParams.Dt = 0.05f;
	bParams.Gravity = glm::vec3(0.0f, -10.0f, 0.0f);
	bParams.LinearDrag = 0.0f;

	buffer.UpdateStep(bParams);
	assert(buffer.ActiveCount() == 16);

	for (size_t i = 0; i < 16; ++i)
	{
		assert(std::abs(buffer.GetSoA().Life[i] - 0.95f) < 1e-4f);
		assert(std::abs(buffer.GetSoA().VelY[i] - (2.0f - 10.0f * 0.05f)) < 1e-4f);
	}

	// 3. SwapAndPop death cleanup
	buffer.Kill(0);
	assert(buffer.ActiveCount() == 15);

	std::cout << "[PASS] TestParticleSoABatchSimd\n";
}

static void TestParticleCurveEvolution()
{
	ParticlePool pool(10);

	// Setup EaseIn size curve
	CubicBezierCurve sizeCurve = CubicBezierCurve::EaseIn();
	pool.SetSizeCurve(sizeCurve);
	assert(pool.HasSizeCurve());

	// Setup ColorGradient
	ColorGradient grad;
	grad.AddColorStop(0.0f, glm::vec3(1.0f, 0.0f, 0.0f));
	grad.AddColorStop(1.0f, glm::vec3(0.0f, 0.0f, 1.0f));
	grad.AddAlphaStop(0.0f, 1.0f);
	grad.AddAlphaStop(1.0f, 0.0f);
	grad.PrecomputeLut(256);
	pool.SetColorGradient(grad);
	assert(pool.HasColorGradient());

	// Setup SpeedCurve
	CubicBezierCurve speedCurve = CubicBezierCurve::Linear();
	pool.SetSpeedCurve(speedCurve);
	assert(pool.HasSpeedCurve());

	ParticleEmitterConfig comp;
	comp.Lifetime = 1.0f;
	comp.Speed = 0.0f;
	comp.EmissionRate = 0.0f;
	comp.IsPlaying = 1;
	comp.SizeStart = 1.0f;
	comp.SizeEnd = 1.0f;

	pool.Emit(1, comp, glm::vec3(0.0f));
	assert(pool.ActiveCount() == 1);

	// Advance to half life (t = 0.5)
	pool.Update(0.5f, comp, glm::vec3(0.0f));

	const auto& particles = pool.GetParticles();
	assert(particles[0].Active);
	assert(std::abs(particles[0].Life - 0.5f) < 1e-3f);

	// Assert Color from ColorGradient
	const glm::vec4 expectedCol = grad.EvaluateFastLut(0.5f);
	assert(std::abs(particles[0].Color.r - expectedCol.r) < 0.05f);
	assert(std::abs(particles[0].Color.b - expectedCol.b) < 0.05f);
	assert(std::abs(particles[0].Color.a - expectedCol.a) < 0.05f);

	// Assert Size from SizeCurve
	const float expectedSize = sizeCurve.EvaluateFastLut(0.5f);
	assert(std::abs(particles[0].Size.x - expectedSize) < 0.05f);

	// Verify SoA buffer also captured active particles
	assert(pool.GetBuffer().ActiveCount() == 1);
	assert(std::abs(pool.GetSoA().Life[0] - 0.5f) < 1e-3f);
	assert(std::abs(pool.GetSoA().ColorR[0] - expectedCol.r) < 0.05f);
	assert(std::abs(pool.GetSoA().SizeX[0] - expectedSize) < 0.05f);

	std::cout << "[PASS] TestParticleCurveEvolution\n";
}

static void TestCurlNoiseDivergenceFree()
{
	const glm::vec3 samplePoints[] = {
		{ 0.0f, 0.0f, 0.0f },
		{ 1.5f, 2.7f, -3.1f },
		{ -10.2f, 4.8f, 15.3f },
		{ 100.0f, -50.0f, 25.0f },
		{ 0.123f, -0.456f, 0.789f }
	};

	constexpr float delta = 1e-3f;
	constexpr float inv2delta = 0.5f / delta;

	for (const auto& pt : samplePoints)
	{
		glm::vec3 vx_plus  = Math::Simd::ComputeCurlNoise(pt + glm::vec3(delta, 0.0f, 0.0f), 1.0f, 1u);
		glm::vec3 vx_minus = Math::Simd::ComputeCurlNoise(pt - glm::vec3(delta, 0.0f, 0.0f), 1.0f, 1u);

		glm::vec3 vy_plus  = Math::Simd::ComputeCurlNoise(pt + glm::vec3(0.0f, delta, 0.0f), 1.0f, 1u);
		glm::vec3 vy_minus = Math::Simd::ComputeCurlNoise(pt - glm::vec3(0.0f, delta, 0.0f), 1.0f, 1u);

		glm::vec3 vz_plus  = Math::Simd::ComputeCurlNoise(pt + glm::vec3(0.0f, 0.0f, delta), 1.0f, 1u);
		glm::vec3 vz_minus = Math::Simd::ComputeCurlNoise(pt - glm::vec3(0.0f, 0.0f, delta), 1.0f, 1u);

		float dvx_dx = (vx_plus.x - vx_minus.x) * inv2delta;
		float dvy_dy = (vy_plus.y - vy_minus.y) * inv2delta;
		float dvz_dz = (vz_plus.z - vz_minus.z) * inv2delta;

		float divergence = dvx_dx + dvy_dy + dvz_dz;
		assert(std::abs(divergence) < 5e-3f);
	}

	float posX[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
	float posY[4] = { 0.0f, 0.5f, 1.0f, 1.5f };
	float posZ[4] = { 0.0f, -0.5f, -1.0f, -1.5f };
	float curlX[4] = { 0.0f };
	float curlY[4] = { 0.0f };
	float curlZ[4] = { 0.0f };

	Math::Simd::ComputeCurlNoiseBatch(posX, posY, posZ, curlX, curlY, curlZ, 4, 1.0f, 1u);
	for (size_t i = 0; i < 4; ++i)
	{
		glm::vec3 expected = Math::Simd::ComputeCurlNoise(glm::vec3(posX[i], posY[i], posZ[i]), 1.0f, 1u);
		assert(std::abs(curlX[i] - expected.x) < 1e-5f);
		assert(std::abs(curlY[i] - expected.y) < 1e-5f);
		assert(std::abs(curlZ[i] - expected.z) < 1e-5f);
	}

	std::cout << "[PASS] TestCurlNoiseDivergenceFree\n";
}

static void TestRibbonTrailGeneration()
{
	RibbonConfig config;
	config.InitialWidth = 1.0f;
	config.MaxAge = 2.0f;
	config.TextureTileLength = 5.0f;
	config.SubdivisionsPerSegment = 3;
	config.TaperWidthOverLife = true;
	config.FadeAlphaOverLife = true;

	ParticleRibbon ribbon(config);

	for (int i = 0; i < 5; ++i)
	{
		float angle = static_cast<float>(i) * 0.3f;
		glm::vec3 pos(std::cos(angle) * 5.0f, static_cast<float>(i) * 2.0f, std::sin(angle) * 5.0f);
		glm::vec4 color(1.0f, 0.5f, 0.2f, 1.0f);
		ribbon.AddPoint(pos, color, 1.0f, 2.0f);
	}

	assert(ribbon.GetControlPointCount() == 5);

	glm::vec3 cameraPos(0.0f, 5.0f, 15.0f);
	std::vector<ParticleVertex> vertices;
	std::vector<uint32_t> indices;

	ribbon.GenerateMesh(cameraPos, vertices, indices);

	assert(!vertices.empty());
	assert(!indices.empty());
	assert(indices.size() % 3 == 0);
	assert(vertices.size() % 2 == 0);

	for (size_t i = 0; i < vertices.size(); i += 2)
	{
		assert(std::abs(vertices[i].UV.x - 0.0f) < 1e-4f);
		assert(std::abs(vertices[i + 1].UV.x - 1.0f) < 1e-4f);
		assert(vertices[i].UV.y >= 0.0f);
		assert(std::abs(vertices[i].UV.y - vertices[i + 1].UV.y) < 1e-4f);
	}

	ribbon.Update(2.5f);
	assert(ribbon.GetControlPointCount() == 0);

	std::cout << "[PASS] TestRibbonTrailGeneration\n";
}

static void TestMeshParticleBatch()
{
	ParticlePool pool(100);
	ParticleEmitterConfig comp;
	comp.Lifetime = 2.0f;
	comp.Speed = 5.0f;
	comp.SizeStart = 2.0f;
	comp.SizeEnd = 2.0f;

	pool.Emit(10, comp, glm::vec3(0.0f, 5.0f, 0.0f));
	assert(pool.ActiveCount() == 10);

	ParticleMeshConfig meshConfig;
	meshConfig.MeshScale = glm::vec3(1.5f);
	meshConfig.AlignToVelocity = true;
	meshConfig.ScaleWithParticleSize = true;

	ParticleMeshBatch batch(meshConfig);
	batch.BuildInstances(pool);

	assert(batch.GetInstanceCount() == 10);

	const auto& instances = batch.GetInstances();
	for (const auto& inst : instances)
	{
		glm::vec3 translation(inst.Transform[3][0], inst.Transform[3][1], inst.Transform[3][2]);
		assert(glm::length(translation) >= 0.0f);

		float det = glm::determinant(inst.Transform);
		assert(det > 1.0f);
		assert(inst.Color.a > 0.0f);
	}

	ParticleMeshBatch soaBatch(meshConfig);
	soaBatch.BuildInstancesFromSoA(pool.GetSoA());
	assert(soaBatch.GetInstanceCount() == 10);

	std::cout << "[PASS] TestMeshParticleBatch\n";
}

static void TestParticleLightExtraction()
{
	ParticlePool pool(50);
	ParticleEmitterConfig comp;
	comp.Lifetime = 3.0f;
	comp.Speed = 0.0f;
	comp.SizeStart = 1.0f;
	comp.SizeEnd = 1.0f;

	pool.Emit(10, comp, glm::vec3(0.0f));

	auto& particles = pool.GetParticlesMut();
	particles[0].Color = glm::vec4(1.0f, 0.8f, 0.2f, 1.0f);
	particles[1].Color = glm::vec4(0.1f, 0.9f, 1.0f, 1.0f);
	particles[2].Color = glm::vec4(1.0f, 0.1f, 0.1f, 1.0f);
	for (size_t i = 3; i < 10; ++i)
	{
		particles[i].Color = glm::vec4(0.05f, 0.05f, 0.05f, 0.5f);
	}

	ParticleLightConfig lightConfig;
	lightConfig.IntensityThreshold = 0.5f;
	lightConfig.MaxLights = 16;
	lightConfig.RangeMultiplier = 10.0f;
	lightConfig.IntensityScale = 2.0f;

	ParticleLightExtractor extractor(lightConfig);
	std::vector<PointLightData> extractedLights;
	size_t count = extractor.ExtractLights(pool, extractedLights);

	assert(count == 3);
	assert(extractedLights.size() == 3);
	assert(extractedLights[0].Intensity >= extractedLights[1].Intensity);
	assert(extractedLights[1].Intensity >= extractedLights[2].Intensity);
	assert(extractedLights[0].Range >= 5.0f);

	std::vector<PointLightData> sceneLights;
	sceneLights.push_back(PointLightData { glm::vec3(1.0f), 1.0f, glm::vec3(0.0f), 10.0f });
	extractor.InjectIntoLightRig(pool, sceneLights, 4);

	assert(sceneLights.size() == 4);

	std::cout << "[PASS] TestParticleLightExtraction\n";
}

static void TestGpuParticleSystem()
{
	GpuParticleSystem gpuSys(nullptr, 1000);
	assert(gpuSys.GetMaxParticles() == 1000);
	assert(gpuSys.GetActiveCount() == 0);

	GpuParticle proto;
	proto.PositionLife = glm::vec4(0.0f, 10.0f, 0.0f, 2.0f);
	proto.VelocityMaxLife = glm::vec4(0.0f, 5.0f, 0.0f, 2.0f);
	proto.Color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
	proto.SizeRotation = glm::vec4(1.0f, 1.0f, 0.0f, 0.5f);

	gpuSys.Emit(50, proto);
	assert(gpuSys.GetActiveCount() == 50);
	assert(gpuSys.GetIndirectCommand().InstanceCount == 50);
	assert(gpuSys.GetIndirectCommand().VertexCount == 4);

	GpuSimulationParams params;
	params.GravityDt = glm::vec4(0.0f, -9.8f, 0.0f, 0.1f);
	params.DragTurbulence = glm::vec4(0.05f, 0.0f, 1.0f, 1.0f);

	gpuSys.Simulate(0.1f, params);

	assert(gpuSys.GetActiveCount() == 50);

	std::vector<GpuParticle> readback;
	gpuSys.ReadbackParticles(readback);
	assert(readback.size() == 50);
	assert(readback[0].PositionLife.w < 2.0f);
	assert(readback[0].PositionLife.y != 10.0f);

	std::cout << "[PASS] TestGpuParticleSystem\n";
}

static void TestVfxComponentContract()
{
	static_assert(sizeof(VfxComponent) == 64, "VfxComponent must be 64 bytes");
	static_assert(std::is_trivially_copyable_v<VfxComponent>, "VfxComponent must be trivially copyable");

	VfxComponent vfx;
	assert(!vfx.Effect.HasPath());
	assert(vfx.PlaybackSpeed == 1.0f);
	assert(vfx.Intensity == 1.0f);
	assert(vfx.WarmupTime == 0.0f);
	assert(vfx.CullDistance == 60.0f);
	assert(vfx.PlaybackState == VfxPlaybackState::Playing);
	assert(vfx.Looping == true);
	assert(vfx.PlayOnAwake == true);

	vfx.PlaybackState = VfxPlaybackState::Paused;
	assert(vfx.PlaybackState == VfxPlaybackState::Paused);

	std::cout << "[PASS] TestVfxComponentContract\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldParticleSystemTests...\n";

	TestVfxComponentContract();
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
	TestColorGradientLut();
	TestParticleSoABatchSimd();
	TestParticleCurveEvolution();
	TestCurlNoiseDivergenceFree();
	TestRibbonTrailGeneration();
	TestMeshParticleBatch();
	TestParticleLightExtraction();
	TestGpuParticleSystem();

	std::cout << "All WorldParticleSystemTests passed successfully.\n";
	return 0;
}
