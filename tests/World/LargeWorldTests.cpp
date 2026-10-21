#include "World/Scene/LWC.h"
#include "World/Level/WorldPartition3D.h"
#include "World/Renderer/HLODManager.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestLWCPrecisionAndCameraRelative()
{
	// 场景位于 100 公里之外 (100,000 米)
	const glm::dvec3 objectWorldDouble(100000.0, 500.0, 100000.0);
	const glm::dvec3 cameraWorldDouble( 99990.0, 500.0, 100000.0); // 相机距离物体仅 10 米

	WorldTransform64 xform;
	xform.DoublePosition = objectWorldDouble;
	xform.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	xform.Scale = glm::vec3(1.0f);

	// 1. 相对坐标转换
	glm::vec3 relPos = WorldTransform64::ToCameraRelative(objectWorldDouble, cameraWorldDouble);
	assert(std::abs(relPos.x - 10.0f) < 1e-5f);
	assert(std::abs(relPos.y -  0.0f) < 1e-5f);
	assert(std::abs(relPos.z -  0.0f) < 1e-5f);

	// 2. 相对矩阵转换
	glm::mat4 relMat = xform.ToCameraRelativeMatrix(cameraWorldDouble);
	assert(std::abs(relMat[3][0] - 10.0f) < 1e-5f);
	assert(std::abs(relMat[3][1] -  0.0f) < 1e-5f);
	assert(std::abs(relMat[3][2] -  0.0f) < 1e-5f);

	std::cout << "[PASS] TestLWCPrecisionAndCameraRelative (100km Sub-millimeter Jitter-Free)\n";
}

static void TestWorldPartition3DStreaming()
{
	WorldPartition3D partition(64.0f, 128.0f);

	const glm::dvec3 viewerStart(0.0, 0.0, 0.0);
	std::vector<PartitionCell3D> toLoad, toUnload;

	partition.UpdateViewer(viewerStart, toLoad, toUnload);
	assert(!toLoad.empty());
	assert(toUnload.empty());
	assert(partition.GetActiveCells().size() == toLoad.size());

	// 移动观察者到 500m 远方
	const glm::dvec3 viewerMoved(500.0, 0.0, 500.0);
	partition.UpdateViewer(viewerMoved, toLoad, toUnload);

	assert(!toLoad.empty());
	assert(!toUnload.empty()); // 远方旧切片全部卸载

	std::cout << "[PASS] TestWorldPartition3DStreaming (Active Cells: "
		<< partition.GetActiveCells().size() << ")\n";
}

static void TestHLODEvaluation()
{
	bool drawProxy = false;
	bool drawChildren = false;

	// 1. 近距离 (50m < 200m 切换距离)
	HLODManager::EvaluateCluster(50.0f, 200.0f, 1000.0f, drawProxy, drawChildren);
	assert(!drawProxy);
	assert(drawChildren);

	// 2. 远距离 (350m > 200m 切换距离)
	HLODManager::EvaluateCluster(350.0f, 200.0f, 1000.0f, drawProxy, drawChildren);
	assert(drawProxy);
	assert(!drawChildren);

	// 3. 超出裁剪距离 (1200m > 1000m)
	HLODManager::EvaluateCluster(1200.0f, 200.0f, 1000.0f, drawProxy, drawChildren);
	assert(!drawProxy);
	assert(!drawChildren);

	std::cout << "[PASS] TestHLODEvaluation\n";
}

static void TestLargeWorldComponentLayout()
{
	HLODComponent hlod;
	assert(sizeof(hlod) == 48);
	assert(hlod.TransitionDistance == 200.0f);
	assert(hlod.CullDistance == 1000.0f);

	RVTComponent rvt;
	assert(sizeof(rvt) == 32);
	assert(rvt.SoftBlendRadius == 0.5f);

	std::cout << "[PASS] TestLargeWorldComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.LargeWorld tests...\n";
	TestLWCPrecisionAndCameraRelative();
	TestWorldPartition3DStreaming();
	TestHLODEvaluation();
	TestLargeWorldComponentLayout();
	std::cout << "All World.LargeWorld tests passed successfully!\n";
	return 0;
}
