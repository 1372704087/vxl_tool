# VXL2 — RA2 VXL 的宽字段超集格式

版本：6.0（2026-09）
状态：规范终稿，实现中
作者：vxl_tool

> v6.0 在 v5 基础上解除「模型体量」硬上限。v3/v4/v5 的区块化 payload 复用 `u16` 保存各轴块数、
> 非空块数、块索引、去重目标与目录条目（⇒ 单轴 ≤ 65535 块 ≈ 209 万格，非空块 ≤ 65535），
> v6 将这些**外部索引字段全部扩为 `u32`**（单轴 ≤ 2^32-1 块 ≈ 1370 亿格，非空块 ≤ 2^32-1），
> 块内记录/操作码/占用位图/去重/共享调色板/多算法/并行压缩等全部沿用 v5，仅索引字宽不同；
> v6 读取器完整兼容 v1..v5（旧文件 u16 索引仍按旧布局解析），通过 `version=6` 分派。
> v5.0 在 v4 基础上新增「最省存储」：稀疏块占用位图、多向游程（x/y/z 三轴）、块级内容去重、
> 跨 section 共享调色板、多算法压缩（zlib/zstd/lz4/raw 默认 zstd）、并行逐块压缩。各特性经第二
> 布局字节自描述、按需启用、读取无损；v5 读取器完整兼容 v1..v4。
> v4.0 在 v3 基础上新增「高级区块体」，将 v3 的区块化 body 扩展为**自描述布局**：节级量化调色板
> （色→1B 索引）、normal/layer 位域打包（合并进 1B）、纵向游程合并、逐块 CRC32、块索引目录
> （支持 mmap 随机定位）。各高级特性按需启用、读取无损，编码器自动权衡；v4 读取器完整兼容
> v1/v2/v3。
> v3.0 在 v2 基础上新增「优化存储」扩展，聚焦体积与加载效率：文件级 CRC32 校验、
> 扩展元数据键值段、节内区块化压缩 body（坐标降位 + 逐块 zlib）。v3 读取器完整兼容
> v2/v1；v2 读取器遇 v3 的未知 chunk 一律跳过（长度明确，安全）。
> v2.0 在 v1 基础上引入统一的 chunk 扩展框架，新增可选调色板段、轴语义/物理缩放元数据、
> 内嵌缩略图、子材质命名 + PBR、自定义精细法向、LOD 层级。v1 文件仍可直接读取。

## 1. 概述

VXL2 是与 RA2 `.vxl` 并存的一种体素容器，用于解除 RB2 VXL 的三项硬限制：

| 属性 | RB2 VXL | VXL2 |
|---|---|---|
| 单轴格数 | ≤ 255（span `skip/count` 均为 1 字节） | 任意（`u32`） |
| 颜色 | 8-bit 调色板索引（256 色） | 每体素 `u32 RGBA` 直存 |
| 单格实体 | 每列每个 z 至多一个 | 每格可叠加多条记录（`layer` 分层材质） |
| 多实体 | 多 section | 多 section（沿用） |

VXL2 **不是**全新格式，而是 RB2 VXL 的超集：它保留「魔数 + 多 section」的外层骨架，
把每个 section 内部的 span RLE body 替换为**线性、定长的体素记录流**。定长记录 + 明确
offset 表使其非常容易被第三方程序解析。

所有多字节字段均为**小端序**（与 RB2 VXL 一致）。

## 2. 文件整体布局

```
+------------+               +----------------------------+
| 文件头     |               |  Section 0                 |
|  (magic +  |               |  Section 1                 |
|   count)   |  ---------->  |  ...                       |
+------------+               +----------------------------+
```

### 2.1 文件头（16 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | 固定为 ASCII `'V''X''L''2'`（0x56 0x58 0x4C 0x32） |
| 4 | 4 | version | 格式主版本，当前 `1` |
| 8 | 4 | sectionCount | section 数量（≥1） |
| 12 | 4 | reserved | 保留，填 `0` |

校验：`magic != "VXL2"` → 判定为非法；`sectionCount == 0` → 非法。

