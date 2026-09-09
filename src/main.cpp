// VXL 工具命令行入口
//   用法:
//     vxltool generate <cube|tank|tank2|building|aircraft|ship|heli> -o <out.vxl> [--hva <out.hva>] [--mode N]
//     vxltool verify <file.vxl> [--hva <file.hva>]
//     vxltool roundtrip <cube|tank|tank2|building|aircraft|ship|heli> [--mode N]
//     vxltool render <file.vxl> [--hva <file.hva>] -o <out.png> [--size N] [--yaw A] [--pitch A] [--frame N]
//
//   --mode N : normalsMode（1..4，默认 4 = RA2）
//   tank2 为多 section 模型（Body/turret/barrel），生成时建议同时输出 HVA 定位文件。
//   heli  为精细多 section 攻击直升机（Body/MainRotor/TailRotor），HVA 含 8 帧旋翼旋转动画，
//          render 时可用 --frame 0..7 选择动画帧。

#include <HvaDecoder.h>
#include <HvaEncoder.h>
#include <ObjExporter.h>
#include <ObjLoader.h>
#include <Palette.h>
#include <PngWriter.h>
#include <Vxl2.h>
#include <VxlDecoder.h>
#include <VxlEncoder.h>
#include <VoxelRenderer.h>
#include <Voxelizer.h>
#include <generators.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
// Window 控制台默认使用 GBK 代码页，而本程序以 UTF-8 输出中文，
// 需把控制台输出代码页切换为 UTF-8 才能正常显示（仅平台层，不改后端逻辑）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void SetupUtf8Console()
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
}
#else
static void SetupUtf8Console() {}
#endif

static void PrintUsage()
{
	std::printf(
		"VXL 体素模型工具\n"
		"用法:\n"
		"  vxltool import <file.obj> -o <out.vxl> [--size N] [--color N] [--up y|z] [--mode N]\n"
		"  vxltool verify <file.vxl> [--hva <file.hva>]\n"
		"  vxltool info <file.vxl|file.vxl2>     （自动识别 v1/v2 并打印 section 信息）\n"
		"  vxltool vxl2 <in> -o <out>            （v1 ↔ v2 双向转换；超 255 轴写 v1 时需 --force）\n"
		"  vxltool export <file.vxl> -o <out.obj> [--scale N]\n"
		"  vxltool render <file.vxl> [--hva <file.hva>] -o <out.png> [--size N] [--yaw A] [--pitch A] [--frame N]\n"
		"\n"
		"  --mode N    : normalsMode（1..4，默认 4 = RA2）\n"
		"  --size N    : 体素网格最长轴边长（4..256，import 默认 64）\n"
		"  --color N   : import 固定调色板颜色索引（默认 0 = 按高度渐变）\n"
		"  --up y|z    : OBJ 的上轴（y = Blender 默认，z = 3ds Max 默认）\n"
		"  --palette P : 自定义调色板文件（768 字节 .pal 或文本 INI），import/export/render/vxl2 可用\n"
		"  vxl2 扩展: --axis y|z（上轴） --scale F（物理缩放） --lod N（LOD 层级） --thumb <img>（内嵌缩略图）\n"
		"           --v3|--compress（区块化压缩存储，写 version=3,体积更小）\n"
		"           --v4|--optimize（高级区块体，写 version=4：调色板+位域打包+纵向游程+逐块CRC+索引目录）\n"
		"           --v5|--ultra（最省存储，写 version=5：稀疏占用位图+多向游程+块级去重+跨节共享调色板+并行压缩+zstd/lz4）\n"
		"           --v6（超大模型，写 version=6：v5 全部特性 + 索引 u16→u32，支持 >65535 块 / 任意大网格）\n"
		"           --crc（文件级 CRC32 校验）\n"
		"           --meta k=v（扩展元数据键值，可多次使用；也可用 --kv k:v）\n"
		"  import 将 OBJ 三角网格体素化为 VXL（射线投射法，支持顶点色）\n"
		"  export 将 VXL 解码并写成 OBJ（z-up，单位立方体表面，带顶点色）\n"
		"  render 将模型软件渲染为 PNG（默认 512px，yaw=-0.6，pitch=-0.45，frame=0）\n");
}

static bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& data)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
	{
		std::printf("错误: 无法写入 %s\n", path.c_str());
		return false;
	}
	size_t written = std::fwrite(data.data(), 1, data.size(), f);
	std::fclose(f);
	return written == data.size();
}

static bool ReadFile(const std::string& path, std::vector<std::uint8_t>& data)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		std::printf("错误: 无法打开 %s\n", path.c_str());
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (sz <= 0)
	{
		std::fclose(f);
		return false;
	}
	data.resize((size_t)sz);
	size_t rd = std::fread(data.data(), 1, (size_t)sz, f);
	std::fclose(f);
	return rd == (size_t)sz;
}

// 导入 OBJ 网格 → 体素化 → VXL
// palettePath 非空时：加载外部自定义调色板（768 字节 .pal 或文本 INI）覆盖默认调色板。
static bool ApplyPaletteArg(const std::string& palettePath, std::vector<std::uint8_t>& pal)
{
	if (palettePath.empty())
		return true;
	std::vector<std::uint8_t> p(768, 0);
	if (!PaletteLoader::Load(palettePath, p.data()))
	{
		std::printf("错误: 无法加载调色板文件（%s）\n", palettePath.c_str());
		return false;
	}
	pal = std::move(p);
	return true;
}

static int CmdImport(const std::string& objPath, const std::string& outPath,
	int gridSize, int baseColor, char upAxis, int mode, const std::string& palettePath)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(objPath, data))
		return 1;
	std::string text((const char*)data.data(), data.size());

	ObjMesh mesh;
	if (!ObjLoader::Load(text, mesh))
	{
		std::printf("错误: 解析 OBJ 失败（%s 不是有效的 OBJ 文件）\n", objPath.c_str());
		return 1;
	}

	std::vector<std::uint8_t> palette;
	BuildMilitaryPalette(palette);
	if (!ApplyPaletteArg(palettePath, palette))
		return 1;

	std::vector<VxlVoxel> voxels;
	int size[3] = { 0, 0, 0 };
	if (!Voxelizer::Voxelize(mesh, gridSize, baseColor, upAxis, palette, voxels, size))
	{
		std::printf("错误: 体素化失败\n");
		return 1;
	}
	if (voxels.empty())
	{
		std::printf("错误: 体素化为空（网格可能未闭合、面朝向不一致或尺寸过小）\n");
		return 1;
	}

	VxlSection sec;
	if (!BuildSectionFromVoxels("Body", voxels, size[0], size[1], size[2], mode, sec))
	{
		std::printf("错误: 构建 section 失败\n");
		return 1;
	}

	std::vector<VxlSection> sections;
	sections.push_back(std::move(sec));

	std::vector<std::uint8_t> vxl;
	if (!VxlEncoder::Encode(sections, palette, vxl))
	{
		std::printf("错误: 编码失败\n");
		return 1;
	}
	if (!WriteFile(outPath, vxl))
		return 1;

	std::printf("已导入 %s -> %s\n", objPath.c_str(), outPath.c_str());
	std::printf("  网格: %zu 顶点, %zu 面, 顶点色: %s\n",
		mesh.positions.size() / 3, mesh.faces.size(),
		mesh.hasVertexColors ? "有" : "无");
	std::printf("  体素: %zu, 网格尺寸 %dx%dx%d, normalsMode: %d, 文件: %zu 字节\n",
		voxels.size(), size[0], size[1], size[2], mode, vxl.size());
	return 0;
}

