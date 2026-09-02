#pragma once

// 软件体素渲染器：将 VXL（可选 HVA）模型渲染为 RGBA 像素缓冲。
// 坐标映射与 gamemd 渲染器一致（MapVoxelWithHva + HVA 法线旋转），
// 采用画家算法（按深度排序）逐体素绘制方块，法线漫反射着色。

#include <cstdint>
#include <vector>

#include <VxlTypes.h>

struct RenderOptions
{
	int imageSize = 512;        // 输出图像边长（像素）
	float yaw = -0.6f;          // 绕 Z 轴旋转（弧度，负=逆时针）
	float pitch = -0.45f;       // 俯角（弧度，负=俯视，约 26°）
	float zoom = 1.0f;          // 缩放倍率
	std::uint8_t bg[3] = { 38, 42, 50 };  // 背景色（深蓝灰）
};

class VoxelRenderer
{
public:
	// 渲染 VXL sections 到 RGBA 缓冲（imageSize × imageSize × 4）。
	//   sections        : 解码后的 section 列表
	//   palette         : 768 字节 RGB 调色板
	//   hvaTransforms   : 可选 HVA 变换（扁平 [frame*sectionCount+section]×12 float）
	//   hvaSectionCount : HVA 的 section 数（0 表示无 HVA）
	//   frame           : 使用的 HVA 帧
	//   opt             : 渲染选项
	//   out_rgba        : 输出 RGBA（每像素 4 字节，alpha=255）
	static bool Render(const std::vector<VxlSection>& sections,
		const std::vector<std::uint8_t>& palette,
		const std::vector<float>& hvaTransforms,
		int hvaSectionCount, int frame,
		const RenderOptions& opt,
		std::vector<std::uint8_t>& out_rgba);
};
