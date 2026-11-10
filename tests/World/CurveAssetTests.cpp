#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Math/Curve/CurveLibrary.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Core/WorldContext.h"
#include "World/Core/Log.h"
#include "World/Script/Vm/LuauVm.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

using namespace World;

static void TestExtrapolations()
{
	CubicBezierCurve curve;
	// (0.0, 0.0) with InTangent=2, OutTangent=2
	// (1.0, 10.0) with InTangent=3, OutTangent=3
	curve.AddKeyframe(Keyframe(0.0f, 0.0f, 2.0f, 2.0f, TangentMode::Aligned));
	curve.AddKeyframe(Keyframe(1.0f, 10.0f, 3.0f, 3.0f, TangentMode::Aligned));

	// 1. Clamp
	curve.PreExtrapolation = CurveExtrapolation::Clamp;
	curve.PostExtrapolation = CurveExtrapolation::Clamp;
	assert(std::abs(curve.Evaluate(-1.0f) - 0.0f) < 1e-4f);
	assert(std::abs(curve.Evaluate(2.0f) - 10.0f) < 1e-4f);

	// 2. Linear
	curve.PreExtrapolation = CurveExtrapolation::Linear;
	curve.PostExtrapolation = CurveExtrapolation::Linear;
	// t = -1.0: 0.0 + (-1.0 - 0.0) * InTangent(2.0) = -2.0
	assert(std::abs(curve.Evaluate(-1.0f) - (-2.0f)) < 1e-4f);
	// t = 2.0: 10.0 + (2.0 - 1.0) * OutTangent(3.0) = 13.0
	assert(std::abs(curve.Evaluate(2.0f) - 13.0f) < 1e-4f);

	// 3. Cycle
	curve.PreExtrapolation = CurveExtrapolation::Cycle;
	curve.PostExtrapolation = CurveExtrapolation::Cycle;
	float midVal = curve.Evaluate(0.4f);
	assert(std::abs(curve.Evaluate(1.4f) - midVal) < 1e-4f);
	assert(std::abs(curve.Evaluate(2.4f) - midVal) < 1e-4f);
	assert(std::abs(curve.Evaluate(-0.6f) - midVal) < 1e-4f);

	// 4. PingPong
	curve.PreExtrapolation = CurveExtrapolation::PingPong;
	curve.PostExtrapolation = CurveExtrapolation::PingPong;
	// Post-extrapolation: span=1.0. t = 1.25 -> bounces: 1.0 - 0.25 = 0.75
	assert(std::abs(curve.Evaluate(1.25f) - curve.Evaluate(0.75f)) < 1e-4f);
	// t = 2.25 -> bounces: 0.0 + 0.25 = 0.25
	assert(std::abs(curve.Evaluate(2.25f) - curve.Evaluate(0.25f)) < 1e-4f);
	// Pre-extrapolation: t = -0.25 -> bounces: 0.0 + 0.25 = 0.25
	assert(std::abs(curve.Evaluate(-0.25f) - curve.Evaluate(0.25f)) < 1e-4f);
	// t = -1.25 -> bounces: 1.0 - 0.25 = 0.75
	assert(std::abs(curve.Evaluate(-1.25f) - curve.Evaluate(0.75f)) < 1e-4f);

	std::cout << "[PASS] TestExtrapolations\n";
}

static void TestAutoSmoothTangents()
{
	CubicBezierCurve curve;
	curve.AddKeyframe(Keyframe(0.0f, 0.0f));
	curve.AddKeyframe(Keyframe(1.0f, 2.0f));
	curve.AddKeyframe(Keyframe(2.0f, 0.0f));

	curve.AutoSmoothTangents();

	// Middle keyframe: (0,0) and (2,0) -> Catmull-Rom slope = (0 - 0) / (2 - 0) = 0
	assert(std::abs(curve.GetKeyframe(1).InTangent) < 1e-4f);
	assert(std::abs(curve.GetKeyframe(1).OutTangent) < 1e-4f);
	assert(curve.GetKeyframe(1).Mode == TangentMode::Aligned);

	// Start keyframe: slope to next = (2 - 0) / (1 - 0) = 2.0
	assert(std::abs(curve.GetKeyframe(0).InTangent - 2.0f) < 1e-4f);
	assert(std::abs(curve.GetKeyframe(0).OutTangent - 2.0f) < 1e-4f);

	// End keyframe: slope from prev = (0 - 2) / (2 - 1) = -2.0
	assert(std::abs(curve.GetKeyframe(2).InTangent - (-2.0f)) < 1e-4f);
	assert(std::abs(curve.GetKeyframe(2).OutTangent - (-2.0f)) < 1e-4f);

	// Test FindNearestKeyframe
	int hit = curve.FindNearestKeyframe(1.01f, 1.99f, 0.05f, 0.05f);
	assert(hit == 1);

	int miss = curve.FindNearestKeyframe(5.0f, 5.0f, 0.05f, 0.05f);
	assert(miss == -1);

	std::cout << "[PASS] TestAutoSmoothTangents\n";
}

