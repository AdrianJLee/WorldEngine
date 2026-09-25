// pattern.slang — WorldEngine 材质函数库(示例)
// ===========================================================================
// **材质函数怎么写**:一个库文件里只放**纯函数** —— 没有 `Evaluate` 入口、不写 `//! param`
// 注解、不声明自己的贴图/采样器/绑定。要用贴图就把 `Sampler2D` 作为**形参**传进来
// (引擎按材质自己的注解生成槽位)。
//
// 规则(与 docs/dev/shader-contract.md「材质函数」一节同一口径):
//   1. **纯函数**:输入 → 输出;不读全局状态、不碰材质的 `input` / `surface`(它们属于材质)。
//   2. **资源由材质传入**:函数体内不出现 `Texture2D` / `Sampler2D` 变量声明;需要采样时
//      形参写成 `Sampler2D map`,材质侧调用 `f(albedoMap, uv)`。函数本身不占槽位。
//   3. **命名与版本**:共享库建议加 2–4 字母前缀(如 `WeChecker`),避免与材质自己的辅助
//      函数/引擎包装符号重名(同一个翻译单元);破坏性改动**加新函数**(`CheckerPatternV2`)
//      而不是改签名 —— 材质各自 include,改签名会一次打断所有调用点。
//   4. **改库 = 重烘**:表面着色器缓存键含被包含文件的**路径 + 内容哈希**,本文件改一个
//      字符,依赖它的材质就重新编译(发布包里的产物同样跟着变)。
//   5. **参数带默认值**:调用点可以少写末尾参数;Slang 不做隐式宽度转换,默认值类型要写对
//      (`float2(0.5, 0.5)` 而不是 `0.5` 塞进 `float2`)。
//
// 最小用法(材质侧,见 shaders/examples/ShowcaseMaterial.slang):
//   #include "lib/pattern.slang"
//   Surface Evaluate(MaterialInputs input)
//   {
//       Surface surface = MakeDefaultSurface();
//       surface.BaseColor = CheckerBlend(surface.BaseColor, float3(0.1, 0.1, 0.12), input.UV, 8.0);
//       return surface;
//   }
// ===========================================================================

// 把 baseColor 朝 tint 方向偏 amount(0 = 原色,1 = 纯 tint)。返回值可以继续喂给其它函数。
// 用法:`float3 c = ApplyTint(albedo, tint.rgb, 0.5);`
float3 ApplyTint(float3 baseColor, float3 tint, float amount = 1.0)
{
    return lerp(baseColor, baseColor * tint, saturate(amount));
}

// 棋盘图案:uv 每 scale 格翻转一次,返回 0 或 1(纯标量,方便自己再做颜色/强度混合)。
// 用 abs() 保证负 UV 也交替(Slang 的 fmod 对负数返回负值)。
// 用法:`float checker = CheckerPattern(input.UV, 12.0);`
float CheckerPattern(float2 uv, float scale = 8.0)
{
    float2 cell = floor(uv * scale);
    float parity = fmod(abs(cell.x + cell.y), 2.0);
    return step(0.5, parity);
}

// 棋盘两色交替:组合 CheckerPattern,一次调用就把图案混进颜色(不引入任何绑定)。
// 用法:`surface.BaseColor = CheckerBlend(baseColor, patternColor, uv, 16.0);`
float3 CheckerBlend(float3 colorA, float3 colorB, float2 uv, float scale = 8.0)
{
    return lerp(colorA, colorB, CheckerPattern(uv, scale));
}

// 径向遮罩:以 center 为中心,radius 内为 1,到 radius + softness 渐隐到 0(只用 uv,不需要相机)。
// 用法:`float glow = RadialMask(input.UV, float2(0.5, 0.5), 0.6);` → 乘到 Emissive 上 = 中心发光。
float RadialMask(float2 uv, float2 center = float2(0.5, 0.5), float radius = 0.5,
    float softness = 0.25)
{
    float radialDistance = distance(uv, center);
    return saturate(1.0 - smoothstep(radius, radius + max(softness, 0.001), radialDistance));
}