static int CmdVerify(const std::string& path, const std::string& hvaPath)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(path, data))
		return 1;

	std::vector<VxlSection> sections;
	int voxelCount = 0;
	if (!VxlDecoder::Decode(data.data(), (int)data.size(), sections, voxelCount))
	{
		std::printf("错误: 解码失败（%s 不是有效的 VXL 文件）\n", path.c_str());
		return 1;
	}

	std::printf("验证通过: %s\n", path.c_str());
	std::printf("  文件: %zu 字节, section: %zu, 体素: %d\n",
		data.size(), sections.size(), voxelCount);
	for (size_t i = 0; i < sections.size(); ++i)
	{
		const auto& s = sections[i];
		std::printf("  [%zu] '%s' 尺寸 %dx%dx%d normalsMode=%d hvaMultiplier=%.4f\n",
			i, s.name.c_str(), s.sizeX, s.sizeY, s.sizeZ, s.normalsMode, s.hvaMultiplier);
	}

	std::vector<std::uint8_t> pal(768);
	if (VxlDecoder::GetPalette(data.data(), (int)data.size(), pal.data()))
	{
		std::printf("  调色板: 内嵌 768 字节 RGB（索引 0=%d,%d,%d）\n",
			pal[0], pal[1], pal[2]);
	}

	// 可选：验证配套 HVA
	if (!hvaPath.empty())
	{
		std::vector<std::uint8_t> hvaData;
		if (!ReadFile(hvaPath, hvaData))
			return 1;

		int frameCount = 0, hvaSectionCount = 0;
		std::vector<HvaSectionInfo> hvaSections;
		std::vector<float> hvaTransforms;
		if (!HvaDecoder::Decode(hvaData.data(), (int)hvaData.size(),
			frameCount, hvaSectionCount, hvaSections, hvaTransforms))
		{
			std::printf("错误: HVA 解码失败（%s 不是有效的 HVA 文件）\n", hvaPath.c_str());
			return 1;
		}

		std::printf("HVA 验证通过: %s\n", hvaPath.c_str());
		std::printf("  文件: %zu 字节, %d 帧, %d section\n", hvaData.size(), frameCount, hvaSectionCount);
		for (int i = 0; i < hvaSectionCount; ++i)
		{
			const float* m = &hvaTransforms[(size_t)i * 12];
			std::printf("    [%d] '%s' 平移=(%.1f, %.1f, %.1f)\n",
				i, hvaSections[i].name.c_str(), m[3], m[7], m[11]);
		}

		// 检查 section 名与 VXL 是否一致
		if ((int)sections.size() != hvaSectionCount)
		{
			std::printf("  警告: VXL section 数(%zu) 与 HVA section 数(%d) 不一致\n",
				sections.size(), hvaSectionCount);
		}
		else
		{
			for (int i = 0; i < hvaSectionCount; ++i)
			{
				if (sections[i].name != hvaSections[i].name)
					std::printf("  警告: section 名不一致 VXL[%d]='%s' HVA[%d]='%s'\n",
						i, sections[i].name.c_str(), i, hvaSections[i].name.c_str());
			}
		}
	}
	return 0;
}

