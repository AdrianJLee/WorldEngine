# 性能分析与内存分析(Profiling)

> 面向两类读者,同一份事实:
> **人**——读本文与编辑器面板;**AI/脚本**——读同一张数据字典与 `prof.*` 命令返回的 JSON。
> 字段语义的**单一事实源**是 `Engine/src/World/Profiling/TraceEvents.h`;本文是它的镜像表,
> 三处(代码 / 本文 / 知识库 `contract.telemetry-schema`)必须一致 —— 加字段先补表,再改代码。

## 1. 两条通道(代价不同,开关不同)

| 通道 | 何时开 | 代价 | 回答什么 |
| --- | --- | --- | --- |
| **帧统计** | **常开** | 每帧 2 次取时 + 1 次直方图落桶,与作用域数无关 | 这一帧慢不慢、稳不稳(p50/p95/p99、掉帧数、FPS) |
| **作用域追踪** | **按需**(采集窗口) | 每作用域一条事件写进预分配缓冲 | 慢在哪(层级作用域树/火焰图) |

设计前提:采集热路径**零堆分配、零锁**;关闸时的代价只有一次 relaxed 原子读 + 分支;
发行版可用 `WLD_TELEMETRY_ENABLED=0` 整体剥离。

## 2. 怎么用

### 2.1 无头 / CI(主通道,环境变量)

```powershell
$env:WLD_TELEMETRY_CAPTURE = "300"                  # 采集 300 帧
$env:WLD_TELEMETRY_OUT     = "tmp/trace.json"       # 输出路径(可省略,默认 telemetry/trace-<ts>.json)
$env:WLD_TELEMETRY_BACKEND = "opengl"               # 可选:写进 trace 元数据
Start-Process -FilePath build/x64-Debug/Editor/Debug/Editor.exe -WindowStyle Hidden
```

采集满 `Frames` 帧后**自动落盘**(落盘发生在采集结束之后,不在帧循环内)。

### 2.2 编辑器里(人用)

`profiler` 面板显示帧时间曲线、分位数、掉帧计数与采集控制;`memory`/`stats` 面板读同一份
快照(同一指标在面板 / AI 命令 / 导出文件 / 无障碍节点四处**必须来自同一个结构体**,不允许各自计算)。

### 2.3 AI / 脚本

```
prof.stats          → 帧时间分位数 / 掉帧 / 采集状态(结构化 JSON,键名见下表)
prof.capture.start  → {"frames": "N", "path": "..."} 开始采集
prof.capture.stop   → 提前结束并落盘
prof.memory         → 内存标签表 + 显存驻留(M2 起有数据)
```

`ui.tree` 的无障碍节点暴露同一组键值,无鼠标脚本可直接断言。

## 3. 数据字典

`-1` 一律表示**未采集**(不是 0):0 可能是"真的是 0",两者必须能区分。

### 3.1 帧统计 `frameMs`(这是 **WUI 快照**的键,单位毫秒)

| 键 | 单位 | 范围 | 缺失含义 |
| --- | --- | --- | --- |
| `last` | ms | ≥0 | — |
| `p50` / `p95` / `p99` | ms | ≥0 | `-1` = 样本不足 |
| `max` | ms | ≥0 | 本次运行最大帧 |
| `samples` | 帧 | ≥0 | 累计帧数 |
| `window` | 帧 | ≤3600 | 分位数统计窗口内的样本数 |
| `jank` | 帧 | ≥0 | 超过 `2 × p50` 的帧数 |
| `jankThresholdMs` | ms | ≥0 | `-1` = 判据尚未建立 |
| `overflowed` | bool | — | true = 有样本落在 >128ms 的溢出桶 |

分位数由**固定直方图**给出(分辨率 0.25ms,覆盖 0..128ms),是**量化估计值**,
查询时被 `max` 夹住;不要把它当精确值用于逐帧断言。

### 3.1b trace 里的计数器(注意**微秒**)

导出文件里的计数器用**微秒**,名字就写单位(避免"叫 Ms 存 Us"这类 1000 倍错误):

| 计数器 | 单位 | 含义 |
| --- | --- | --- |
| `frameUs` | 微秒 | 本帧时长(等价于 `frameMs` × 1000) |
| `Scene.CullUs` | 微秒 | 3D 视锥剔除耗时(cull 常 <1ms,故用微秒) |
| `Scene.ShadowCasters` | 个 | 阴影通道提交数(阴影关闭时为 0) |
| `systems.executed` | 个 | 本相位实际执行的帧系统数 |