### 2.2 Section（变长）

每个 section 顺序排列，紧跟文件头。结构：

| 偏移(相对本 section) | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | nameLen | 名称字节数（UTF-8，≤ 65535） |
| 4 | nameLen | name | UTF-8 名称 |
| 4+nameLen | 4 | sizeX | X 轴格数（≥1） |
| +4 | 4 | sizeY | Y 轴格数（≥1） |
| +4 | 4 | sizeZ | Z 轴格数（≥1） |
| +4 | 4 | voxelCount | 本 section 的体素记录条数 |
| +4 | voxelCount×20 | body | 线性定长体素记录流（见 §3） |

校验：任一尺寸为 0 → 非法；`voxelCount` 过大超出剩余字节 → 非法（防越界）。

## 3. 体素记录（body，每条定长 20 字节）

体素记录按扫描顺序存储：先 z 递增，再 y，再 x（即记录 `(x,y,z)` 以 `z*sizeY*sizeX + y*sizeX + x` 递增排布；**不强制排序**，读取方以坐标定位）。每条：

| 偏移(条内) | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | x | 列坐标（0..sizeX-1） |
| 4 | 4 | y | 行坐标（0..sizeY-1） |
| 8 | 4 | z | 层坐标（0..sizeZ-1） |
| 12 | 4 | rgba | 0xRRGGBBAA，预乘？否，直通不动。α=0 视为透明层 |
| 16 | 2 | normal | 法向索引；0=由引擎自算，非 0 见 §5 |
| 18 | 2 | layer | 同格材质层号，0=底层，越大越靠上 − **多材质核心** |

校验：`x>=sizeX || y>=sizeY || z>=sizeZ` → 越界，该条丢弃或整文件判非法（由解码器策略决定，
推荐判非法并返回失败）。

## 4. 多实体与多材质语义

- **多实体** = 多 section。每个 section 是一个独立网格（可不同 `sizeX/Y/Z`），名称通常
  `Body` / `Turret` / `Barrel`，与 RB2 VXL 的多 section 语义一致。
- **多材质** = 同 `(x,y,z)` 允许多条记录，以 `layer` 区分：
  - `layer=0` 为最底层实体，向上一层图层覆盖在上面。
  - 半透明渲染时按 `layer` 升序（底层先画）合成；`layer` 越高越靠上。
  - 图层数不限（文件内实际为准）。
- 降级（导出到单材质目标的规则）：只取每个格子 `layer` 最小的那条作为该格颜色，
  其余图层按 §7 降级丢弃或合并。

## 5. 法向

`normal` 为 16-bit 无符号索引：`0` 表示按几何自动计算；非 0 时索引到与 RB2 VXL 相同的
26 向 normals 表（高位 8-bit 可保留给扩展，实现前 8 位 `0x00FF` 掩码使用）。若要表达更细
法向，可约定表外索引表示「自定义法向跟随 RGBA 高 16 位」——当前版本不启用，保留为 `0`。

## 6. 编码建议

- x/y/z 建议覆盖 `sizeX/Y/Z` 全轴实体布尔占用，但**允许跨层**（多材质核心价值）。
- 调色板未内嵌：RGBA 已直存，无需全局调色板（这是相对 RB2 VXL 的关键改进）。
- 若需 RB2 VXL 传统交换（引擎只读 `.vxl`），请用 §7 转换并接受 255 轴长 + 单层限制。

## 6a. v2 扩展（version=2）

v2 用统一的 **chunk（type u32 + len u32 + payload）** 框架承载可选扩展。
读取器对未知 chunk 一律跳过（type/len 前置，长度明确），因此向后/向前都兼容。

### 全局 chunk 段

紧跟在 16 字节文件头之后；head.flags 的 bit0=1 表示存在本段；开头为 chunk 个数 u32。chunk 类型：

