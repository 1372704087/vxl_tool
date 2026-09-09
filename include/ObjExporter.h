#pragma once

// VXL → OBJ 导出器：把解码后的体素模型写为 Wavefront OBJ 文本（z-up）。
// 每个 solid 体素与空气(或越界)相邻的外露面都会被输出为一个单位正方形
// （2 个三角形），并按体素调色板索引写入顶点色 `v x y z r g b`。
// 坐标一律为整数（网格格点 × scale），因此不依赖运行时 locale 的小数点。
//
// 供 round-trip 使用：`export` 输出的 OBJ 可再被 `import --up z` 体素化回 VXL，
// 体素数应与原文件基本一致。

#include <cstdint>
#include <string>
#include <vector>

#include <VxlTypes.h>

class ObjExporter
{
public:
	// 将一个或多个 section 导出为 OBJ 文本。
	//   sections : VxlDecoder::Decode 的结果
	//   palette  : 768 字节 RGB（按体素 colorIndex 取色）
	//   scale    : 整数单位缩放（默认 1，即一个格子 = 1 单位）
	//   out      : 目标 OBJ 文本
	// 成功返回 true。任何 section 无体素时仍返回 true（输出空组但不崩溃）。
	static bool ExportObjText(const std::vector<VxlSection>& sections,
		const std::vector<std::uint8_t>& palette,
		int scale,
		std::string& out);
};