`frameMs`(毫秒)是**快照/AI 查询**的键;`frameUs`(微秒)是**trace 文件**的键 ——
两者同名不同域会让人混淆,所以刻意分开命名。

### 3.2 采集 `capture`

| 键 | 含义 |
| --- | --- |
| `active` | 是否正在采集 |
| `hasResult` | 上一次采集是否产出了文件 |
| `framesRequested` / `framesCaptured` | 请求帧数 / 实际帧数 |
| `events` | 写入的事件数 |
| `droppedEvents` | **>0 ⇒ 本次采集不可用于定量结论**(缓冲溢出,数据不完整) |
| `path` / `error` | 输出路径 / 失败原因 |

### 3.3 内存 `mem`(M2 起有数据)

| 键 | 单位 | 缺失含义 |
| --- | --- | --- |
| `collected` | bool | false ⇒ 下表全部为 `-1` |
| `cpuTotal` / `cpuPeak` | 字节 | `-1` = 未采集 |
| `gpuResident` | 字节 | `-1` = 后端未登记 |

## 4. 事件 schema(导出文件)

`schemaVersion` 写在文件头(当前 **2**;v1→v2 把计数器 `frameMs`→`frameUs`、`Scene.CullMs`→`Scene.CullUs`,名字与单位对齐);`TraceEvent` 是 40 字节定长 POD(编译期断言尺寸与可平凡拷贝)。

| 类型 | 语义 |
| --- | --- |
| `FrameBegin` / `FrameEnd` | 帧边界。**没有它,trace 无法按帧切片** |
| `ScopeBegin` / `ScopeEnd` | 层级作用域;导出时配对成 `dur` |
| `Counter` | 计数器采样(`Value`) |

导出格式是 **Chrome Trace Event JSON**:Perfetto / `chrome://tracing` 直接打开。
文件头 `otherData` 带 `schemaVersion` / `droppedEvents` / `eventCount`;元数据含进程名与线程名。

**名字的生命周期契约**:事件只存 `const char*`,必须指向静态字面量,或生命周期覆盖整个采集
窗口的稳定字符串(例如 `SystemRegistry` 用稳定存储保存的系统名)。**不要传局部变量/临时串**。

## 5. 怎么验证(反假通过)

见知识库 `verification/profiling-bar`。要点:

- 采集路径堆分配 = 0(不是"看起来没分配");
- 必须检查 `droppedEvents == 0` 才能拿 trace 做定量结论;
- 分位数用"多数快帧 + 少数慢帧"的已知形状验证(只断言 "p95 > 0" 不算通过);
- 内存归因注入已知大小后断言**恰好**增加该大小。

```powershell
cmake --build build/x64-Debug --config Debug --target WorldTelemetryTests
build/x64-Debug/tests/Debug/WorldTelemetryTests.exe
```

## 5.1 实测开销(2026-10-05,本机 RTX 3070 / Debug 构建)

同机同场景(`build/x64-Debug/perf3d-project`,约 16 事件/帧),采集**全程开启**对比关闭,
各取采样窗口末段的周期统计行:

| 项 | 采集关 | 采集开 | 差 |
| --- | --- | --- | --- |
| p50 | 5.88 ms | 5.88 ms | 0(低于桶分辨率) |
| p95 | 6.38 ms | 6.38 ms | 0 |
| p99 | 6.88 ms | 6.88 ms | 0 |
| FPS | 170.5 | 169.9 | −0.35% |

**口径与边界**:该数字对应约 16 事件/帧的插桩密度;开销随写入事件数线性增长,
所以在**作用域数量极大**的场景(每帧上万事件)必须重新测,不能套用这个百分比。
另外 `World.Telemetry` 用例用 `TraceBuffer` 的容量/计数证明写入路径本身不分配、不增长。

观测用(不写文件、只打印,可与采集独立开关):

```powershell
$env:WLD_TELEMETRY_STATS = "1"              # 每 2 秒一行 [telemetry] Frame ... 分位数
$env:WLD_TELEMETRY_STATS_SECONDS = "5"      # 改间隔
```

## 5.2 内存归因(M2,2026-10-05)

三段式:见知识库 `contract.memory-attribution`。

