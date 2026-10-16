#include "World/Level/Streaming/SpatialGridManager.h"
#include "World/Core/Log.h"

#include <cassert>
#include <iostream>

using namespace World::Level;

static void TestSpatialHashAndCoordinates()
{
	glm::vec3 p1(10.0f, 20.0f, 30.0f);
	ChunkCoord3D c1 = ChunkCoord3D::WorldToChunk(p1, 64.0f);
	assert(c1.x == 0 && c1.y == 0 && c1.z == 0);

	glm::vec3 p2(65.0f, -10.0f, 130.0f);
	ChunkCoord3D c2 = ChunkCoord3D::WorldToChunk(p2, 64.0f);
	assert(c2.x == 1 && c2.y == -1 && c2.z == 2);

	glm::vec3 center = c2.GetWorldCenter(64.0f);
	assert(center.x == 96.0f && center.y == -32.0f && center.z == 160.0f);

	uint64_t id = c2.ToId();
	assert(id != 0);

	std::cout << "[PASS] TestSpatialHashAndCoordinates\n";
}

static void TestStreamingEvaluationRadii()
{
	SpatialGridManager grid(64.0f, 64.0f, 128.0f, 192.0f);
	glm::vec3 viewer(0.0f, 0.0f, 0.0f);

	auto eval = grid.Evaluate(viewer, false);
	assert(!eval.ChunksToLoadNear.empty());
	assert(!eval.ChunksToLoadMid.empty());
	assert(!eval.ChunksToLoadFar.empty());

	for (const auto& c : eval.ChunksToLoadNear)
		grid.MarkChunkLoaded(c, StreamingLodTier::Near);

	assert(grid.ActiveChunkCount() == eval.ChunksToLoadNear.size());

	auto eval2D = grid.Evaluate(viewer, true);
	for (const auto& c : eval2D.ChunksToLoadNear)
		assert(c.z == 0);
	for (const auto& c : eval2D.ChunksToLoadMid)
		assert(c.z == 0);
	for (const auto& c : eval2D.ChunksToLoadFar)
		assert(c.z == 0);

	std::cout << "[PASS] TestStreamingEvaluationRadii\n";
}

int main()
{
	World::Log::Init();
	std::cout << "Running WorldSpatialGridStreamingTests...\n";

	TestSpatialHashAndCoordinates();
	TestStreamingEvaluationRadii();

	std::cout << "All WorldSpatialGridStreamingTests passed successfully.\n";
	return 0;
}
