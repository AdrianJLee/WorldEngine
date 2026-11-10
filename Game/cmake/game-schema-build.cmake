# ============================================================================
# Game 模块 schema 生成 —— **构建期**输入发现 + 账本同步 + 内容哈希增量门
# (CPPT-7/T2-C1;由 Game/CMakeLists.txt 的 GameSchema 目标用 `cmake -P` 每次构建执行)。
#
# 旧症状(输入发现是配置期 glob):删掉 <Game 源码根>/Components/*.h 之后的**第一次**构建
# 必失败 —— `schema-compiler: cannot open input file: <已删的头>` + MSB8066
# (自定义生成退出码 1),第二次才成功。原因:--input/--reg-include 列表在配置期算好,
# 而这次删除不会在构建开始前重跑那次配置。事实源因此搬到构建期:每次构建 glob 一次。
#
# 每次构建做四件事(**没变化就不写任何文件,生成物 mtime 不动**):
#   1) glob 输入:<Game 源码根>/Components/*.h;一个都没有时 ——
#      引擎骨架构建退回 <Game>/src/GameAPI.h(零声明 = 空模块),项目构建直接返回
#      (配置期已按"零声明项目编引擎自带空模块"选好了源文件,这里不该被调用);
#   2) 账本(<Game 源码根>/Generated/Game.manifest)与源码声明双向同步:
#      · 生成物顺序 = 账本顺序 ⇒ 账本条目按 "kind 短名" 规范排序(与旧配置期播种同一口径),
#        这样"删掉一个脚本头再加回来"能回到逐字节相同的账本与生成物;
#      · 声明了但账本没有 ⇒ 追加 "kind World::短名"(与编辑器 New C++ Component… 同一写法);
#      · 账本有但声明已消失 ⇒ 删除(删掉脚本头后账本随之更新,不再卡住构建);
#      · 注释行跟随其后的条目(条目被删则其注释一起走);文件头注释与文件尾注释原样保留;
#        既不是注释也不是条目的行原样保留 —— 不猜用户写了什么,真错让 schema-compiler 报;
#   3) 内容哈希 = 各输入头 sha256 + 账本 sha256 + schema-compiler 自身 sha256 + 脚本版本;
#      与 stamp 一致且生成物都在 ⇒ 直接返回(不重跑编译器、不碰生成物);
#   4) 重跑 schema-compiler,生成物用 `cmake -E copy_if_different` 回写源码树。
#
# 调用方(Game/CMakeLists.txt)通过 -D 传入,全部为绝对路径:
#   WLD_GAME_SRC_DIR          <Game 源码根>(项目模式 = <项目根>/src)
#   WLD_GAME_PROJECT_DIR      "" = 引擎骨架构建;非空 = 项目构建
#   WLD_GAME_SCHEMA_COMPILER  schema-compiler.exe
#   WLD_GAME_SCHEMA_GEN_DIR   生成中间目录(构建树)
#   WLD_GAME_SCHEMA_STAMP     内容哈希 stamp(构建树)
#   WLD_GAME_SCHEMA_MANIFEST  <Game 源码根>/Generated/Game.manifest
#   WLD_GAME_SCHEMA_HEADER    <Game 源码根>/Generated/Game/GameSchemaRegistration.h
#   WLD_GAME_SCHEMA_SOURCE    <Game 源码根>/Generated/Game/GameSchemaRegistration.cpp
#   WLD_GAME_SCHEMA_ENGINE_API <Game 模块>/src/GameAPI.h(引擎骨架模式的唯一输入)
# ============================================================================

# 脚本版本参与哈希:改动本脚本的生成口径时 +1(旧 stamp 因此失效一次)。
set(WLD_GAME_SCHEMA_SCRIPT_VERSION "2")

