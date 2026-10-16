# Studio Decisions & Role Activation Log (决策与职能开关台账)

## 1. 立项与流程基准决策
- **决策人**: `producer` (L1 AI 制作人) / `publisher_l0` (L0 出品人)
- **预设基准**: `default` (8 阶段工业级研发流程)
- **协作模式**: `managed` (托管模式：L1 制作人主导拆解与调度，关键门禁呈报 L0 出品人)
- **伦理底线**: `forbid_dark_patterns: true` (强制开启反黑暗模式伦理审查)

## 2. 职能动态唤醒与休眠决断 (D13)
- **常驻激活 (ACTIVE)**: `producer`, `creative_director`, `art_director`, `experience_director`, `technical_director`, `qa_lead`, `automation_qa`, `system_designer`, `gameplay_programmer`
- **按阶段动态唤醒 (DORMANT)**: `level_designer`, `combat_designer`, `2d_artist`, `3d_environment_artist`, `audio_designer`, `perf_compat_qa`
- **裁撤屏蔽 (DISABLED)**: 非当前项目品类所需的专有重资产职能不加载会话与 MCP 工具清单，保持零 Token 损耗。