| 层 | 回答什么 | 怎么用 |
| --- | --- | --- |
| **Tag** 子系统 | "涨在场景/资产/渲染/脚本哪一块" | 作用域内加 `WLD_MEM_TAG("Renderer.Texture");`(RAII,零分配) |
| **Owner** 资源 | "是哪些纹理/缓冲占的" | RHI 资源创建/销毁自动登记(GPU 驻留) |
| **Site** 调用点 | "哪一行分配的" | 本期未做(候选增强) |

**覆盖口径(读数字前必看)**:

- **按模块生效**:替换式 `operator new` 只覆盖**编译了 hook 的模块**。
  `Engine/src/World/Profiling/GlobalAllocHooks.cpp` 随 World 模块编译;宿主(Editor/Runtime/Game)
  与测试要统计自己的分配,需把该文件加进各自 target 的源列表(状态在 `WorldRuntime.dll` 里,共享同一张表)。
  **没装 hook 的模块的释放不进入统计 ⇒ "活跃字节"只会偏大,不会偏小**(偏大是安全的)。
- `untrackedFrees > 0` 就是这条口径的可见化:**它不是错误**,而是"有释放没被看到"的计数
  (典型来源:进程静态初始化期分配、MemoryTrack 启动后才释放)。
- 池级(分配器)是**轮询式**:由引擎每帧 `Tick()` 采样,所以面板不开也有峰值。
- 单次采集的内存表在 `MemoryTrack::Init()` 一次性分配(默认 524288 槽 ≈ 12 MiB,
  `WLD_MEMORY_TRACK_SLOTS` 可调);之后采集路径零分配、零锁跨调用。
- `WLD_MEMORY_TRACK=0` 关闭内存归因(表不分配、hook 直接直通)。

**关闭时机与泄漏报告**:`Telemetry::Shutdown()` → `MemoryTrack::Shutdown()`,
它跑在 `Reset()`/`Shutdown()` **之前**(旧实现顺序是反的,真泄漏已被清空)。
关闭时输出"活跃分配数 + 字节",以及归因不完美时的 `untrackedFrees` / 表满计数。

**实测基线**(`build/x64-Debug/perf3d-project`,隐藏启动,10 秒稳态):

```
Memory: heap live 23.64 MB (peak 23.64 MB, 415 allocs live) | pools 0.00 MB | GPU 49.61 MB | untracked frees 17
```

## 5.3 调用点归因(Site)与转储(M2b,2026-10-05)

三段式的第三段落地:**1/N 采样 + 离屏符号化**(DbgHelp,读同目录 PDB)。

```powershell
$env:WLD_MEMORY_SITES = "1"          # 1/N 采样;1 = 全采(诊断用),默认 64,0 = 关
$env:WLD_MEMORY_DUMP  = "15"         # 每 15 秒写一份转储
$env:WLD_MEMORY_DUMP_PATH = "tmp\mem\dump"   # 产出 dump-001.txt / dump-002.txt ...
```

转储内容(人读 + 可 diff):总量行、**Top 活跃调用点**(模块!函数+偏移 [文件:行])、
活跃分配的**大小直方图**、以及按字节排序的 Top 活跃分配。

三条纪律(写在 `SymbolResolver` 里):

- **调用点在 hook 自己的帧里取**(`_ReturnAddress()`):在 `MemoryTrack` 里取会退化成
  `RecordAllocate` 自己 —— 实测踩过,站点表全是同一个符号。
- **符号化只在转储时做**,结果缓存;解析自身产生的分配被 `SuppressionScope` 抑制,
  否则观察者污染被观察对象(这正是我们在修的老毛病)。
- 采样率决定代价:`1/N` 时**只有 1/N 的分配**建站点表项,其余分配只走原有的簿记。

> 判读提示:`x<次数>` 列在**跨模块**场景会偏大。分配在装了 hook 的模块、
> 释放却发生在没装 hook 的模块时,只记得到分配、记不到释放(见 §5.2 的按模块口径)。
> **以字节为准,次数只作相对参考。**

## 5.4 Profiler 面板(M3,2026-10-05)

编辑器 `profiler` 面板(`面板菜单 ▸ Profiler`,或 AI `ui.activate panel=profiler`):

- **帧时间曲线**(最近 600 帧,固定 0..100ms 量程)+ **掉帧阈值线**(= 2× p50,引擎给的判据);
- 分位数行(p50/p95/p99/max + 窗口帧数,口径写明是"最近 N 帧");
- 内存概览(heap live/peak、活跃分配数、GPU 驻留);
- 采集状态(进行中帧数 / 上次落盘路径与丢弃数)。

