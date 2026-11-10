#include "World/Physics/PhysicsMaterial.h"
#include "World/Scene/Components.h"
#include "World/Core/Log.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World;

static void TestCombineAlgorithms()
{
	float a = 0.4f;
	float b = 0.8f;

	// Average: (0.4 + 0.8) * 0.5 = 0.6
	float avg = PhysicsMaterial::Combine(PhysMatCombineMode::Average, a, b);
	assert(std::abs(avg - 0.6f) < 1e-5f);

	// Minimum: min(0.4, 0.8) = 0.4
	float minVal = PhysicsMaterial::Combine(PhysMatCombineMode::Minimum, a, b);
	assert(std::abs(minVal - 0.4f) < 1e-5f);

	// Multiply: 0.4 * 0.8 = 0.32
	float mul = PhysicsMaterial::Combine(PhysMatCombineMode::Multiply, a, b);
	assert(std::abs(mul - 0.32f) < 1e-5f);

	// Maximum: max(0.4, 0.8) = 0.8
	float maxVal = PhysicsMaterial::Combine(PhysMatCombineMode::Maximum, a, b);
	assert(std::abs(maxVal - 0.8f) < 1e-5f);

	std::cout << "[PASS] TestCombineAlgorithms\n";
}

static void TestSerialization()
{
	const std::string testPath = "test_material.wphysmat";

	PhysicsMaterial original;
	original.Friction = 0.05f;
	original.Restitution = 0.85f;
	original.FrictionCombine = PhysMatCombineMode::Minimum;
	original.RestitutionCombine = PhysMatCombineMode::Maximum;

	bool saved = original.SaveToFile(testPath);
	assert(saved);
	assert(std::filesystem::exists(testPath));

	PhysicsMaterial loaded;
	bool loadedOk = PhysicsMaterial::LoadFromFile(testPath, loaded);
	assert(loadedOk);

	assert(std::abs(loaded.Friction - 0.05f) < 1e-5f);
	assert(std::abs(loaded.Restitution - 0.85f) < 1e-5f);
	assert(loaded.FrictionCombine == PhysMatCombineMode::Minimum);
	assert(loaded.RestitutionCombine == PhysMatCombineMode::Maximum);

	std::filesystem::remove(testPath);
	std::cout << "[PASS] TestSerialization\n";
}

static void TestPhysicsMaterialComponentDefaults()
{
	PhysicsMaterialComponent comp;
	assert(comp.Friction == 0.5f);
	assert(comp.Restitution == 0.0f);
	assert(comp.FrictionCombine == PhysMatCombineMode::Average);
	assert(comp.RestitutionCombine == PhysMatCombineMode::Average);

	std::cout << "[PASS] TestPhysicsMaterialComponentDefaults\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldPhysicsMaterialTests...\n";

	TestCombineAlgorithms();
	TestSerialization();
	TestPhysicsMaterialComponentDefaults();

	std::cout << "All WorldPhysicsMaterialTests passed successfully.\n";
	return 0;
}
