# we-gen (WorldEngine Gen)

WorldEngine A+C 混合资产生成管线插件（Hybrid Asset Generation Pipeline, Windows DPAPI & StyleLock）。

本插件作为方案 [0001-ai-native-workflow](E:/WorldEngine-docs/knowledge/decisions/0001-ai-native-workflow.md) 的资产生成核心（方案 A，D15/D16），支持托管云端与本地生成模型、Windows 原生 DPAPI 凭据安全存储、StyleLock 风格防漂移锁、成本预算守卫，以及资产来源侧车审计（Provenance Sidecar）。

---

## 1. 核心特性

- **Windows DPAPI 凭据安全托管（Zero-Leak Secret Storage）**：
  所有外部 API 密钥（OpenAI、Anthropic、Stability、Midjourney 等）均通过 Windows 原生数据保护 API（`CryptProtectData` / `CryptUnprotectData`）在操作系统层面进行强加密，存储在 `local/credentials/*.bin`。**绝不进入任何项目文件、代码仓库或 Git 记录**。
- **StyleLock 美术风格防漂移锁**：
  通过项目美术配置 `art/style-lock.yaml`，锁定全局或品类专属的正向提示词增强模板（Positive Prompt Template）、全局负面提示词（Negative Prompts）、推荐采样参数与固定风格 Seed，彻底杜绝 AI 批量出图时风格漂移断代的问题。
- **BudgetGuard 成本预算守卫**：
  提供单次资产生成上限与月度总预算硬性门禁，一旦超过设定阈值直接拒绝调用，杜绝突发账单暴涨。
- **`.gen.yaml` 来源侧车审计（Provenance Sidecar）**：
  任何通过管线生成的图像、模型或音频资产，均会在同级目录自动伴生生成 `<asset_name>.<ext>.gen.yaml` 侧车文件，记录模型名称、生成 Prompt、随机种子 Seed、时间戳、操作角色与费用，实现资产来源 100% 可回溯与版规审计。

---

## 2. 核心架构与工作流程

```
AI 角色请求生成 ─▶ BudgetGuard 检查预算 ─▶ StyleLock 注入风格提示词 ─▶ CredentialManager 解密 DPAPI 凭据
                                                                          │
资产落盘 ◀─── 伴生生成 <asset>.gen.yaml 来源侧车 ◀─── 调用模型提供商 API ◀┘
```

---

## 3. 程序化与 MCP 命令接口

`we-gen` 注册了以下 5 条核心命令：

### 3.1 `gen.generate` (触发资产生成)
- **Effect**: `Write`
- **参数**:
  - `prompt` (string, 必需): 资产生成描述；
  - `type` (string, 必需): 资产类型（`texture`, `model`, `audio`, `sprite` 等）；
  - `output` (string, 必需): 目标输出相对路径（如 `assets/textures/hero_run.png`）；
  - `cost` (number, 可选): 预计消耗美元，用于记账与预算核减（缺省 `0.02`）。
- **调用示例**:
  ```json
  {
    "command": "gen.generate",
    "args": {
      "prompt": "16-bit retro pixel art knight running cycle, side view",
      "type": "texture",
      "output": "assets/textures/knight_run.png",
      "cost": 0.03
    }
  }
  ```
- **生成产物**:
  1. `assets/textures/knight_run.png`（资产本体）；
  2. `assets/textures/knight_run.png.gen.yaml`（来源审计侧车）：
     ```yaml
     source_model: "mock-gen-provider"
     timestamp: 1729000000
     prompt: "16-bit retro pixel art knight running cycle, side view"
     style_locked: true
     cost_usd: 0.03
     ```

### 3.2 `gen.style_lock.get` (读取风格锁配置)
- **Effect**: `Read`
- **参数**:
  - `root` (string, 可选): 项目根路径。
- **调用示例**:
  ```json
  {
    "command": "gen.style_lock.get",
    "args": {}
  }
  ```

### 3.3 `gen.style_lock.set` (配置风格锁)
- **Effect**: `Write`
- **参数**:
  - `positive_template` (string): 正向风格增强模板；
  - `negative_prompt` (string): 负面排除词；
  - `style_name` (string): 风格名称（如 `16bit_pixel`）。
- **调用示例**:
  ```json
  {
    "command": "gen.style_lock.set",
    "args": {
      "style_name": "retro_pixel",
      "positive_template": "{prompt}, 16-bit pixel art style, low saturation, sharp edge, game asset sprite",
      "negative_prompt": "blurry, 3d render, photo, photorealistic, modern, high poly, smooth"
    }
  }
  ```

### 3.4 `gen.budget.get` 与 `gen.budget.set` (预算管理)
- **Effect**: `Read` / `Write`
- **参数**:
  - `monthly_limit` (number): 月度总预算（美元）；
  - `per_asset_limit` (number): 单次资产生成上限（美元）。
- **调用示例**:
  ```json
  {
    "command": "gen.budget.set",
    "args": {
      "monthly_limit": 50.0,
      "per_asset_limit": 0.50
    }
  }
  ```

---

## 4. 源码结构

- `src/CredentialManager.h` / `src/CredentialManager.cpp`：Windows DPAPI 强加密接口，密钥存储与内存解密；
- `src/StyleLock.h` / `src/StyleLock.cpp`：`art/style-lock.yaml` 解析、模板合成与负面提示词注入；
- `src/BudgetGuard.h` / `src/BudgetGuard.cpp`：单次与累计花费阈值核算、预算超限阻断；
- `src/GenPipeline.h` / `src/GenPipeline.cpp`：资产生成流水线调度与 `.gen.yaml` 侧车伴生写入；
- `src/GenPlugin.cpp`：插件 ABI 注册入口。
