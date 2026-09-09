#pragma once

// VXL2 — RA2 VXL 的宽字段超集（规范见 docs/VXL2.md）。
// 解除三项硬限制：轴长<=255 → u32；256 色调色板索引 → 每体素 RGBA；单格单实体 → layer 分层多材质。
// 读取按前 4 字节魔数判 v1/v2，天然向下兼容旧 .vxl。
//
// v2 版本（version=2）在 v1 的定长体素记录基础上，用统一的"chunk（类型+长度+payload）"
// 框架承载现代扩展，且 chunk 未知类型可跳过，便于向前兼容：
//   * 全局 chunk：可选调色板段、轴语义/物理缩放元数据、内嵌缩略图
//   * 节内 chunk：子材质命名+PBR、自定义精细法向表、LOD 层级
// v1 的 20 字节体素记录保持不变（其余字段即 v2 的 base），因此 v1 文件可直接读回。
//
// v3 版本（version=3）在 v2 基础上追加"优化存储"扩展，聚焦体积与加载效率：
//   * 全局 chunk 新增：CRC32 完整性校验、扩展元数据（creator/license/description 键值）
//   * 节内 body 可选"区块化压缩存储"：体素按固定尺寸分块，块内坐标降位为 u8 相对偏移，
//     逐块用 zlib 压缩 → 支持流式/局部加载 + 大幅减小体积
//   * 体素记录追加可选 per-voxel userData 通道（u8，语义由宿主定义）
// v3 读取器完整兼容 v2/v1；v2 读取器遇 v3 的未知 chunk 一律跳过（长度明确，安全）。
// 写入时由调用方选择存储模式（见 EncodeMode）。默认为兼容 v2；请求 v3 区块存储则写 version=3。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <VxlTypes.h>

// vxl2 体素记录：内存表示（v1/v2/v3 通用）。序列化按版本选择编码以减小体积。
struct VxlVoxel2
{
	std::uint32_t x, y, z;      // 坐标（0..size-1）
	std::uint32_t rgba;         // 0xRRGGBBAA，直存
	std::uint16_t normal;       // 0=自动；1..26=标准法向表；>=27=引用节内 customNormals（索引偏移 -27）
	std::uint16_t layer;        // 同格材质层号，0=底层
	std::uint8_t  userData = 0; // 可选 per-voxel 属性通道（v3 区块体素支持，语义由宿主定义；可忽略）
};

// 子材质：按 layer 绑定名称与 PBR 参数（非 PBR 场景仅用 name）
struct VxlMaterial2
{
	std::uint16_t layer;        // 关联的材质层号
	std::string name;           // 图层/子材质名
	float roughness;            // 0..1
	float metalness;            // 0..1
	std::uint32_t emissive;     // 0xRRGGBB，自发光颜色（0=无）
};

// vxl2 section（多实体），body 为线性定长记录流。
// v2 扩展字段：materials / customNormals / lodLevel
// v3 扩展字段：userData 通道（每次体素一个字节，与 voxels 同序，可为空）
struct VxlSection2
{
	std::string name;
	std::uint32_t sizeX, sizeY, sizeZ;
	std::vector<VxlVoxel2> voxels;

	// —— v2 扩展 ——
	std::vector<VxlMaterial2> materials;      // 子材质（layer → 名称 + PBR）
	std::vector<float> customNormals;         // 3N 个 float；normal 索引 27..26+N-1 引用
	std::uint32_t lodLevel = 0;              // LOD 层级，0=最高精度

	// —— v3 扩展 ——
	// 与 voxels 一一对应的 per-voxel 用户属性字节（可为空）。非空时长度须==voxels.size()。
	std::vector<std::uint8_t> userData;
};

// vxl2 全局信息（v2/v3 可选）
struct VxlGlobal2
{
	bool hasGlobal = false;          // 头部是否携带全局 chunk 段
	std::vector<std::uint8_t> paletteRgba; // 可选全局调色板（每项 4 字节 RGBA，直存非索引）
	std::uint8_t upAxis = 'y';       // 轴语义：'y'（Blender）或 'z'（3ds Max）
	float unitScale = 1.f;           // 物理缩放（相对体素单位）
	std::uint8_t thumbFormat = 0;    // 缩略图格式：0=无 1=PNG 2=裸 RGB
	std::vector<std::uint8_t> thumbnail; // 缩略图像素

