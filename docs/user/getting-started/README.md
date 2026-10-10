# 入门

目标:第一次把引擎跑起来、看到画面,并知道下一步去哪。

## 1. 准备与构建

需要 Windows + Visual Studio(含"使用 C++ 的桌面开发")、CMake、Vulkan SDK(配置阶段就要)。
完整的配置 / 构建 / 测试命令在[构建与运行](../../dev/build.md);最少先构建 `Editor` 目标。

## 2. 第一次启动编辑器

```powershell
# 工作目录 = 仓库根
.\build\x64-Debug\Editor\Debug\Editor.exe

# 想直接打开某个关卡(路径按**项目内容根**解析;把 <项目根> 换成你的项目目录)
.\build\x64-Debug\Editor\Debug\Editor.exe --project <项目根> -scene assets/scenes/3DTest.wlevel
```

- 不带参数时编辑器进**项目启动器**(不挂载任何项目);建项目用 `文件 ▸ 新建项目…`(可选"示例"模板)。
- 示例关卡(`2DTest`、`3DTest`、`LightingTest`、`Physics3DTest`)在示例模板
  `templates/project-example/assets/scenes/` 里:`scenes/<名字>.wlevel` 是可双击打开的**入口文件**,
  内容按一实体一文件放在 `assets/levels/<名字>/entities/` 下。用示例模板建项目后即可在内容浏览器里双击打开。
- 编辑器启动时恢复上次的面板布局;要回到默认布局,用 Window 菜单里的 `Reset Layout`。

## 3. 看到画面之后

| 想做的事 | 去哪 |
| --- | --- |
| 转视角 / 选实体 | 视口(见[编辑器](../editor/README.md)) |
| 改实体与组件 | 属性面板 |
| 打开场景 / 材质 / 脚本 / 模型 | 内容浏览器(双击) |
| 试运行 | 视口上方的运行控制(Play / Simulate / Pause) |
| 改项目设置(渲染、物理) | `Project Settings` 面板 |

## 术语速览

| 词 | 含义 |
| --- | --- |
| 项目 | 一棵内容树 + 一份项目清单(`<项目根>/project.we.yaml`:起始场景、渲染后端、物理参数、内容包) |
| 关卡 | `scenes/<Id>.wlevel`(入口)+ `levels/<Id>/entities/*.entity`(一实体一文件) |
| 实体 | 场景里的一条对象,自带 Transform,可挂组件 |
| 组件 | 挂在实体上的数据或行为:Transform、摄像机、灯光、网格、碰撞体、脚本…… |
| 资产 | 可被场景引用的文件:场景、模型、材质、纹理、预制体 |
| 内容根 | 项目清单里 `content_root` 指向的目录(当前是 `assets`);资产路径都相对于它 |

## 下一步

- [编辑器](../editor/README.md) —— 面板与常用操作
- [资产](../assets/README.md) —— 模型 / 材质 / 预制体
- [脚本](../scripting/README.md) —— 给实体挂脚本
- [打包](../packaging/README.md) —— 打成可分发的产物
- 遇到问题:[常见问题](../troubleshooting/README.md)
