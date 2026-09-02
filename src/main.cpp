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
#include <ObjLoader.h>
#include <PngWriter.h>
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
		"  vxltool render <file.vxl> [--hva <file.hva>] -o <out.png> [--size N] [--yaw A] [--pitch A] [--frame N]\n"
		"\n"
		"  --mode N : normalsMode（1..4，默认 4 = RA2）\n"
		"  --size N : 体素网格最长轴边长（4..256，import 默认 64）\n"
		"  --color N: import 固定调色板颜色索引（默认 0 = 按高度渐变）\n"
		"  --up y|z : OBJ 的上轴（y = Blender 默认，z = 3ds Max 默认）\n"
		"  import 将 OBJ 三角网格体素化为 VXL（射线投射法，支持顶点色）\n"
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
static int CmdImport(const std::string& objPath, const std::string& outPath,
	int gridSize, int baseColor, char upAxis, int mode)
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
	if (!VxlEncoder::Encode(sections, palette, "import.vxl", vxl))
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
	const std::string& outPath, int size, float yaw, float pitch, int frame)
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

	// 调色板：优先用 VXL 内嵌，否则用军事调色板
	std::vector<std::uint8_t> palette(768, 0);
	if (!VxlDecoder::GetPalette(data.data(), (int)data.size(), palette.data()))
		BuildMilitaryPalette(palette);

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

	// 解析 --mode N 与 --hva <path>
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
		return CmdImport(argv[2], outPath, gridSize, baseColor, upAxis, mode);
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
		return CmdRender(argv[2], hvaPath, outPath, size, yaw, pitch, frame);
	}

	PrintUsage();
	return 1;
}