static void TestYamlSerializationRoundtrip()
{
	CubicBezierCurve curve;
	curve.PreExtrapolation = CurveExtrapolation::Linear;
	curve.PostExtrapolation = CurveExtrapolation::PingPong;
	curve.AddKeyframe(Keyframe(0.0f, 1.0f, 0.5f, 0.6f, TangentMode::Free));
	curve.AddKeyframe(Keyframe(0.5f, 3.0f, 1.2f, 1.2f, TangentMode::Aligned));
	curve.AddKeyframe(Keyframe(1.0f, 2.0f, -0.5f, -0.5f, TangentMode::Clamped));

	std::string yaml = curve.SerializeToYaml();
	assert(!yaml.empty());
	assert(yaml.find("Linear") != std::string::npos);
	assert(yaml.find("PingPong") != std::string::npos);
	assert(yaml.find("Keyframes") != std::string::npos);

	CubicBezierCurve loaded;
	bool desOk = loaded.DeserializeFromYaml(yaml);
	assert(desOk);
	assert(loaded.PreExtrapolation == CurveExtrapolation::Linear);
	assert(loaded.PostExtrapolation == CurveExtrapolation::PingPong);
	assert(loaded.KeyframeCount() == 3);
	assert(std::abs(loaded.GetKeyframe(0).Time - 0.0f) < 1e-4f);
	assert(std::abs(loaded.GetKeyframe(0).Value - 1.0f) < 1e-4f);
	assert(std::abs(loaded.GetKeyframe(0).InTangent - 0.5f) < 1e-4f);
	assert(std::abs(loaded.GetKeyframe(0).OutTangent - 0.6f) < 1e-4f);
	assert(loaded.GetKeyframe(0).Mode == TangentMode::Free);
	assert(loaded.GetKeyframe(1).Mode == TangentMode::Aligned);
	assert(loaded.GetKeyframe(2).Mode == TangentMode::Clamped);

	std::filesystem::create_directories("tmp");
	const std::string testFile = "tmp/curve_roundtrip_test.wcurve";
	assert(curve.SaveToFile(testFile));

	CubicBezierCurve fileLoaded;
	assert(fileLoaded.LoadFromFile(testFile));
	assert(fileLoaded.KeyframeCount() == 3);
	assert(fileLoaded.PreExtrapolation == CurveExtrapolation::Linear);
	assert(fileLoaded.PostExtrapolation == CurveExtrapolation::PingPong);

	std::cout << "[PASS] TestYamlSerializationRoundtrip\n";
}

static void TestCurveLibrary()
{
	std::filesystem::create_directories("tmp");
	const std::string libTestFile = "tmp/lib_test.wcurve";
	CubicBezierCurve sampleCurve = CubicBezierCurve::EaseInOut();
	assert(sampleCurve.SaveToFile(libTestFile));

	CurveLibrary::Get().Clear();
	auto c1 = CurveLibrary::Get().Load(libTestFile);
	assert(c1 != nullptr);

	auto c2 = CurveLibrary::Get().Get(libTestFile);
	assert(c1 == c2); // Identity check, cached

	sampleCurve.AddKeyframe(Keyframe(2.0f, 5.0f));
	assert(sampleCurve.SaveToFile(libTestFile));

	CurveLibrary::Get().Reload(libTestFile);
	auto c3 = CurveLibrary::Get().Get(libTestFile);
	assert(c3 != nullptr);
	assert(c3->KeyframeCount() == sampleCurve.KeyframeCount());

	CurveLibrary::Get().Clear();
	std::cout << "[PASS] TestCurveLibrary\n";
}

static void TestLuauCurveBindings()
{
	ScriptEngine::Init();

	const std::string script = R"LUAU(
		local lin = Curve.EvaluatePreset("Linear", 0.5)
		assert(math.abs(lin - 0.5) < 0.01)

		local ei = Curve.EvaluatePreset("EaseIn", 0.0)
		assert(math.abs(ei - 0.0) < 0.01)

		local eo = Curve.EvaluatePreset("EaseOut", 1.0)
		assert(math.abs(eo - 1.0) < 0.01)

		local eio = Curve.EvaluatePreset("EaseInOut", 0.5)
		assert(math.abs(eio - 0.5) < 0.05)

		local bounce = Curve.EvaluatePreset("Bounce", 1.0)
		assert(math.abs(bounce - 1.0) < 0.01)

		local fileVal = Curve.Evaluate("tmp/lib_test.wcurve", 0.5)
		assert(type(fileVal) == "number")
	)LUAU";

	std::string err;
	bool ok = ScriptEngine::GetState().RunString(script, "TestCurveBindings", &err);
	if (!ok)
	{
		std::cerr << "Script error: " << err << std::endl;
	}
	assert(ok);

	ScriptEngine::Shutdown();
	std::cout << "[PASS] TestLuauCurveBindings\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldCurveAssetTests...\n";

	TestExtrapolations();
	TestAutoSmoothTangents();
	TestYamlSerializationRoundtrip();
	TestCurveLibrary();
	TestLuauCurveBindings();

	std::cout << "All WorldCurveAssetTests passed successfully.\n";
	return 0;
}

