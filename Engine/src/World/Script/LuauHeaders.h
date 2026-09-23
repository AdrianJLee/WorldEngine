#pragma once

// 锁定 Luau 的头文件路径。
//
// 为什么用这个 shim:W1b 之前 PUC Lua 与 Luau 都有 `lua.h`/`lualib.h`,而 World 的 include
// 目录里同时存在两者,`#include "lua.h"` 会命中先生效的那个(实测命中过 PUC Lua →
// luaopen_bit32/luaL_sandbox/luau_load 全部"未声明")。PUC Lua 已在 W1b 删除,同名冲突不再存在,
// 但保留显式相对路径仍然更稳:脚本前端的头文件来源只有这一个事实源,不依赖 include 顺序。

#include "../../../../third_party/luau/VM/include/lua.h"
#include "../../../../third_party/luau/VM/include/lualib.h"
