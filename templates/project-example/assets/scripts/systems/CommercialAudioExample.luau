-- =============================================================================
-- 商用级音频系统综合演示与玩法接入示例 (CommercialAudioExample.luau)
-- =============================================================================
-- 本系统完整演示 WorldEngine 音频系统三大里程碑特性闭环:
--
-- 【Milestone M1: 核心底座与通用闭环】
--   1. Audio:PostEvent(eventId, [x, y, z, volume, pitch, loop]) -> 3D/全局声效触发
--   2. Audio:SetBusVolume(busId, volume) -> 总线分级混音控制 (Master=0, Music=1, SFX=2, Voice=3, UI=4, Ambience=5)
--   3. Audio:TriggerSnapshot(name, fadeSec) / ClearSnapshot -> 情境混音快照平滑过渡
--   4. Music:Play(trackId, fadeSec) / Music:TransitionTo(trackId, sync, fadeSec) -> 基于硬件采样时钟的交互式音乐无缝淡入淡出
--   5. ECS AudioSourceComponent (PlayOnAwake, Spatial3D, 距离衰减) 与 AudioListenerComponent 自动同步
--
-- 【Milestone M2: 2D 游戏音频完备包】
--   1. Audio:PostEvent2D(eventId, screenX, screenY, [volume, pitch, loop]) -> 2D 屏幕空间等功率左右声像 (Constant-Power Pan) 与 XY 径向衰减 (忽略 Z 深度)
--   2. Audio:PostCombo(eventId, comboGroup, [screenX, screenY, volume]) -> 连击音高递进器 (Combo Pitch Escalator): 每次命中自动提升半音, 超时重置
--   3. 割草/弹幕同帧防爆音 (Rapid-Fire Throttling) 与微相位随机抖动 (Phase Jitter 0~5ms)
--   4. 经典复古像素风 Bitcrusher DSP 效果器 (位深量化 2~16bit + 采样率降频保持)
--
-- 【Milestone M3: 3D 次世代空间声学与物理联动包】
--   1. 双耳 HRTF 空间化: 身前/身后耳廓遮挡高频衰减 (~6.5kHz 低通, -4dB) 与三维上下仰角高度听声辨位
--   2. Audio:SetVoiceVolumetricRadius(handle, radius) -> 近场立体声体积展宽 (Volumetric Spread): 贴脸时声音平滑展开包裹双耳
--   3. Audio:SetVoiceVelocity(handle, vx, vy, vz) -> 视线投影物理相对速度多普勒频移 (Doppler Pitch Shift)
--   4. Audio:SetVoiceOcclusion(handle, occlusion) -> 动态几何射线物理遮挡与距离空气高频吸收 (Air Absorption)
--   5. Audio:GetDspTime() -> 基于硬件混音累计帧数的绝对采样时钟 (毫秒级防漂移)
--   6. 刚体物理碰撞冲量映射发声与滑动摩擦声系统
-- =============================================================================

-- 关卡归属守卫: 本演示只在含 "Audio Demo Stage" 标记的关卡里跑。
-- 没有守卫时脚步声循环会在每个关卡里一直响(用户听到的"滋滋声")。
local SceneScope = ecs:RequireLib("scene_scope")
local inScope = SceneScope.owns("Audio Demo Stage")

local initialized = false
local comboTimer = 0.0
local simulatedEnemyPos = { x = -8.0, y = 2.0, z = 5.0 }
local simulatedEnemyVel = { vx = 15.0, vy = 0.0, vz = -5.0 } -- 快速飞驰接近的敌人
local activeSfxVoice = 0
local footstepTimer = 0.0

