// cgltf 是单头库(实现体在 cgltf.h 的 CGLTF_IMPLEMENTATION 段内),上游不提供 .c 文件;
// 这里按仓库既有 stb_image.cpp 的同款做法提供一个最小编译单元。
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
