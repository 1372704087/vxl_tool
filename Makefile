# VXL 工具 Makefile（Linux/macOS 用 g++/clang++；Windows 可改用 MSVC 或 MinGW）
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Iinclude
LDFLAGS  ?=

SRCS := src/main.cpp src/VxlEncoder.cpp src/VxlDecoder.cpp src/VxlNormals.cpp src/generators.cpp src/HvaEncoder.cpp src/HvaDecoder.cpp src/PngWriter.cpp src/VoxelRenderer.cpp src/ObjLoader.cpp src/Voxelizer.cpp
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