ecs:AddSystem("CommercialAudioDemoSystem", "Update", function(dt: number)
    if not inScope then
        return
    end
    if not initialized then
        initialized = true
        print("==================================================================")
        print("[AUDIO-DEMO] 商业级音频系统演示系统启动 (M1 + M2 + M3 全特性接入)")
        print("==================================================================")

        -- -------------------------------------------------------------
        -- M1: 通用底座控制 (总线音量、快照、交互音乐走带)
        -- -------------------------------------------------------------
        local kBusMaster = 0
        local kBusMusic = 1
        local kBusSfx = 2

        Audio:SetBusVolume(kBusMaster, 0.9)
        Audio:SetBusVolume(kBusMusic, 0.75)
        Audio:SetBusVolume(kBusSfx, 1.0)
        print("[AUDIO-DEMO] M1: 已初始化总线增益 (Master: 90% | Music: 75% | SFX: 100%)")

        -- 触发全局 UI 确认声
        local uiVoice = Audio:PostEvent("ui_click", 0, 0, 0, 1.0, 1.0, false)
        print(string.format("[AUDIO-DEMO] M1: 触发 UI 点击事件, 分配 VoiceHandle = %d", uiVoice))

        -- 启动基于采样时钟的交互式背景音乐 (带 1.0 秒交叉淡入)
        Music:Play("theme_explore", 1.0)
        print("[AUDIO-DEMO] M1: 交互音乐驱动 -> 播放 'theme_explore', 采样时钟启动")

        -- -------------------------------------------------------------
        -- M2: 2D 屏幕空间声像与连击音高递进演示
        -- -------------------------------------------------------------
        -- 场景一: 屏幕左侧发生爆炸 (X = -6.5 米, Y = 0.0 米)
        local explVoice = Audio:PostEvent2D("footstep_player", -6.5, 0.0, 1.0, 0.8, false)
        print(string.format("[AUDIO-DEMO] M2: 触发 2D 屏幕声源 (ScreenX=-6.5m) -> 等功率左偏声像, VoiceHandle = %d", explVoice))

        -- 场景二: 模拟连续拾取金币触发连击音高步进 (Combo Pitch Escalator)
        for i = 1, 4 do
            local coinVoice = Audio:PostCombo("ui_click", "coin_chain", -2.0 + i * 1.0, 1.0, 1.0)
            print(string.format("[AUDIO-DEMO] M2: 连击拾取金币 [%d/4] -> 音高自动抬升 +%.1f 半音, Voice = %d", i, (i - 1) * 1.0, coinVoice))
        end

        -- -------------------------------------------------------------
        -- M3: 3D 次世代空间声学 (近场展宽、多普勒频移、遮挡与 DSP 时钟)
        -- -------------------------------------------------------------
        -- 场景一: 在 3D 空间中生成一个飞速掠过的声源。
        -- 刻意用一次性播放(loop=false)并由帧更新按脚步节奏重发: 循环播放一段
        -- 脚步采样会变成持续底噪(用户反馈的"滋滋声"), 真正的脚步应当有间隔。
        activeSfxVoice = Audio:PostEvent("footstep_player", simulatedEnemyPos.x, simulatedEnemyPos.y, simulatedEnemyPos.z, 1.0, 1.0, false)
        if activeSfxVoice > 0 then
            -- 启用近场立体声展宽 (Volumetric Radius = 6.0 米): 当靠近时平滑从单点展开为双耳包裹
            Audio:SetVoiceVolumetricRadius(activeSfxVoice, 6.0)

            -- 注入物理速度矢量 (驱动视线投影多普勒频移)
            Audio:SetVoiceVelocity(activeSfxVoice, simulatedEnemyVel.vx, simulatedEnemyVel.vy, simulatedEnemyVel.vz)

            -- 模拟穿墙声学遮挡: 设置 40% 物理障碍遮挡 (驱动低通滤波截止频率与直达声衰减)
            Audio:SetVoiceOcclusion(activeSfxVoice, 0.4)

            print(string.format("[AUDIO-DEMO] M3: 3D 次世代声源就绪 (Voice=%d): 体积展宽=6m | 多普勒速度=(%.1f, %.1f) | 物理遮挡=40%%",
                activeSfxVoice, simulatedEnemyVel.vx, simulatedEnemyVel.vz))
        end

        -- 读取亚毫秒级硬件混音 DSP 绝对时钟
        local dspSeconds = Audio:GetDspTime()
        print(string.format("[AUDIO-DEMO] M3: 硬件混音绝对 DSP 时间戳 = %.4f 秒 (硬件采样计数驱动, 0 帧率漂移)", dspSeconds))
    end

    -- 运行时帧更新: 模拟 3D 移动与连击超时监测
    comboTimer = comboTimer + dt
    -- 按脚步节奏(约 0.55s 一步)重发一次性脚步, 而不是循环长鸣。
    footstepTimer = footstepTimer - dt
    if footstepTimer <= 0.0 then
        footstepTimer = 0.55
        simulatedEnemyPos.x = simulatedEnemyPos.x + simulatedEnemyVel.vx * 0.55
        simulatedEnemyPos.z = simulatedEnemyPos.z + simulatedEnemyVel.vz * 0.55
        if math.abs(simulatedEnemyPos.x) > 14.0 then
            simulatedEnemyPos.x = -simulatedEnemyPos.x * 0.5
        end
        activeSfxVoice = Audio:PostEvent("footstep_player", simulatedEnemyPos.x, simulatedEnemyPos.y, simulatedEnemyPos.z, 1.0, 1.0, false)
        if activeSfxVoice > 0 then
            Audio:SetVoiceVolumetricRadius(activeSfxVoice, 6.0)
            Audio:SetVoiceVelocity(activeSfxVoice, simulatedEnemyVel.vx, simulatedEnemyVel.vy, simulatedEnemyVel.vz)
            Audio:SetVoiceOcclusion(activeSfxVoice, 0.4)
        end
    end
    if comboTimer > 3.0 and activeSfxVoice > 0 then
        comboTimer = 0.0
        -- 模拟敌人已飞掠而过并远离: 切换为远离速度
        simulatedEnemyVel.vx = -10.0
        Audio:SetVoiceVelocity(activeSfxVoice, simulatedEnemyVel.vx, simulatedEnemyVel.vy, simulatedEnemyVel.vz)
        -- 平滑过渡到洞穴混响快照
        Audio:TriggerSnapshot("Snapshot_CaveReverb", 0.8)
    end
end)

-- 退出本关时收尾: 停掉仍在循环的演示声与背景音乐, 不把声音带进下一个关卡。
ecs:AddTeardownSystem("CommercialAudioDemoTeardown", function()
    if not inScope then
        return
    end
    if activeSfxVoice > 0 then
        Audio:Stop(activeSfxVoice, 0.15)
        activeSfxVoice = 0
    end
    Music:Stop(0.2)
end)
