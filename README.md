# VXL 体素模型工具（vxl\_tool）

用 C++17 实现的《红色警戒 2》VXL 体素模型导入、校验与渲染工具链，零外部依赖（仅标准库）。
支持多 section 模型与 HVA 动画定位文件，可将模型软件渲染为 PNG 图片。

## 功能特性

- **VXL 解码**：符合 RA2 格式（header / palette / section header / span 数据 / tailer），span 采用游程编码

- **VXL 编码**：由 VxlSection 写出 RA2 格式 VXL（导入流程需要）

- **HVA 解码**：多 section 定位与多帧动画矩阵（3×4 变换，行主序）

- **法线表**：normalsMode 1..4 全套法线查找（与 gamemd 工程一致）

- **OBJ 导入**：将 Wavefront OBJ 三角网格体素化为 VXL（射线投射法，支持顶点色与高度渐变着色）

- **软件渲染器**：HVA 变换、3D 投影（yaw/pitch 相机）、画家算法深度排序、法线光照，输出 PNG

- **PNG 写入**：手写 zlib stored 块 + Adler32，无需链接 zlib

- **CLI**：`import` / `verify` / `render` 三个子命令

## 构建

需要 g++/clang++（C++17）与 make，无其他依赖。也可用 Visual Studio 打开 `vxl_tool.sln` 构建。

```bash
make            # 编译，产物为 out/vxltool
make clean      # 清理
```

## 用法

```
vxltool import <file.obj> -o <out.vxl> [--size N] [--color N] [--up y|z] [--mode N]
vxltool verify <file.vxl> [--hva <file.hva>]
vxltool render <file.vxl> [--hva <file.hva>] -o <out.png> [--size N] [--yaw A] [--pitch A] [--frame N]
```

| 参数          | 说明                                                       |
| ----------- | -------------------------------------------------------- |
| `--mode N`  | normalsMode（1..4，默认 4 = RA2）                             |
| `--size N`  | 渲染图像边长（64..2048，默认 512）；import 时指体素网格最长轴边长（4..256，默认 64） |
| `--color N` | import 固定调色板颜色索引（默认 0 = 按高度渐变）                           |
| `--up y\|z` | import 的 OBJ 上轴（y = Blender 默认，z = 3ds Max 默认）           |
| `--yaw A`   | 绕 Z 轴旋转弧度（默认 -0.6）                                       |
| `--pitch A` | 俯角弧度（默认 -0.45；正俯视用 -3.14）                                |
| `--frame N` | HVA 动画帧（默认 0）                                            |

### 示例

```bash
# 导入 OBJ 网格（Blender 导出 Y-up，64³ 网格，高度渐变着色）
./out/vxltool import samples/sphere_y_color.obj -o out/sphere.vxl --up y --size 64

# 导入带顶点色的 OBJ（3ds Max 导出 Z-up，128³ 网格）
./out/vxltool import model.obj -o out/model.vxl --up z --size 128

# 校验文件
./out/vxltool verify out/model.vxl

# 渲染为 PNG
./out/vxltool render out/model.vxl -o out/model.png --size 1024
```

## 目录结构

```
include/  头文件（VxlTypes / VxlEncoder / VxlDecoder / VxlNormals / HvaDecoder /
          generators / ObjLoader / Voxelizer / VoxelRenderer / PngWriter）
src/      实现（main 为 CLI 入口）
samples/  示例 OBJ 网格（供 import 测试）
Makefile  构建脚本
vxl_tool.sln / vxl_tool.vcxproj   Visual Studio 工程
```

## 技术说明

- VXL 导入体素化：对每个体素中心沿 +X/+Y/+Z 三轴各做一次射线投射（Möller–Trumbore 求交），
  奇数交点判为内部，三轴多数表决以规避擦边/过顶点误判；顶点色按最近三角形重心插值后映射到调色板

- 法线由邻域暴露面计算，再查法线表取最接近索引（`FindNormalIndex`）

- 渲染器对全部体素做 HVA 变换 → 投影 → 按深度排序（画家算法）→ 法线光照着色

- PNG 输出为 8-bit RGB，zlib stored（未压缩）块，兼容常见查看器

