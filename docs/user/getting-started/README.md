# 入门

目标:第一次把引擎跑起来、看到画面,并知道下一步去哪。

## 1. 准备与构建

需要 Windows + Visual Studio(含"使用 C++ 的桌面开发")、CMake、Vulkan SDK(配置阶段就要)。
完整的配置 / 构建 / 测试命令在[构建与运行](../../dev/build.md);最少先构建 `Editor` 目标。

## 2. 第一次启动编辑器

```powershell
# 工作目录 = 仓库根
.\build\x64-Debug\Editor\Debug\Editor.exe

# 想直接打开某个场景(路径按项目内容根解析)
.\build\x64-Debug\Editor\Debug\Editor.exe -scene projects/default/assets/scenes/3DTest.wd
```

- 不带参数时编辑器从**空场景**起步,不会自动打开示例场景。
- 仓库自带的示例场景在 `projects/default/assets/scenes/`:`2DTest.wd`、`3DTest.wd`、`LightingTest.wd`、`Physics3DTest.wd`;
  也可以在内容浏览器里双击 `.wd` 打开。
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
| 项目 | 一棵内容树 + 一份项目清单(`projects/default/project.we.yaml`:起始场景、渲染后端、物理参数、内容包) |
| 场景 | `.wd` 文件:实体树 + 每个实体挂的组件 |
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