| type | 名称 | payload 布局 |
|---|---|---|
| 1 | 可选调色板段 | `u16 count` + `count` 个 RGBA（每项 u32）。直存非索引；仅用于需要调色板的交换流程 |
| 2 | 元数据 | `u8 upAxis`（'y'/'z'）+ `f32 unitScale`（物理缩放） |
| 3 | 内嵌缩略图 | `u8 format`（1=PNG，2=裸 RGB）+ 原始字节 |
| 4 | 文件级 CRC32（v3） | `u32 crc32`——对「sections 区段」（全局段之后到文件尾）的标准 CRC32 |
| 5 | 扩展元数据键值（v3） | `u32 kvCount` + 每条 `u32 keyLen` + key(UTF-8) + `u32 valLen` + val(UTF-8)。键值对如 `creator` / `license` / `description` |

### 节内 chunk 段

在每个 section 的 `voxelCount` 之后、body 之前；开头为 chunk 个数 u32。chunk 类型：

| type | 名称 | payload 布局 |
|---|---|---|
| 1 | 子材质命名 + PBR | `u16 matCount` + 每条 `u16 layer` + `u16 nameLen` + name(UTF-8) + `f32 roughness` + `f32 metalness` + `u32 emissive`(0xRRGGBB) |
| 2 | 自定义精细法向表 | `u32 count` + `count` 个归一化方向（每向 3×f32） |
| 3 | LOD 层级 | `u32 lodLevel`（0=最高精度） |
| 4 | 区块化压缩 body（v3） | 见 §6b。存在时取代定长记录流；每体素可带 `u8 userData` |
| 4 | 区块化压缩 body（v4） | 见 §6c。同一 chunk type，按文件 `version=4` 分派到 v4 布局 |

### v2 法向索引语义扩展

`normal` 字段：`0`=自动计算；`1..26`=标准 26 向法向表；`>=27` 引用节内自定义法向表，
即索引 `normal-27`（需表内存在，否则回退自动计算）。

### 与 v1 的兼容

- **读取**：按文件头 `version` 分支。version=1 走旧布局（无 chunk 段）；version=2 走本节布局。
- **写入**：编码器固定写 version=2；无任何扩展时 `flags.bit0=0`（不写全局段），
  每个 section 的 chunk 个数为 0，仅比 v1 多 4 字节（chunkCount 恒写在 voxelCount 后）。

## 6b. v3 优化存储（version=3）

v3 在 v2 的 chunk 框架内新增三项能力，聚焦体积与加载效率：

### 文件级 CRC32 校验（全局 chunk type=4）

- payload：`u32 crc32`——标准 CRC32，覆盖「全局段之后到文件尾」的 sections 区段字节。
- 读取时若 `global.crc32 != 0` 且调用方请求校验，则重算比对；不符即判文件损坏。
- 写入时编码器保留该字段占位，在所有 sections 编码完成后回填真实 CRC。

### 扩展元数据键值段（全局 chunk type=5）

- payload：`u32 kvCount` + 每条 `u32 keyLen` + key(UTF-8) + `u32 valLen` + val(UTF-8)。
- 典型键：`creator` / `license` / `description` / `units` 等，语义由宿主定义。

### 区块化压缩 body（节内 chunk type=4）

对该 section 的 body 采用「分块 + 坐标降位 + 逐块 zlib 压缩」，支持流式/局部加载。

payload 布局：

```
u32 blockBase        块边长（固定 32）                           # 4 字节
u16 nBlockX, nBlockY, nBlockZ   各轴块数                            # 6 字节
u8  userBytes        每体素 userData 字节数（0 或 1，当前支持 0/1）     # 1 字节
u32 nTotal           体素总数（校验用）                             # 4 字节
u16 blockCount       非空块数                                     # 2 字节
--- 每非空块 ---
u16 blockIndex       块索引（(bz*nY+by)*nX+bx）                    # 2 字节
u16 localCount       本块体素数（≤65535）                          # 2 字节
u32 cLen             zlib 压缩流字节数                              # 4 字节
zlib流               localCount × 记录
```

块内记录（定长，`userBytes` 是否存在决定长度）：

