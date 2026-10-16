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

	std::cout << "All WorldParticleSystemTests passed successfully.\n";
	return 0;
}