// 渲染 VXL（可选 HVA）为 PNG
static int CmdRender(const std::string& path, const std::string& hvaPath,
	const std::string& outPath, int size, float yaw, float pitch, int frame,
	const std::string& palettePath)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(path, data))
		return 1;

	// vxl2（RGBA 直存）渲染路径
	if (Vxl2::IsVxl2(data.data(), (int)data.size()))
	{
		std::vector<VxlSection2> secs;
		int vc = 0;
		VxlGlobal2 global;
		if (!Vxl2::Decode(data.data(), (int)data.size(), secs, vc, &global) || secs.empty())
		{
			std::printf("错误: 解码失败（%s 不是有效的 VXL2 文件）\n", path.c_str());
			return 1;
		}
		RenderOptions opt;
		opt.imageSize = size;
		opt.yaw = yaw;
		opt.pitch = pitch;
		std::vector<std::uint8_t> rgba;
		if (!VoxelRenderer::RenderVxl2(secs, &global, opt, rgba))
		{
			std::printf("错误: 渲染失败\n");
			return 1;
		}
		std::vector<std::uint8_t> rgb;
		rgb.reserve((size_t)size * size * 3);
		for (int i = 0; i < size * size; ++i)
		{
			rgb.push_back(rgba[i * 4 + 0]);
			rgb.push_back(rgba[i * 4 + 1]);
			rgb.push_back(rgba[i * 4 + 2]);
		}
		std::vector<std::uint8_t> png;
		if (!PngWriter::WriteRGB(size, size, rgb, png) || !WriteFile(outPath, png))
		{
			std::printf("错误: PNG 写出失败\n");
			return 1;
		}
		std::printf("已渲染（VXL2） %s -> %s\n", path.c_str(), outPath.c_str());
		std::printf("  %dpx, section: %zu, 体素: %d, PNG: %zu 字节\n",
			size, secs.size(), vc, png.size());
		return 0;
	}

	std::vector<VxlSection> sections;
	int voxelCount = 0;
	if (!VxlDecoder::Decode(data.data(), (int)data.size(), sections, voxelCount) || sections.empty())
	{
		std::printf("错误: 解码失败（%s 不是有效的 VXL 文件）\n", path.c_str());
		return 1;
	}

	// 调色板：优先用 VXL 内嵌，否则用军事调色板；--palette 可覆盖
	std::vector<std::uint8_t> palette(768, 0);
	if (!VxlDecoder::GetPalette(data.data(), (int)data.size(), palette.data()))
		BuildMilitaryPalette(palette);
	if (!ApplyPaletteArg(palettePath, palette))
		return 1;

	// 可选 HVA
	std::vector<float> hvaTransforms;
	int hvaSectionCount = 0;
	if (!hvaPath.empty())
	{
		std::vector<std::uint8_t> hvaData;
		if (!ReadFile(hvaPath, hvaData))
			return 1;
		int frameCount = 0;
		std::vector<HvaSectionInfo> hvaSections;
		if (!HvaDecoder::Decode(hvaData.data(), (int)hvaData.size(),
			frameCount, hvaSectionCount, hvaSections, hvaTransforms))
		{
			std::printf("错误: HVA 解码失败（%s 不是有效的 HVA 文件）\n", hvaPath.c_str());
			return 1;
		}
	}

	RenderOptions opt;
	opt.imageSize = size;
	opt.yaw = yaw;
	opt.pitch = pitch;

	std::vector<std::uint8_t> rgba;
	if (!VoxelRenderer::Render(sections, palette, hvaTransforms, hvaSectionCount, frame, opt, rgba))
	{
		std::printf("错误: 渲染失败\n");
		return 1;
	}

	// RGBA → RGB
	std::vector<std::uint8_t> rgb;
	rgb.reserve((size_t)size * size * 3);
	for (int i = 0; i < size * size; ++i)
	{
		rgb.push_back(rgba[i * 4 + 0]);
		rgb.push_back(rgba[i * 4 + 1]);
		rgb.push_back(rgba[i * 4 + 2]);
	}

	std::vector<std::uint8_t> png;
	if (!PngWriter::WriteRGB(size, size, rgb, png))
	{
		std::printf("错误: PNG 编码失败\n");
		return 1;
	}
	if (!WriteFile(outPath, png))
		return 1;

	std::printf("已渲染 %s -> %s\n", path.c_str(), outPath.c_str());
	std::printf("  %dpx, section: %zu, 体素: %d, PNG: %zu 字节%s\n",
		size, sections.size(), voxelCount, png.size(),
		hvaSectionCount > 0 ? "（含 HVA 定位）" : "");
	return 0;
}