function(wld_game_schema_run)
    set(_src "${WLD_GAME_SRC_DIR}")
    set(_manifest "${WLD_GAME_SCHEMA_MANIFEST}")
    set(_gen "${WLD_GAME_SCHEMA_GEN_DIR}")
    set(_stamp "${WLD_GAME_SCHEMA_STAMP}")

    # ---- 1. 构建期输入发现 -------------------------------------------------
    file(GLOB _discovered
        "${_src}/Components/*.h")
    if(_discovered)
        set(_inputs ${_discovered})
    elseif(WLD_GAME_PROJECT_DIR)
        message(STATUS "Game schema: project declares no Components/*.h; "
            "the engine's empty-module registration is compiled instead; nothing to generate")
        return()
    else()
        set(_inputs "${WLD_GAME_SCHEMA_ENGINE_API}")
    endif()

    foreach(_input IN LISTS _inputs)
        if(NOT EXISTS "${_input}")
            message(FATAL_ERROR "Game schema: input disappeared during the build: ${_input} "
                "(the file changed while this build was running; re-run the build)")
        endif()
    endforeach()

    # ---- 2a. 从源码声明派生账本条目(与旧配置期播种同一正则、同一 World:: 前缀)----
    set(_declared "")
    foreach(_input IN LISTS _inputs)
        file(READ "${_input}" _input_text)
        string(REPLACE "\r\n" "\n" _input_text "${_input_text}")
        string(REPLACE "\n" ";" _input_lines "${_input_text}")
        foreach(_line IN LISTS _input_lines)
            # 注释行里的示意写法(例:`WE_SCHEMA_BODY(..., Struct)` 的说明)不算声明。
            string(STRIP "${_line}" _stripped)
            if(_stripped MATCHES "^//" OR _stripped MATCHES "^#")
                continue()
            endif()
            string(REGEX MATCHALL
                "WE_SCHEMA_BODY[ \t]*\\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*[A-Za-z_][A-Za-z0-9_]*"
                _struct_hits "${_line}")
            foreach(_hit IN LISTS _struct_hits)
                string(REGEX REPLACE "^.*,[ \t]*" "" _type "${_hit}")
                list(APPEND _declared "struct World::${_type}")
            endforeach()
            string(REGEX MATCHALL
                "WE_ENUM_SCHEMA[ \t]*\\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*[A-Za-z_][A-Za-z0-9_]*"
                _enum_hits "${_line}")
            foreach(_hit IN LISTS _enum_hits)
                string(REGEX REPLACE "^.*,[ \t]*" "" _type "${_hit}")
                list(APPEND _declared "enum World::${_type}")
            endforeach()
        endforeach()
    endforeach()
    if(_declared)
        list(REMOVE_DUPLICATES _declared)
        list(SORT _declared)
    endif()
    # 排序键用编译器自己的口径(短名):账本条目写 World::Foo 还是别的限定名都认得。
    set(_declared_keys "")
    foreach(_entry IN LISTS _declared)
        string(REGEX REPLACE "^([a-z]+) .*::(.*)$" "\\1 \\2" _key "${_entry}")
        list(APPEND _declared_keys "${_key}")
    endforeach()

    # ---- 2b. 读账本(不存在 = 首次构建,播种与旧配置期播种逐字节同形的头两行)----
    if(EXISTS "${_manifest}")
        file(READ "${_manifest}" _manifest_text)
    else()
        # 注意:这里必须是**一个**字符串(两行之间是真换行),不能写成两个参数 ——
        # CMake 的 set() 多参数会构成列表,换行之间会插一个 ';'(曾经把账本写成 "…\n;…")。
        set(_manifest_text "# Game 模块类型账本(双向清单):每行 \"struct <限定名>\" 或 \"enum <限定名>\",# 之后是注释。\n# 首次构建时由 Game/cmake/game-schema-build.cmake 从源码声明派生;此后由构建期同步与编辑器维护。\n")
    endif()

    # 逐行扫描:注释/空行先攒着,遇到条目行就把它和攒下的注释合成一段。
    # 第一个条目之前的注释 = 文件头注释(原样保留,不跟随任何条目)。
    set(_rest "${_manifest_text}")
    set(_head "")
    set(_tail "")
    set(_pending "")
    set(_segment_count 0)
    set(_no_trailing_newline FALSE)
    while(NOT _rest STREQUAL "")
        string(FIND "${_rest}" "\n" _newline)
        if(_newline EQUAL -1)
            set(_line "${_rest}")
            set(_rest "")
            set(_no_trailing_newline TRUE)
        else()
            string(SUBSTRING "${_rest}" 0 ${_newline} _line)
            math(EXPR _after_newline "${_newline} + 1")
            string(SUBSTRING "${_rest}" ${_after_newline} -1 _rest)
        endif()

        string(STRIP "${_line}" _stripped)
        if(_stripped STREQUAL "" OR _stripped MATCHES "^#")
            string(APPEND _pending "${_line}\n")
            continue()
        endif()
        # 条目行 = "struct|enum <限定名>",行尾允许 `# 注释`(schema-compiler 同口径)。
        set(_parse "${_stripped}")
        string(FIND "${_parse}" "#" _comment_at)
        if(NOT _comment_at EQUAL -1)
            string(SUBSTRING "${_parse}" 0 ${_comment_at} _parse)
        endif()
        string(STRIP "${_parse}" _parse)
        if(NOT _parse MATCHES "^(struct|enum)[ \t]+[A-Za-z_][A-Za-z0-9_:]*$")
            # 既不是注释也不是条目:原样保留(不猜)。
            string(APPEND _pending "${_line}\n")
            continue()
        endif()
        if(_segment_count EQUAL 0)
            set(_head "${_pending}")
            set(_pending "")
        endif()
        set(_segment_kind_${_segment_count} "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "^.*[ \t]+" "" _qualified "${_parse}")
        string(REGEX REPLACE "^.*::" "" _short "${_qualified}")
        set(_segment_key_${_segment_count} "${_segment_kind_${_segment_count}} ${_short}")
        set(_segment_text_${_segment_count} "${_pending}${_line}\n")
        set(_pending "")
        math(EXPR _segment_count "${_segment_count} + 1")
    endwhile()
    if(_segment_count EQUAL 0)
        # 账本里一个条目都没有(引擎骨架 / 刚建出的空项目):攒下的就是一个纯注释文件,
        # 整块算文件头 —— 新条目追加在它后面,而不是把注释挤到最后。
        set(_head "${_pending}")
        set(_pending "")
    endif()
    set(_tail "${_pending}")

    # 期望顺序 = 声明键规范化排序(与旧播种的 list(SORT "kind World::name") 同序)。
    set(_ordered_keys ${_declared_keys})
    if(_ordered_keys)
        list(SORT _ordered_keys)
        list(REMOVE_DUPLICATES _ordered_keys)
    endif()
    set(_last_segment -1)
    if(_segment_count GREATER 0)
        math(EXPR _last_segment "${_segment_count} - 1")
    endif()

    set(_body "")
    foreach(_key IN LISTS _ordered_keys)
        set(_matched FALSE)
        foreach(_index RANGE 0 ${_last_segment})
            if(_segment_key_${_index} STREQUAL _key)
                string(APPEND _body "${_segment_text_${_index}}")
                set(_matched TRUE)
            endif()
        endforeach()
        if(NOT _matched)
            string(REGEX REPLACE "^([a-z]+) (.*)$" "\\1 World::\\2" _new_entry "${_key}")
            string(APPEND _body "${_new_entry}\n")
        endif()
    endforeach()

    set(_new_text "${_head}${_body}${_tail}")
    if(_no_trailing_newline AND _new_text MATCHES "\n$")
        string(LENGTH "${_new_text}" _length)
        math(EXPR _length "${_length} - 1")
        string(SUBSTRING "${_new_text}" 0 ${_length} _new_text)
    endif()
    if(NOT _new_text STREQUAL _manifest_text)
        file(WRITE "${_manifest}" "${_new_text}")
        message(STATUS "Game schema: manifest synced with the source declarations -> ${_manifest}")
    endif()

    # ---- 3. 内容哈希增量门 -------------------------------------------------
    if(NOT EXISTS "${WLD_GAME_SCHEMA_COMPILER}")
        message(FATAL_ERROR "Game schema: schema-compiler not found: ${WLD_GAME_SCHEMA_COMPILER}")
    endif()
    file(SHA256 "${WLD_GAME_SCHEMA_COMPILER}" _compiler_hash)
    file(SHA256 "${_manifest}" _manifest_hash)
    set(_inputs_hash "")
    foreach(_input IN LISTS _inputs)
        file(RELATIVE_PATH _relative "${_src}" "${_input}")
        file(SHA256 "${_input}" _file_hash)
        string(APPEND _inputs_hash "${_relative}|${_file_hash}\n")
    endforeach()
    string(SHA256 _stamp_value
        "${WLD_GAME_SCHEMA_SCRIPT_VERSION}\n${_compiler_hash}\n${_manifest_hash}\n${_inputs_hash}")

    set(_stale TRUE)
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous_stamp)
        string(STRIP "${_previous_stamp}" _previous_stamp)
        if(_previous_stamp STREQUAL _stamp_value)
            set(_stale FALSE)
        endif()
    endif()
    if(NOT _stale)
        # 生成物被人删掉/挪走了就重建,不然"命中哈希"会掩盖缺失。
        foreach(_artifact "${WLD_GAME_SCHEMA_HEADER}" "${WLD_GAME_SCHEMA_SOURCE}"
                "${_gen}/Game/GameSchemaRegistration.h" "${_gen}/Game/GameSchemaRegistration.cpp")
            if(NOT EXISTS "${_artifact}")
                set(_stale TRUE)
            endif()
        endforeach()
    endif()
    if(NOT _stale)
        message(STATUS "Game schema: inputs unchanged; skipping schema-compiler (copy_if_different not needed)")
        return()
    endif()

    # ---- 4. 生成 + copy_if_different 回写 -----------------------------------
    set(_args "")
    foreach(_input IN LISTS _inputs)
        list(APPEND _args --input "${_input}")
    endforeach()
    foreach(_input IN LISTS _inputs)
        file(RELATIVE_PATH _relative "${_src}" "${_input}")
        file(TO_CMAKE_PATH "${_relative}" _relative)
        list(APPEND _args --reg-include "${_relative}")
    endforeach()

    file(MAKE_DIRECTORY "${_gen}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_manifest}" "${_gen}/Game.manifest"
        RESULT_VARIABLE _stage_result)
    if(NOT _stage_result EQUAL 0)
        message(FATAL_ERROR "Game schema: cannot stage the manifest into ${_gen}")
    endif()

    execute_process(
        COMMAND "${WLD_GAME_SCHEMA_COMPILER}" ${_args}
                --module Game --output "${_gen}" --manifest "${_gen}/Game.manifest"
        WORKING_DIRECTORY "${WLD_ENGINE_ROOT}"
        RESULT_VARIABLE _compile_result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(_stdout)
        string(STRIP "${_stdout}" _stdout_text)
        if(_stdout_text)
            message(STATUS "Game schema: ${_stdout_text}")
        endif()
    endif()
    if(NOT _compile_result EQUAL 0)
        message(FATAL_ERROR "Game schema: schema-compiler failed (exit ${_compile_result})\n"
            "${_stdout}${_stderr}")
    endif()

    get_filename_component(_output_dir "${WLD_GAME_SCHEMA_HEADER}" DIRECTORY)
    file(MAKE_DIRECTORY "${_output_dir}")
    foreach(_name "GameSchemaRegistration.h" "GameSchemaRegistration.cpp")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_gen}/Game/${_name}"
                    "${_output_dir}/${_name}"
            RESULT_VARIABLE _copy_result)
        if(NOT _copy_result EQUAL 0)
            message(FATAL_ERROR "Game schema: cannot write ${_output_dir}/${_name}")
        endif()
    endforeach()

    file(WRITE "${_stamp}" "${_stamp_value}\n")
    message(STATUS "Game schema: regenerated (inputs=${_inputs} -> ${_output_dir})")
endfunction()

wld_game_schema_run()