```
u8  rx, ry, rz       块内相对坐标（0..blockBase-1）   # 3 字节
u32 rgba             0xRRGGBBAA                     # 4 字节
u16 normal           法向索引                          # 2 字节
u16 layer            材质层号                          # 2 字节
u8  userData         [可选]per-voxel 用户属性通道（语义宿主定义） # 0/1 字节
```

说明：

- 恢复坐标：`绝对 = 块原点 + 相对偏移`；块原点 = `(块索引坐标) * blockBase`。
- `userData` 通道：仅当该 section 存在任一非零 userData 时才写（`userBytes=1`），否则省略以省空间。
- 读取顺序：体素按块索引升序输出，块内保持原输入顺序，不保证全局扫描序——读取方按坐标定位即可。
- 相比 v2 定长记录：坐标 4×3→1×3 字节（稀疏场景）、逐块 zlib、仅存非空块，典型压缩比约 30%~70%。
- 越界/损坏防护：块数、每块体素数、单块坐标、总计数全部校验，不符即判非法。

## 6c. v4 高级区块体（version=4）

v4 沿用 v3 的「分块 + zlib」骨架，把 body 改成**自描述布局**：用一个 `layout` 字节声明启用了哪些
高级特性，读取方按标志解析；特性未启用时字段自然退化（色仍存 `u32`、法向/层仍各存 `u16`），
因此调色板/打包在数据不满足条件时自动关闭，**并不损失精度**。v4 文件 `version=4`，body 用与 v3
相同的节内 chunk type=4，读取器按文件版本分派。

与 v3 的差异一句话：v3 的块内记录是「固定每体素若干字节」；v4 是「带操作码的字节流」，允许
同 (x,y) 沿 z 连续、字段一致的体素合并成一个纵向游程，配合调色板与位域打包，进一步压缩。

### 布局标志（payload 首字节 `layout`）

| bit | 常量 | 含义 |
|---|---|---|
| 0 | k4UserData | 每体素携带 `u8 userData` |
| 1 | k4Palette | 启用节级量化调色板：颜色存 1B 索引，色表在 body 内 |
| 2 | k4PackedAttr | `normal`(5b) \| `layer`(3b) 合并进 1B |
| 3 | k4VerticalRun | 启用纵向游程操作码（通常总是启用） |
| 4 | k4BlockCrc | 每块追加 CRC32（通常总是启用） |
| 5 | k4IndexTable | body 结尾追加块索引目录（通常总是启用） |

### payload 布局

```
u8  layout            布局标志（表 6c-1）                              # 1 字节
u8  reserved                                                        # 1 字节
u16 blockBase         块边长（当前 32）                                # 2 字节
u16 nBlockX, nY, nZ   各轴块数                                         # 6 字节
u32 nTotal            体素总数（校验用）                               # 4 字节
u16 blockCount        非空块数                                         # 2 字节
u16 palLen            调色板项数（layout.bit1 关闭时=0）                # 2 字节
u8  reserved          (header 固定 21 字节，末 3 字节保留=0)            # 3 字节
--- 调色板表（palLen 项，仅 layout.bit1 开启） ---
u32 rgba              RGBA（顺序即索引 0..palLen-1）                   # 4×palLen
--- 每非空块 ---
u16 blockIndex        块索引（(bz*nY+by)*nX+bx）                      # 2 字节
u32 rawLen            解压后字节数                                    # 4 字节
u32 cLen              zlib 压缩流字节数                                # 4 字节
u32 crc32             该块解压后字节(raw)的 CRC32（layout.bit4）        # 4 字节
zlib流                块内记录流（见下）
--- 块索引目录（layout.bit5）---
u16 dirCount          应等于 blockCount                             # 2 字节
u16 blockIndex        块索引                                          # 2 字节
u32 offset            该块 header 在 body 中的绝对偏移（支持 mmap 随机定位） # 4 字节
```

### 块内记录流（操作码编码）

每条以 1 字节 `kind` 开头：

| kind | 操作码 | 布局 |
|---|---|---|
| 0 | k4KindLiteral（单个体素） | `kind` + `rx` + `ry` + `rz` + fields |
| 1 | k4KindRun（纵向游程） | `kind` + `rx` + `ry` + `z0` + `count` + fields |