// 打印模型信息（自动识别 v1/v2，等价于 verify 的信息输出）
static int CmdInfo(const std::string& path)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(path, data))
		return 1;

	if (Vxl2::IsVxl2(data.data(), (int)data.size()))
	{
		std::uint32_t ver = Vxl2::Version(data.data(), (int)data.size());
		std::vector<VxlSection2> secs;
		int vc = 0;
		VxlGlobal2 global;
		if (!Vxl2::Decode(data.data(), (int)data.size(), secs, vc, &global))
		{
			std::printf("错误: 解码失败（%s 不是有效的 VXL2 文件）\n", path.c_str());
			return 1;
		}
		std::printf("格式: VXL2 (version=%u), section: %zu, 体素: %d\n",
			(unsigned)ver, secs.size(), vc);
		if (global.hasGlobal)
		{
			std::printf("  全局: 上轴=%c 缩放=%.4g%s%s\n",
				global.upAxis ? global.upAxis : '?', global.unitScale,
				global.paletteRgba.empty() ? "" : (" 调色板=" + std::to_string(global.paletteRgba.size() / 4)).c_str(),
				!global.thumbnail.empty() ? " 缩略图=有" : "");
		}
		int i = 0;
		for (const VxlSection2& s : secs)
		{
			std::printf("  [%d] %s: %ux%ux%u, 体素: %zu%s%s\n",
				i++, s.name.c_str(), (unsigned)s.sizeX, (unsigned)s.sizeY, (unsigned)s.sizeZ, s.voxels.size(),
				s.lodLevel ? (" LOD=" + std::to_string(s.lodLevel)).c_str() : "",
				!s.materials.empty() ? (" 材质=" + std::to_string(s.materials.size())).c_str() : "");
			if (!s.materials.empty())
			{
				for (const auto& m : s.materials)
					std::printf("      layer %u 材质 '%s' rough=%.2f metal=%.2f emissive=0x%06X\n",
						(unsigned)m.layer, m.name.c_str(), m.roughness, m.metalness, m.emissive);
			}
			if (!s.customNormals.empty())
				std::printf("      自定义法向: %u 条\n", (unsigned)(s.customNormals.size() / 3));
		}
		return 0;
	}

	std::vector<VxlSection> secs;
	int vc = 0;
	if (!VxlDecoder::Decode(data.data(), (int)data.size(), secs, vc) || secs.empty())
	{
		std::printf("错误: 解码失败（%s 不是有效的 VXL 文件）\n", path.c_str());
		return 1;
	}
	std::printf("格式: VXL, section: %zu, 体素: %d\n", secs.size(), vc);
	int i = 0;
	for (const VxlSection& s : secs)
		std::printf("  [%d] %s: %dx%dx%d, normalsMode=%d\n",
			i++, s.name.c_str(), s.sizeX, s.sizeY, s.sizeZ, s.normalsMode);
	return 0;
}

// vxl2 转换的可选扩展参数
struct Vxl2Opt
{
	char axis = 0;          // 上轴 'y'/'z'；0=默认 y
	float scale = 0.f;      // 物理缩放；0=默认 1
	unsigned lod = 0;       // LOD 层级
	std::string thumb;      // 内嵌缩略图文件路径（整文件字节按 PNG 约定内嵌）
	// ———— v3 扩展 ————
	bool compress = false; // 区块化压缩存储（写 version=3，体积更小、支持局部加载）
	// ———— v4 扩展 ————
	bool optimize = false; // 高级区块体（写 version=4，调色板+打包+游程+逐块CRC+索引目录，更省）
	// ———— v5 扩展 ————
	bool v5 = false;       // 最省存储（写 version=5：稀疏占用位图+多向游程+块级去重+共享调色板+并行+zstd/lz4）
	// ———— v6 扩展 ————
	bool v6 = false;       // 超大模型（写 version=6：v5 全部特性 + 外部索引字段 u16→u32，突破 65535 块上限）
	bool crc = false;       // 文件级 CRC32 校验和
	std::vector<std::pair<std::string, std::string>> metadata; // 扩展元数据键值（--meta k=v）
};

