#pragma once

// VXL 体素模型编码器（Red Alert 2 / Yuri's Revenge 的车辆/建筑 3D 模型写出）。
// 与 gamemd 工程的 VxlDecoder 互为逆操作，格式对齐 ra2ff.txt 与社区 modding 指南。
//
// VXL 文件结构（写出时按此布局）：
//   [Header 802 字节]
//     fileType[16]="Voxel Animation" + u32(1) + u32(section数) + u32(section数)
//     + u32(bodySize) + remapStart(1)=16 + remapEnd(1)=31 + palette[768]
//   [Section 头 N×28 字节]  name[16] + 3×u32
//   [Body bodySize 字节]    每 section：spanStart 表 + spanEnd 表 + span 数据
//   [Tailer N×92 字节]      spanStartOffset/spanEndOffset/spanDataOffset + scale
//                           + transform[12] + minBounds[3] + maxBounds[3]
//                           + sizeX + sizeY + sizeZ + normalsMode
//
// Span 数据（每格一列）：
//   段: [skip][count][color normal]×count [count(重复)]
//   结尾: [剩余Z][0]
//   空格: spanStart=spanEnd=-1

#include <cstdint>
#include <string>
#include <vector>

#include <VxlTypes.h>

class VxlEncoder
{
public:
	// 将 sections 编码为 VXL 二进制数据。
	//   sections : 一个或多个 section（每 section 需有 sizeX/Y/Z、spans、normalsMode）
	//   palette  : 768 字节 RGB 调色板（256 色）
	//   fileName : 写入文件头（最长 16 字符，超长截断，不足补 0）
	//   out_data : 输出的 VXL 二进制
	// 返回是否成功。失败时 out_data 清空。
	static bool Encode(const std::vector<VxlSection>& sections,
		const std::vector<std::uint8_t>& palette,
		const std::string& fileName,
		std::vector<std::uint8_t>& out_data);
};