其中字段区 `fields`（宽度由布局决定）：

| 布局 | 字段宽度 |
|---|---|
| 色 | `usePalette?1:4` 字节（1B=调色板索引，4B=直存 RGBA） |
| 法向/层 | `packedAttr?1:4` 字节（1B=`normal\|(layer<<5)` 打包；4B=normal u16 + layer u16） |
| userData | `k4UserData?1:0` 字节 |

字段区各特性独立开关：调色板占用 1B、打包占用 1B、userData 占用 1B，三种可自由组合。

- **Literal**：绝对恢复为 `(块原点+rx, 块原点+ry, 块原点+rz)`。
- **Run**：`count` 个体素为同 (x,y)、z 从 `z0` 连续递增 `count` 步，字段只存一次；恢复为
  `(块原点+rx, 块原点+ry, 块原点+z0+k)`，`k=0..count-1`。仅当 `count≥2 && count≤255` 时启用合并。

### 编码器自动权衡

- **调色板**：仅当该 section 唯一 RGBA 数 ≤ 256 时启用（无损直存索引表）；否则关闭，色仍存 4B。
- **打包**：仅当所有体素 `normal<32 && layer<8` 时启用；否则关闭，法向/层仍各存 `u16`。
- **纵向游程**：Dense 数据（无重体素堆叠）时生效；同格多 `layer` 因字段不同不被合并，作为 Literal 存。
- **userData**：仅当存在任一非零 per-voxel 字节时携带通道，否则省略。
- **CRC / 索引目录**：默认始终启用（每块 CRC + 结尾目录），开销小（每块 4B + 目录 6B/块）。

### 数据完整性

- **逐块 CRC32**：覆盖该块解压后的原始字节。即便不启用文件级 CRC，篡改某块数据也会被该块检出
  （解压失败或 CRC 不符）。这是 v4 相对全文件 CRC 的**更细粒度**完整性保障。
- 加载所有块的 `nTotal` 计数校验；块索引越界、调色板索引越界、坐标越界均判非法。

### 命令行/API

`Vxl2::Encode(..., Vxl2EncodeMode::BlockedOptimized)` 写 `version=4` 高级区块体；
读取自动识别。典型压缩比相对 v2 约 60%~88%，相对 v3 再省约 15%~25%（游程 + 调色板/打包越有效省越多）。

## 6d. v5 最省存储（version=5）

v5 在 v4 的自描述块体基础上再叠加五类优化：**稀疏块占用位图**、**多向游程**（x/y/z 三轴 RLE）、
**块级内容去重**、**跨 section 共享调色板**，并把逐块压缩升级为**多算法可切换**（zlib / zstd / LZ4 /
raw，默认 zstd）与**并行压缩**。所有特性通过第二布局字节 `layout2` 声明，按需启用、读取无损，
编码器自动权衡、保障合法边界全部判非法。

### 压缩算法（`VXL2_ALG` 环境变量）

默认 `zstd`；可选 `zlib` / `lz4` / `raw`（调试）。算法位写入 `layout2` bit4-5：

| 值 | 算法 |
|---|---|
| 0 | raw（不压缩） |
| 1 | zlib（deflate） |
| 2 | zstd |
| 3 | lz4 |

### 第二布局字节 `layout2`（payload 第 18 字节，v4 曾保留）

| bit | 常量 | 含义 |
|---|---|---|
| 0 | k5Occ | 至少一块使用了占用位图编码 |
| 1 | k5DirRuns | 使用了 x/y 方向游程操作码 |
| 2 | k5Dedup | 至少一块是去重引用 |
| 3 | k5PalShared | 色存「文件级共享调色板全局索引」，节内不写色表 |
| 4-5 | alg | `((layout2>>4)&3)` 压缩算法 |
| 6-7 | 保留 | 填 0 |

### payload 布局