面板与 `prof.stats` / 导出文件 / 无障碍节点**读同一个 `StatsSnapshot`**,键名与 §3 的数据字典一致;
刷新零堆分配(帧历史用定长缓冲)。

## 5.5 采集后端可替换(ITraceSink)

插桩点(宏/作用域/计数器)是长期资产,采集后端是可替换实现:

```cpp
// 注册一个外部后端(可选;进程内唯一,生命周期由注册方保证)
World::Profiling::SetTraceSink(&mySink);   // nullptr = 回到内置 Chrome Trace
```

- 内置后端 `chrome-trace` 始终存在,**即使外部 sink 注册失败也总有可用格式**;
- 换后端只换 sink,**插桩点一行不动**(这是当初选"自研薄层 + 可选成熟后端"的理由);
- trace 元数据里写 `trace_sink`,人/AI 读文件时能确认是谁写的;
- `WLD_TELEMETRY_SINK=<name>` 是选择开关(未注册的名字会告警并回退到内置)。

## 5.6 面板按侧栏宽度设计(2026-10-06)

Profiler / 内存面板的可用宽度常只有 **~240px**,所以:

- 每行 = **一个整宽标签**(名字 + 关键数字拼成一行),不用"多列 + 进度条"的横向组合;
  多列在窄面板下会被推到裁剪区外 ⇒ 面板"看起来没有数据"(见知识库
  `trap.narrow-panel-overflow-hides-data`);
- 长名字按固定字段宽裁剪并补 `…`;数字用 `M` 缩写(`0.01/50.0M pk0.01`),单位不写全拼;
- 帧时间曲线固定 `0..100ms` 量程(配 `2× p50` 阈值线),这样"看得见波动";
- 内存趋势曲线量程**贴着数据取**,因为内存波动常只有 0.1~1MB ——
  用 `0..峰值` 会把曲线压成贴着顶的一条直线。

## 5.7 曲线怎么画(2026-10-06)

趋势/帧时间曲线是"点数 > 绘图宽度"(600 点 / ~220px),因此 `WuiPlot` 用
**按像素列 min/max 抽取**绘制,不是逐点连线:

- 恒定序列也能显示(每列图元至少 1px 高);
- 尖峰不被抽掉(列内保留极值);
- 绘制命令从 O(样本数) 降到 O(列数)。

逐点连线在亚像素间距下会退化成空图元,**平坦的序列会整条不显示**
(实测:显存长期 49.6MB 时曲线消失)。细节与自检写法见知识库
`trap.subpixel-series-invisible`。

参考线(`Guides`)与 `WarnThreshold` 的分工:前者是通用多条标记(p50/p95/p99),
后者是掉帧告警的专用语义(一条、红)。**量程外的参考线不画** ——
画到边缘会被误读成"刚好到顶"。

## 6. 已知边界

- **无采样式热点**(不做无插桩栈回溯):未插桩的位置看不到,这是明确取舍。
- 分位数分辨率 0.25ms、上限 128ms(超出落溢出桶并置 `overflowed`)。
- **GPU 计时已异步化(2026-10-06)**:OpenGL 侧不再用 `GL_QUERY_RESULT` 阻塞等待;
  改为先探 `GL_QUERY_RESULT_AVAILABLE`,任一查询未就绪就整段跳过、下一帧重发
  ⇒ **不阻塞、不读脏值**(最坏是滞后一轮的旧值)。结果窗口 = `kFramesInFlight`(3 帧),
  GPU 落后超过 2 帧时该轮采样被丢弃。数字带**滞后**,不要用它做逐帧因果。
- GPU 计时覆盖=**阴影通道 + 3D/2D 主通道**(起点已前移到阴影之前;阴影关闭时等价于旧口径)。
  开关仍是项目清单 `rendering.gpu_timing`(**默认关**;打开后每帧多约 3 次拷贝 + 6 次就绪探测)。
- 内存归因的**次数**列跨模块会偏大(见 §5.3 判读提示);**字节**仍是可靠信号。
- 调用点采样是**扁平**的(只取一层返回地址),不给完整调用链。
- 内存趋势曲线保留最近 600 帧(引擎环形缓冲上限 3600 帧);曲线画的是**活跃字节**。
- 设备不支持时间戳查询时,GPU 数字保持 0 并**显式告警**(不静默)。
