-- W3e headless fixture: camera fields live behind a nested Object field
-- (CameraComponent.Camera -> SceneCamera). The proxy is read/written through the
-- same schema-driven field proxy as the component root; every access re-checks the
-- entity/component/type chain. Modes are selected by the C++ harness through the
-- TEST_CameraProbeMode global (shared VM), phases through TEST_LuaPhase.
-- 字段名与 schema 一致(SceneCamera 的私有成员带 m_ 前缀,与 PropertiesPanel/存根同名)。
---@class CameraProbe : WorldScript
local CameraProbe = {}

function CameraProbe:OnCreate()
    local phase = TEST_LuaPhase
    if phase ~= "create" then
        return
    end
    local entity = self.entity
    assert(entity:IsValid())
    local component = entity:GetComponent("CameraComponent")
    assert(component ~= nil)
    local camera = component.Camera
    assert(typeof(camera) == "ComponentProxy")
    assert(tostring(camera) == "ComponentProxy(CameraComponent.Camera)")
    assert(camera.m_ProjectionType == 1)
    assert(camera.m_OrthographicZoom == 1.0)
    assert(camera.m_OrthographicNearClip == -1.0)
    assert(camera.m_OrthographicFarClip == 1.0)
    -- enum 字段按既有约定可写数值
    camera.m_ProjectionType = 0
    assert(camera.m_ProjectionType == 0)
    -- 枚举名写入路径（与数值写同一套转换）
    camera.m_ProjectionType = "Orthographic"
    assert(camera.m_ProjectionType == 1)
    TEST_ReportCameraProbe("create-ok")
end

function CameraProbe:OnUpdate(dt)
    local mode = TEST_CameraProbeMode
    local entity = self.entity
    if mode == "write" then
        local camera = entity:GetComponent("CameraComponent").Camera
        camera.m_PerspectiveFOV = 60.0
        camera.m_PerspectiveNearClip = 0.25
        camera.m_PerspectiveFarClip = 250.0
        camera.m_OrthographicZoom = 2.5
        camera.m_OrthographicNearClip = -2.0
        camera.m_OrthographicFarClip = 2.0
        camera.m_ProjectionType = "Perspective"
        assert(camera.m_PerspectiveFOV == 60.0)
        assert(camera.m_ProjectionType == 0)
    elseif mode == "write_transient" then
        local camera = entity:GetComponent("CameraComponent").Camera
        local before = camera.m_AspectRatio
        assert(before > 0.0)
        local ok = pcall(function() camera.m_AspectRatio = 2.0 end)
        assert(not ok, "Transient nested field must be rejected for scripts")
        assert(camera.m_AspectRatio == before)
    elseif mode == "write_object" then
        local ok, err = pcall(function() entity:GetComponent("CameraComponent").Camera = 1 end)
        assert(not ok, "nested object field must be read-only for scripts")
        assert(type(err) == "string")
        assert(string.find(err, "read-only", 1, true), err)
    elseif mode == "bad_field" then
        local camera = entity:GetComponent("CameraComponent").Camera
        local ok, err = pcall(function() return camera.m_ProjectionTypo end)
        assert(not ok, "unknown nested field must raise")
        assert(string.find(err, "no field 'm_ProjectionTypo'", 1, true), err)
    end
end

return CameraProbe
