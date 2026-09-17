#pragma once

// 锁定 Luau 的头文件路径。
//
// 为什么需要这个 shim:PUC Lua(W2 完成前仍在树里)与 Luau 都有 `lua.h`/`lualib.h`,
// 而 World 的 include 目录里同时存在 `vendor/lua/src` 与 `vendor/luau/VM/include`,
// `#include "lua.h"` 会命中先生效的那个(实测命中了 PUC Lua → luaopen_bit32/luaL_sandbox/
// luau_load 全部"未声明")。这里用相对路径显式指向 Luau;W2 删掉 PUC Lua 后可以简化,
// 但保留 shim 仍然更稳(不再依赖 include 顺序)。

#include "../../../vendor/luau/VM/include/lua.h"
#include "../../../vendor/luau/VM/include/lualib.h"