	// —— v3 扩展 ——
	std::uint32_t crc32 = 0;         // 文件级校验和（对 sections 区段，读取时可重算比对；0=未携带）
	std::vector<std::pair<std::string, std::string>> metadata; // 扩展元数据键值（UTF-8）
};

// 写入时的 body 存储模式
enum class Vxl2EncodeMode
{
	Fixed20        = 0,   // v2 兼容：定长 20 字节记录线性流
	BlockedCompress= 1,   // v3：区块化 + 坐标降位 + 逐块 zlib 压缩（体积更小，支持局部加载）
	// v4：区块化高级优化（version=4）。在 v3 基础上自描述布局，按需启用：
	//   palette(节级量化调色板,色改存 1B 索引)、packedAttr(normal5b+layer3b 打包 1B)、
	//   verticalRuns(同列(z 连续)游程合并且无需重复存字段)、blockCrc(每块 CRC)、
	//   blockIndexTable(块索引目录,支持 mmap 随机定位)。编码器自动权衡启用，读取无损。
	BlockedOptimized = 2,
	// v5：区块化高级优化 II（version=5）。在 v4 基础上新增：
	//   occupancy(稀疏块占用位图:非空格不存坐标,只按序存属性)、
	//   dirRuns(多向游程:x/y/z 三轴 RLE)、dedup(块级内容去重引用)、
	//   sharedPalette(跨 section 共享全局调色板)、multiAlg(zlib/zstd/lz4/raw 按块压缩)、
	//   parallel(多线程并行压缩,结果按序)。读取无损、自动权衡，向后兼容 v1..v4。
	BlockedV5 = 3,
	// v6：超大模型（version=6）。区块化外部索引字段由 v5 的 u16 扩为 u32：
	//   nX/nY/nZ(各轴块数)、blockCount(非空块数)、blockIndex(块索引)、去重目标索引、目录 offset
	//   ⇒ 单轴格数从 ∽209 万(65535块×32) 放宽到 ∽1370 亿(u32块×32)，总非空块数不受 u16 限制。
	//   块内记录/操作码/占用位图/去重/共享调色板/多算法/并行 与 v5 一致；仅外部索引读写作 u32。
	//   读取按 version>=6 分派，向后兼容 v1..v5（旧文件 u16 索引仍按旧布局解析）。
	BlockedV6 = 4,
};

class Vxl2
{
public:
	// 前 4 字节是否为 "VXL2"
	static bool IsVxl2(const std::uint8_t* data, int size);
	// 读取文件主版本（0=非法）
	static std::uint32_t Version(const std::uint8_t* data, int size);

	// 解码 vxl2（v1/v2/v3 自动识别）。失败返回 false（out 清空）。
	// out_global 非空时回填 v2+ 全局信息。
	// verifyCrc 非 true 默认跳过 CRC 重算比对（仅当全局段携带 crc32 且 verifyCrc 时才校验）。
	static bool Decode(const std::uint8_t* data, int size,
		std::vector<VxlSection2>& out_sections, int& out_voxelCount,
		VxlGlobal2* out_global = nullptr, bool verifyCrc = false);

	// 编码 vxl2。版本与 body 存储由 mode 决定：
	//   Fixed20          → 写 version=2（若 global 携带 crc/metadata 也会写入对应 chunk）
	//   BlockedCompress  → 写 version=3（区块化压缩存储）
	// global 非空时写入全局 chunk 段。
	static bool Encode(const std::vector<VxlSection2>& sections,
		std::vector<std::uint8_t>& out_data,
		const VxlGlobal2* global = nullptr,
		Vxl2EncodeMode mode = Vxl2EncodeMode::Fixed20);

	// ---- v1 ↔ v2/v3 投影换算 ----

	// 把 RB2 section 投影为 vxl2（单层：每格 layer=0，RGBA 取调色板，normal 原样）。
	// palette768 : 768 字节 RGB（需与 v1 的颜色索引对应）。
	static bool FromVxl(const std::vector<VxlSection>& v1,
		const std::vector<std::uint8_t>& palette768,
		std::vector<VxlSection2>& v2);

	// 把 vxl2 投影回 RB2 section（降级）：
	//   每格取 layer 最小的记录；尺寸>255 或颜色无法映射时报错（strict）。
	// palette768 : 供 RGBA → 最近色索引 使用的目标调色板。
	static bool ToVxl(const std::vector<VxlSection2>& v2,
		const std::vector<std::uint8_t>& palette768,
		bool strict,
		std::vector<VxlSection>& v1);
};