// v1 ↔ v2 双向转换
static int CmdVxl2(const std::string& inPath, const std::string& outPath, bool forceOver256,
	const std::string& palettePath, const Vxl2Opt& opt)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(inPath, data))
		return 1;

	std::vector<std::uint8_t> palette(768, 0);
	BuildMilitaryPalette(palette);
	if (!ApplyPaletteArg(palettePath, palette))
		return 1;

	if (Vxl2::IsVxl2(data.data(), (int)data.size()))
	{
		// vxl2 → v1（投影降级）
		std::vector<VxlSection2> s2;
		int vc = 0;
		if (!Vxl2::Decode(data.data(), (int)data.size(), s2, vc))
		{
			std::printf("错误: 解码 VXL2 失败（%s）\n", inPath.c_str());
			return 1;
		}
		std::vector<VxlSection> s1;
		if (!Vxl2::ToVxl(s2, palette, !forceOver256, s1))
		{
			std::printf("错误: 投影回 VXL 失败（%s 存在超 255 轴；如需钳位请加 --force）\n", inPath.c_str());
			return 1;
		}
		std::vector<std::uint8_t> v1data;
		if (!VxlEncoder::Encode(s1, palette, v1data) || !WriteFile(outPath, v1data))
		{
			std::printf("错误: 写出 VXL 失败（%s）\n", outPath.c_str());
			return 1;
		}
		std::printf("已转换（vxl2 -> vxl）: %s -> %s, %d 体素%s\n",
			inPath.c_str(), outPath.c_str(), vc, forceOver256 ? "（已钳位 255）" : "");
		return 0;
	}

	// v1 → vxl2
	std::vector<VxlSection> s1;
	int vc = 0;
	if (!VxlDecoder::Decode(data.data(), (int)data.size(), s1, vc) || s1.empty())
	{
		std::printf("错误: 解码 VXL 失败（%s）\n", inPath.c_str());
		return 1;
	}
	if (!VxlDecoder::GetPalette(data.data(), (int)data.size(), palette.data()))
		BuildMilitaryPalette(palette);
	if (!ApplyPaletteArg(palettePath, palette))
		return 1;

	std::vector<VxlSection2> s2;
	if (!Vxl2::FromVxl(s1, palette, s2))
	{
		std::printf("错误: 投影到 VXL2 失败\n");
		return 1;
	}

	// 应用可选的 v2 扩展：LOD 层级（作用于所有 section）
	if (opt.lod)
		for (auto& sec : s2)
			sec.lodLevel = opt.lod;

	// 内嵌缩略图（按 PNG 约定整文件字节内嵌）
	std::vector<std::uint8_t> thumbBytes;
	if (!opt.thumb.empty())
	{
		std::vector<std::uint8_t> raw;
		if (!ReadFile(opt.thumb, raw) || raw.empty())
		{
			std::printf("错误: 无法读取缩略图文件（%s）\n", opt.thumb.c_str());
			return 1;
		}
		std::swap(thumbBytes, raw);
	}

	// 全局扩展：上轴 / 缩放 / 缩略图 / v3 CRC / 元数据
	VxlGlobal2 global;
	bool hasGlobal = opt.axis || opt.scale > 0.f || !thumbBytes.empty() || opt.crc || !opt.metadata.empty();
	Vxl2EncodeMode mode = opt.v6 ? Vxl2EncodeMode::BlockedV6
		: opt.v5 ? Vxl2EncodeMode::BlockedV5
		: opt.optimize ? Vxl2EncodeMode::BlockedOptimized
		: opt.compress ? Vxl2EncodeMode::BlockedCompress
		: Vxl2EncodeMode::Fixed20;
	if (hasGlobal)
	{
		global.hasGlobal = true;
		if (opt.axis == 'y' || opt.axis == 'z') global.upAxis = opt.axis;
		if (opt.scale > 0.f) global.unitScale = opt.scale;
		if (!thumbBytes.empty())
		{
			global.thumbFormat = 1;
			global.thumbnail = thumbBytes;
		}
		if (opt.crc) global.crc32 = 1;              // 非 0 即触发写入并回填真实 CRC
		global.metadata = opt.metadata;
	}

	std::vector<std::uint8_t> v2data;
	if (!Vxl2::Encode(s2, v2data, hasGlobal ? &global : nullptr, mode) || !WriteFile(outPath, v2data))
	{
		std::printf("错误: 写出 VXL2 失败（%s）\n", outPath.c_str());
		return 1;
	}
	std::printf("已转换（vxl -> vxl2）: %s -> %s, %d 体素%s%s%s\n",
		inPath.c_str(), outPath.c_str(), vc,
		hasGlobal ? "（含全局元数据）" : "",
		opt.v5 ? "（v5 最省存储）" : opt.optimize ? "（v4 高级区块体）" : opt.compress ? "（v3 区块压缩）" : "",
		opt.lod ? ("（LOD=" + std::to_string(opt.lod) + "）").c_str() : "");
	return 0;
}

