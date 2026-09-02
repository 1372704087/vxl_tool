#pragma once

// 极简 PNG 写出器（真彩色 RGB，8-bit）。
// 不依赖外部库：IDAT 使用 zlib "stored"（未压缩）deflate 块，
// 任何标准 PNG 解码器均可读取。适合渲染预览图。

#include <cstdint>
#include <vector>

class PngWriter
{
public:
	// 将 RGB 像素缓冲（width × height × 3）编码为 PNG 二进制。
	//   width/height : 图像尺寸（>0）
	//   rgb          : 像素数据，行优先，每像素 3 字节（R,G,B），
	//                  每行从左到右、从上到下。
	//   out_data     : 输出的 PNG 二进制
	static bool WriteRGB(int width, int height,
		const std::vector<std::uint8_t>& rgb,
		std::vector<std::uint8_t>& out_data);
};
