#pragma once

// 自定义调色板加载器。支持两种常见 RA2 调色板文件格式：
//   1. 二进制裸 768 字节 RGB（XCC 导出的 *.pal 常为 768 字节）
//   2. 文本 INI：可含 [Palette] 头、; / # / // 注释，数值用空格或逗号分隔，
//      每 3 个 0..255 为一个 RGB；读到 256 色即停。
// 两种格式无需指定，自动判别。

#include <cstdint>
#include <string>

class PaletteLoader
{
public:
	// 从文件加载 768 字节 RGB 到 out768（长度需 >=768）。
	// 成功返回 true。失败时 out768 保持原样。
	static bool Load(const std::string& path, std::uint8_t* out768);
};