// 导出 VXL → OBJ
static int CmdExport(const std::string& path, const std::string& outPath, int scale,
	const std::string& palettePath)
{
	std::vector<std::uint8_t> data;
	if (!ReadFile(path, data))
		return 1;

	std::vector<VxlSection> sections;
	int voxelCount = 0;
	if (!VxlDecoder::Decode(data.data(), (int)data.size(), sections, voxelCount) || sections.empty())
	{
		std::printf("错误: 解码失败（%s 不是有效的 VXL 文件）\n", path.c_str());
		return 1;
	}

	std::vector<std::uint8_t> palette(768, 0);
	if (!VxlDecoder::GetPalette(data.data(), (int)data.size(), palette.data()))
		BuildMilitaryPalette(palette);
	if (!ApplyPaletteArg(palettePath, palette))
		return 1;

	std::string objText;
	if (!ObjExporter::ExportObjText(sections, palette, scale, objText))
	{
		std::printf("错误: 导出 OBJ 失败\n");
		return 1;
	}

	std::vector<std::uint8_t> outData(objText.begin(), objText.end());
	if (!WriteFile(outPath, outData))
		return 1;

	std::printf("已导出 %s -> %s\n", path.c_str(), outPath.c_str());
	std::printf("  section: %zu, 体素: %d, scale: %d, OBJ: %zu 字节\n",
		sections.size(), voxelCount, scale, outData.size());
	return 0;
}