```
u8  layout           布局标志（同 §6c 表）                            # 1 字节
u8  reserved0                                                       # 1 字节
u16 blockBase        块边长（当前 32）                                # 2 字节
u16 nBlockX, nY, nZ  各轴块数                                         # 6 字节
u32 nTotal           体素总数（校验用）                               # 4 字节
u16 blockCount       非空块数                                         # 2 字节
u16 palLen           调色板项数（k4Palette 关闭时=0）                  # 2 字节
u8  layout2          第二布局字节（bit3 依赖全局调色板段）              # 1 字节  ← v5
u8  reserved1        (header 固定 21 字节)                            # 1 字节
--- 节级调色板表（palLen 项，仅 k4Palette 开启且非共享；顺序即索引） ---
u32 rgba ...
--- 每非空块 ---
u16 blockIndex       块索引（(bz*nY+by)*nX+bx）                      # 2 字节
u8  bmode            0x01=占用位图；0x02=去重引用
-- 去重引用（bmode & 0x02）--
u16 dedupBlockIndex  内容相同的目标块索引                              # 2 字节
-- 否则 --
u32 rawLen           解压后字节数                                    # 4 字节
u32 cLen             压缩流字节数（算法由 layout2 决定）                # 4 字节
u32 crc32            该块 raw 的 CRC32（始终携带）                     # 4 字节
压缩流               块内记录流（按所选算法压缩）
--- 块索引目录 ---
u16 dirCount         应等于 blockCount                               # 2 字节
u16 blockIndex       块索引                                          # 2 字节
u32 offset           该块 header 在 body 中的绝对偏移                  # 4 字节
```

### 块内记录流（多向操作码）

每条以 `kind` 开头，字段区 `fields` 宽度与 §6c 一致（色 `usePalette?1:4`、法向/层 `packedAttr?1:4`、
userData `k4UserData?1:0`），字段只存一次、恢复时沿轴展开：

| kind | 操作码 | 布局 |
|---|---|---|
| 0 | literal（单个体素） | `kind` + `rx` + `ry` + `rz` + fields |
| 1 | z-run（纵向游程） | `kind` + `rx` + `ry` + `z0` + `count` + fields |
| 2 | x-run（水平游程） | `kind` + `ry` + `rz` + `x0` + `count` + fields |
| 3 | y-run（水平游程） | `kind` + `rx` + `rz` + `y0` + `count` + fields |

x/y 游程让同色、沿 x 或 y 连续的一行体素合并成一条记录，配合 z-run 覆盖三个主轴，相比 v4 仅纵向
游程进一步压缩条带数据。

### 稀疏块占用位图（bmode 0x01）

对占用稀疏的块，编码器尝试用「有界盒 + 位图」替代操作码流：先写盒尺寸 `u8 bx,by,bz`，再写
`(bx*by*bz+7)/8` 字节占用位（0=空，跳读），最后按扫描顺序只存被占格子的 fields。仅当
`3 + 位图字节数 + cnt*fieldW < 操作码流字节数` 时采用，故始终不劣于操作码流。对含大量空格的
模型（飞机尾翼、厚壳内部）收益明显。
注意：占用位图不存盒的局部原点，解码按**块原点**重建坐标，因此仅当块内体素锚定在局部原点
（`mnx=mny=mnz=0`）时启用；否则退化为操作码流，避免坐标错移。

### 跨 section 共享调色板（layout2 bit3）

v5 编码器合并全部 section 的唯一色为文件级共享调色板（≤256 色）写入全局 chunk type=1；各节色存
**全局索引**并置 bit3，节内不再写色表。仅当调用方未提供独立全局调色板时自动启用，以免覆盖其
显示语义。解码遇 bit3 时取全局调色板解析。多 section 模型（Body/Turret/Barrel 同色复用）省去
重复色表。

### 块级内容去重（layout2 bit2）

编码器对每个非空块的解压后 raw 做 FNV-1a 散列 + 字节等值确认；内容完全相同的块只存一份，后续
块记 `去重引用`（`u8 bmode=0x02` + `u16` 目标块索引），解码时复用目标块内容（目标块必先出现，
块间无顺序依赖）。对称几何/重复组件显著省空间。

