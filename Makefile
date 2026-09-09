# VXL 工具 Makefile（Linux/macOS 用 g++/clang++；Windows 可改用 MSVC 或 MinGW）
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Iinclude
# VXL2 v3 区块化压缩依赖 zlib（deflate）。库存系统一般自带（-lz）。macOS 用 -lz 即可。
# VXL2 v5 多算法压缩依赖 zstd（-lzstd）、lz4（-llz4）。
LDFLAGS  ?= -lz -lzstd -llz4

SRCS := src/main.cpp src/VxlEncoder.cpp src/VxlDecoder.cpp src/VxlNormals.cpp src/generators.cpp src/HvaEncoder.cpp src/HvaDecoder.cpp src/PngWriter.cpp src/VoxelRenderer.cpp src/ObjLoader.cpp src/Voxelizer.cpp src/ObjExporter.cpp src/Vxl2.cpp src/Palette.cpp
OBJS := $(SRCS:src/%.cpp=out/%.o)
TARGET := out/vxltool

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

out/%.o: src/%.cpp | out
	$(CXX) $(CXXFLAGS) -c -o $@ $<

out:
	mkdir -p out

clean:
	rm -rf out

.PHONY: all clean
