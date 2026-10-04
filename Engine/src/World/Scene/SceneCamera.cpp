#include "wldpch.h"
#include "World/Scene/SceneCamera.h"

// PECS(相机组件数据导向化):SceneCamera 类已删除并降级为纯参数聚合 CameraSettings。
// 投影矩阵是派生量,统一由 CameraSystem 按指纹缓存(见 Systems/CameraSystem.*)。