### 并行压缩与权衡

- 逐块压缩在**线程池**内并行执行（结果按块序写回，字节级可复现）；解压按需逐块。
- API：`Vxl2::Encode(..., Vxl2EncodeMode::BlockedV5)` 写 `version=5`；读取自动识别。
- **实测示例**（size40 渐变立方体，32000 体素）：v2 `640044B` → v3 `99240B` → v4 `87526B` → **v5 `16691B`**，
  相对 v4 再省约 81%（共享调色板 + 稀疏位图 + 多向游程 + zstd 综合；多 section 共享色板增益更大）。

### 数据完整性

- 沿用 v4 逐块 CRC32 + `nTotal` 计数；坐标越界、块索引/调色板索引越界、去重目标缺失均判非法。
- 共享调色板索引越界、占用位图字节数不足/字段数不符，均判非法，不为满足边界而放宽。

## 6e. v6 超大模型（version=6）

v6 不新增编码特性，仅将区块化 body 的**外部索引字段由 `u16` 放宽为 `u32`**，解除模型体量上限。
块内记录流、操作码、占用位图、去重、共享调色板、压缩算法与并行策略与 v5 完全一致。

### 放宽的字段（u16 → u32）

| 语义 | v5（u16）上限 | v6（u32）上限 |
|---|---|---|
| 单轴块数 `nX/nY/nZ`（块=32³） | 65535 → 单轴 ≈209 万格 | 2^32-1 → 单轴 ≈1370 亿格 |
| 非空块数 `blockCount` | 65535 | 2^32-1 |
| 块索引 `blockIndex` / 去重目标索引 | 65535 | 2^32-1 |
| 块索引目录条目数 / 目录 offset | 65535 | 2^32-1 |
| 全局体素计数 `nTotal` | — | 2^32-1 |

### payload 布局（相对 v5 的差异）

payload 头为 **30 字节**（v5 为 30 字节），相较 v5 仅以下字段字宽变化，其余字节序不变：

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 1 | `layout`（v5 语义一致：块尺寸等） |
| 2 | 4 | blockBase（32³，u32） |
| 6 | 4 | nX（u32） |
| 10 | 4 | nY（u32） |
| 14 | 4 | nZ（u32） |
| 18 | 4 | nTotal（全局体素数，u32） |
| 22 | 4 | blockCount（非空块数，u32） |
| 26 | 2 | 节级调色板长度（u16，未共享时） |
| 28 | 1 | `layout2`（v5 语义一致：稀疏/去重/共享/算法标志） |
| 30… | — | 尾随：调色板（可选）→ 块目录/数据 → 块索引目录 |

块目录条目：v5 为 `u16 blockIndex + 1B bmode [+ u16 dedupTarget / + u32 rawLen + u32 compLen + u32 crc + comp]`；
v6 将 `blockIndex`、`dedupTarget`、`rawLen/compLen` 全部改为 **u32**（压缩数据字节仍原样串联）。
块索引目录：`u32 条目数 + (u32 blockIndex, u32 offset)×条目数`（v5 中目录计数与 blockIndex 为 u16、offset 为 u32）。

其余（块内体素记录操作码、每块 CRC、稀疏占用位图语义）均与 v5 完全相同，读取器可复用 V5 逻辑对块内容解码。

### 编码器约束

- 仅在 `nX/nY/nZ ≤ 2^32-1` 的极端校验下放行；`blockBase > 127`、`sizeX/Y/Z` 任一为 0 → 拒绝。
- 索引以 `u32` 写出，不进行 u16 截断；解码端按 `version>=6` 读 u32、`version<6` 读 u16，天然双向兼容。
- 体素坐标本身仍为 `u32`（v1 起即如此），故 v6 不额外放宽单格坐标；仅放宽「块级聚合索引」。

### 命令行/API 与实测

