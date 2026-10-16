#include "World/Math/Curve/CubicBezierCurve.h"
#include "World/Core/Log.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestKeyframeManagement()
{
	CubicBezierCurve curve;
	assert(curve.KeyframeCount() == 0);

	curve.AddKeyframe(Keyframe(1.0f, 10.0f));
	curve.AddKeyframe(Keyframe(0.0f, 0.0f));
	curve.AddKeyframe(Keyframe(0.5f, 5.0f));

	assert(curve.KeyframeCount() == 3);
	// Sorted order check
	assert(curve.GetKeyframe(0).Time == 0.0f);
	assert(curve.GetKeyframe(1).Time == 0.5f);
	assert(curve.GetKeyframe(2).Time == 1.0f);

	curve.RemoveKeyframe(1);
	assert(curve.KeyframeCount() == 2);
	assert(curve.GetKeyframe(0).Time == 0.0f);
	assert(curve.GetKeyframe(1).Time == 1.0f);

	curve.Clear();
	assert(curve.KeyframeCount() == 0);

	std::cout << "[PASS] TestKeyframeManagement\n";
}

static void TestLinearEvaluation()
{
	auto curve = CubicBezierCurve::Linear();
	assert(std::abs(curve.Evaluate(0.0f) - 0.0f) < 1e-4f);
	assert(std::abs(curve.Evaluate(0.5f) - 0.5f) < 1e-4f);
	assert(std::abs(curve.Evaluate(1.0f) - 1.0f) < 1e-4f);

	// Boundary clamping
	assert(std::abs(curve.Evaluate(-0.5f) - 0.0f) < 1e-4f);
	assert(std::abs(curve.Evaluate(1.5f) - 1.0f) < 1e-4f);

	std::cout << "[PASS] TestLinearEvaluation\n";
}

static void TestEasingPresets()
{
	auto easeIn = CubicBezierCurve::EaseIn();
	auto easeOut = CubicBezierCurve::EaseOut();
	auto easeInOut = CubicBezierCurve::EaseInOut();

	assert(std::abs(easeIn.Evaluate(0.0f) - 0.0f) < 1e-4f);
	assert(std::abs(easeIn.Evaluate(1.0f) - 1.0f) < 1e-4f);
	// EaseIn starts slow: at t=0.2, value should be below linear (0.2)
	assert(easeIn.Evaluate(0.2f) < 0.2f);

	assert(std::abs(easeOut.Evaluate(0.0f) - 0.0f) < 1e-4f);
	assert(std::abs(easeOut.Evaluate(1.0f) - 1.0f) < 1e-4f);
	// EaseOut starts fast: at t=0.2, value should be above linear (0.2)
	assert(easeOut.Evaluate(0.2f) > 0.2f);

	// EaseInOut is symmetric around midpoint
	assert(std::abs(easeInOut.Evaluate(0.5f) - 0.5f) < 0.05f);

	auto bounce = CubicBezierCurve::Bounce();
	assert(std::abs(bounce.Evaluate(0.0f) - 0.0f) < 1e-4f);
	assert(std::abs(bounce.Evaluate(1.0f) - 1.0f) < 1e-4f);

	std::cout << "[PASS] TestEasingPresets\n";
}

static void TestLutPrecomputation()
{
	auto curve = CubicBezierCurve::EaseInOut();
	curve.PrecomputeLut(256);
	assert(curve.GetLut().size() == 256);

	// Compare EvaluateFastLut with Evaluate
	for (float t = 0.0f; t <= 1.0f; t += 0.05f)
	{
		float analytical = curve.Evaluate(t);
		float lutVal = curve.EvaluateFastLut(t);
		assert(std::abs(analytical - lutVal) < 0.01f);
	}

	std::cout << "[PASS] TestLutPrecomputation\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldCurveEditorTests...\n";

	TestKeyframeManagement();
	TestLinearEvaluation();
	TestEasingPresets();
	TestLutPrecomputation();

	std::cout << "All WorldCurveEditorTests passed successfully.\n";
	return 0;
}
