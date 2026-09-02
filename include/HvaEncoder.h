#pragma once

// HVA（Hierarchical Voxel Animation）体素动画编码器（Red Alert 2 / Yuri's Revenge）。
// 与 gamemd 工程的 HvaDecoder 互为逆操作，格式对齐社区 modding 指南与 ra2ff：
//
//   Header(24): fileName(16) + frameCount(4) + sectionCount(4)
//   SectionNames: sectionCount × 16 字节（必须与 VXL section 名完全一致）
//   Matrices: frameCount × sectionCount × 48 字节（每个 3×4 行主序矩阵，12 float）
//
// 矩阵布局（行主序）:
//   [ ScaleX, RotXY, RotXZ, TransX ]
//   [ RotYX, ScaleY, RotYZ, TransY ]
//   [ RotZX, RotZY, ScaleZ, TransZ ]
//
// 静态模型用单位矩阵。平移列以 hvaMultiplier 为单位：hvaMultiplier=0.0833(=1/12)
// 时，模型空间 1 体素 = HVA 平移 12 单位（渲染器按 hm[3]×hvaMultiplier 折算）。

#include <cstdint>
#include <string>
#include <vector>

class HvaEncoder
{
public:
	// 将 section 名 + 每帧每 section 的 3×4 变换矩阵编码为 HVA 二进制。
	//   sectionNames : section 名列表（顺序须与 VXL 一致，最长 16 字符，不足补 0）
	//   frameCount   : 帧数（≥1）
	//   transforms   : 扁平存储，索引 [frame * sectionCount + section] × 12 float
	//   fileName     : 写入文件头（最长 16 字符，超长截断，不足补 0）
	//   out_data     : 输出的 HVA 二进制
	// 返回是否成功。失败时 out_data 清空。
	static bool Encode(const std::vector<std::string>& sectionNames,
		int frameCount,
		const std::vector<float>& transforms,
		const std::string& fileName,
		std::vector<std::uint8_t>& out_data);

	// 单位矩阵（12 float，行主序）
	static void Identity(float* m);

	// 平移矩阵：单位旋转 + 平移（tx/ty/tz 为 HVA 单位；
	// 模型空间体素数 × (1/hvaMultiplier) 即得，如 hvaMultiplier=1/12 时 ×12）。
	static void Translate(float* m, float tx, float ty, float tz);
};