- API：`Vxl2::Encode(..., Vxl2EncodeMode::BlockedV6)` 写 `version=6`；读取自动识别（`version>=6` 分派 v6）。
- CLI：`vxltool vxl2 <src>.vxl -o <out>.vxl2 --v6`（其余 `--v5/--v4/--v3/--crc/--meta` 选项同样适用）。
- `--v6` 与 `--v5` 互斥且优先；未指定时默认写兼容 v2 的定长格式。
- **边界实测**（x 跨 2,097,152 格、65536 个非空块，触碰 v5 的 u16 上限）：v5 拒编，**v6 往返成功**；
  普通 size40 模型（8000 体素）v6 与 v5 体积相当。

## 7. 兼容性与降级路径

- **读取**：工具按前 4 字节识别魔数——`VXL2` 走本格式解码；否则按 RB2 `.vxl` 解码。
- **写入 v1（投影降级）**：把 vxl2 数据投影为 RB2 section：
  1. 尺寸钳到 ≤255（超出则报错，不静默）；
  2. 每个格子仅保留 `layer` 最小的那条；
  3. RGBA → 在 768 字节调色板中取最近色索引（欧氏距离）；
  4. `normal` 降至 8-bit（>255 置 0）。
- **不破坏**：vxl2 读取器永远能读回旧 `.vxl`（向下兼容）；旧工具不识别 vxl2，但不影响文件本身。
- **v6 兼容性**：`version=1..6` 均自动识别。version 6 与 version 5 共用自描述块体，仅外部索引
  字宽不同（v6=u32、v5=u16），读取按 `version>=6` 走 §6e、version 5 走 §6d、version 4 走 §6c、
  version 3 走 §6b。逐块 CRC 提供比全文件 CRC 更细粒度的完整性保障，可用于局部加载前只校验所需块。
- **v3 兼容性**：v3 文件对 v2 读取器仍是合法文件——新增的全局/节内 chunk 类型被当作
  「未知 chunk」跳过，代价是 v2 读取器读它只能拿到空 body（区块体 chunk 不识别），
  因此**本地生产推荐 v3+ 读者**；v1/v2 文件对 v3+ 读取器完全正常。

## 8. 解析示例（伪代码，仅定位）

```
if read_u32(f,0) != 0x32_4C_58_56("VXL2"): return not_v2
version  = u32 at 4
flags    = u32 at 12
secCount = u32 at 8
if version >= 2 and flags & 1: skip_global_chunk_segment()   # §6a/6b 全局 chunk
for s in 0..secCount-1:
  nameLen   = u32; name = read(nameLen)
  sx,sy,sz  = u32,u32,u32
  n         = u32
  if version >= 2 and n_chunks = u32 at here:
    for each chunk: type,len =>
      if type==4(#body):
        if version>=5: decode_blocked_v5(§6d)
        elif version>=4: decode_blocked_v4(§6c)
        else: decode_blocked_v3(§6b)
      else: skip(len)
  else:
    for i in 0..n-1:
      x,y,z,rgba = u32,u32,u32,u32   # 20-byte record
      normal,layer = u16,u16
```

## 9. 扩展预留

- `version` 提升时，增补字段一律追加到 Section 尾部并改写 version，读取方按版本分支。
- 预留的 `reserved` 可承载「全局调色板可选段」等后续能力。
- v3 的 `userBytes` 预留扩展：当前支持 `0/1`；未来可支持多字节 per-voxel 属性通道
  （如 `u8` 材质法向组合、`u16` 自发光等），只需相应提升 `userBytes` 并保持块内记录定长。
- v4 的 `layout` 预留位（bit6/bit7）可用于后续特性（如体素 `u16` 属性、块间 delta 编码等）；
- v5 的 `layout2` 预留位（bit6/bit7）与压缩算法位（bit4-5）可扩展更多算法或流式解压提示，
  由编码器按需开启、读取器按标志解析，天然向前兼容。
- **userData 语义 schema**：per-voxel `userData` 的每位含义由宿主自定义，建议通过全局
  元数据键值段（type=5）写入键 `vxl2.userdata.schema` 记录逐字段的描述（如 `"id:8;hp:8"`），
  供下游复现语义。