int main(int argc, char** argv)
{
	SetupUtf8Console();

	if (argc < 2)
	{
		PrintUsage();
		return 1;
	}

	std::string cmd = argv[1];
	int mode = 4;
	std::string hvaPath;
	std::string palettePath;

	// 解析 --mode N、--hva <path> 与 --palette <path>
	for (int i = 2; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			mode = std::atoi(argv[i + 1]);
			if (mode < 1) mode = 1;
			if (mode > 4) mode = 4;
		}
		else if (std::strcmp(argv[i], "--hva") == 0 && i + 1 < argc)
		{
			hvaPath = argv[i + 1];
		}
		else if (std::strcmp(argv[i], "--palette") == 0 && i + 1 < argc)
		{
			palettePath = argv[i + 1];
		}
	}

	if (cmd == "import")
	{
		if (argc < 5)
		{
			PrintUsage();
			return 1;
		}
		std::string outPath;
		int gridSize = 64;
		int baseColor = 0;
		char upAxis = 'y';
		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
				outPath = argv[i + 1];
			else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
				gridSize = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--color") == 0 && i + 1 < argc)
				baseColor = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--up") == 0 && i + 1 < argc)
				upAxis = argv[i + 1][0];
		}
		if (outPath.empty())
		{
			PrintUsage();
			return 1;
		}
		if (gridSize < 4) gridSize = 4;
		if (gridSize > 256) gridSize = 256;
		if (upAxis != 'y' && upAxis != 'z')
			upAxis = 'y';
		return CmdImport(argv[2], outPath, gridSize, baseColor, upAxis, mode, palettePath);
	}
	else if (cmd == "verify")
	{
		if (argc < 3)
		{
			PrintUsage();
			return 1;
		}
		return CmdVerify(argv[2], hvaPath);
	}
	else if (cmd == "info")
	{
		if (argc < 3)
		{
			PrintUsage();
			return 1;
		}
		return CmdInfo(argv[2]);
	}
	else if (cmd == "vxl2")
	{
		if (argc < 3)
		{
			PrintUsage();
			return 1;
		}
		std::string outPath;
		bool force = false;
		Vxl2Opt opt;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
				outPath = argv[i + 1];
			else if (std::strcmp(argv[i], "--force") == 0)
				force = true;
			else if (std::strcmp(argv[i], "--axis") == 0 && i + 1 < argc)
				opt.axis = argv[i + 1][0];
			else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
				opt.scale = (float)std::atof(argv[i + 1]);
			else if (std::strcmp(argv[i], "--lod") == 0 && i + 1 < argc)
				opt.lod = (unsigned)std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--thumb") == 0 && i + 1 < argc)
				opt.thumb = argv[i + 1];
			else if (std::strcmp(argv[i], "--v3") == 0 || std::strcmp(argv[i], "--compress") == 0)
				opt.compress = true;
			else if (std::strcmp(argv[i], "--v4") == 0 || std::strcmp(argv[i], "--optimize") == 0)
				opt.optimize = true;
			else if (std::strcmp(argv[i], "--v5") == 0 || std::strcmp(argv[i], "--ultra") == 0)
				opt.v5 = true;
			else if (std::strcmp(argv[i], "--v6") == 0)
				opt.v6 = true;
			else if (std::strcmp(argv[i], "--crc") == 0)
				opt.crc = true;
			else if ((std::strcmp(argv[i], "--meta") == 0 || std::strcmp(argv[i], "--kv") == 0)
				&& i + 1 < argc)
			{
				// 支持 "k=v"、"k: v"、"k = v" 分隔
				std::string kv = argv[i + 1];
				std::size_t sep = kv.find_first_of("=:");
				if (sep != std::string::npos && sep > 0)
				{
					std::string k = kv.substr(0, sep);
					std::string v = kv.substr(sep + 1);
					auto trimL = [](std::string& s)
					{
						size_t b = s.find_first_not_of(" \t");
						s = (b == std::string::npos) ? "" : s.substr(b);
						size_t e = s.find_last_not_of(" \t");
						s = (e == std::string::npos) ? "" : s.substr(0, e + 1);
					};
					trimL(k); trimL(v);
					if (!k.empty()) opt.metadata.emplace_back(std::move(k), std::move(v));
				}
			}
		}
		if (outPath.empty())
		{
			PrintUsage();
			return 1;
		}
		return CmdVxl2(argv[2], outPath, force, palettePath, opt);
	}
	else if (cmd == "export")
	{
		if (argc < 3)
		{
			PrintUsage();
			return 1;
		}
		std::string outPath;
		int scale = 1;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
				outPath = argv[i + 1];
			else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
				scale = std::atoi(argv[i + 1]);
		}
		if (outPath.empty())
		{
			PrintUsage();
			return 1;
		}
		if (scale < 1) scale = 1;
		if (scale > 1000) scale = 1000;
		return CmdExport(argv[2], outPath, scale, palettePath);
	}
	else if (cmd == "render")
	{
		if (argc < 5)
		{
			PrintUsage();
			return 1;
		}
		std::string outPath;
		int size = 512;
		int frame = 0;
		float yaw = -0.6f, pitch = -0.45f;
		for (int i = 3; i < argc; ++i)
		{
			if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc)
				outPath = argv[i + 1];
			else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
				size = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--yaw") == 0 && i + 1 < argc)
				yaw = (float)std::atof(argv[i + 1]);
			else if (std::strcmp(argv[i], "--pitch") == 0 && i + 1 < argc)
				pitch = (float)std::atof(argv[i + 1]);
			else if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc)
				frame = std::atoi(argv[i + 1]);
		}
		if (outPath.empty())
		{
			PrintUsage();
			return 1;
		}
		if (size < 64) size = 64;
		if (size > 2048) size = 2048;
		if (frame < 0) frame = 0;
		return CmdRender(argv[2], hvaPath, outPath, size, yaw, pitch, frame, palettePath);
	}

	PrintUsage();
	return 1;
}
