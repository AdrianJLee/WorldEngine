---WorldEngineAPI

---@class mat4
---@field inverse fun():mat4 Get the inverse of the matrix
---@field transpose fun():mat4 Get the transpose of the matrix
---@field determinant fun():number Get the determinant of the matrix
mat4 = {}

---@class mat3
---@field inverse fun():mat3 Get the inverse of the matrix
---@field transpose fun():mat3 Get the transpose of the matrix
---@field determinant fun():number Get the determinant of the matrix
mat3 = {}

---@class vec2
---@field x number
---@field y number
---@field length fun():number Get the length of the vector
vec2 = {}

---@class vec3
---@field x number
---@field y number
---@field z number
---@field length fun():number Get the length of the vector
---@field normalize fun():vec3 Normalize the vector
---@field dot fun(vec3):number Calculate the dot product with another vector
---@field cross fun(vec3):vec3 Calculate the cross product with another vector
vec3 = {}

---@class vec4
---@field x number
---@field y number
---@field z number
---@field w number
---@field length fun():number Get the length of the vector
vec4 = {}

---@class SpriteComponent
---@field Color vec4
---@field Texture any
---@field TilingFactor number
SpriteComponent = {}

---@class Camera
---@field m_ProjectionMatrix mat4
Camera = {}

---@class UUID
---@field m_UUID number
UUID = {}

---@class ProjectionType
---@field Perspective number
---@field Orthographic number
ProjectionType = {}

---@class SceneCamera
---@field m_ProjectionType number
---@field m_AspectRatio number
---@field m_OrthographicZoom number
---@field m_OrthographicNearClip number
---@field m_OrthographicFarClip number
---@field m_PerspectiveFOV number
---@field m_PerspectiveNearClip number
---@field m_PerspectiveFarClip number
SceneCamera = {}

---@class CircleRendererComponent
---@field Color vec4
---@field Thickness number
---@field Fade number
CircleRendererComponent = {}

---@class UUIDComponent
---@field ID UUID
UUIDComponent = {}

---@class TagComponent
---@field Tag string
TagComponent = {}

---@class NativeScriptComponent
---@field ScriptName string
NativeScriptComponent = {}

---@class TransformComponent
---@field Location vec3
---@field Rotation vec3
---@field RotationQuat any
---@field Scale vec3
---@field Transform mat4
TransformComponent = {}

---@class BoxCollider2DComponent
---@field Offset vec2
---@field Size vec2
---@field Density number
---@field Friction number
---@field Restitution number
---@field ShowCollider boolean
BoxCollider2DComponent = {}

---@class CameraComponent
---@field Camera SceneCamera
---@field Primary boolean
---@field FixedAspectRatio boolean
CameraComponent = {}

---@class StressTestType
---@field None number
---@field Test1 number
---@field Test2 number
StressTestType = {}

---@class LuaScriptComponent
---@field ScriptFilePath string
LuaScriptComponent = {}

---@class BodyType
---@field Static number
---@field Dynamic number
---@field Kinematic number
BodyType = {}

---@class RigidBody2DComponent
---@field Type number
---@field FixedRotation boolean
RigidBody2DComponent = {}

---@class CircleCollider2DComponent
---@field Offset vec2
---@field Radius number
---@field Density number
---@field Friction number
---@field Restitution number
---@field ShowCollider boolean
CircleCollider2DComponent = {}

---@class OpenGLTexture2D
---@field m_Path string
OpenGLTexture2D = {}

---@class StressTest
---@field Weight number
---@field Height number
---@field m_Type number
StressTest = {}

---@class ExampleScript
ExampleScript = {}

