#include <VxlEncoder.h>

#include <algorithm>
#include <cstring>

namespace
{
	// 小端写入辅助
	inline void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
	inline void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		out.push_back((std::uint8_t)(v & 0xFF));
		out.push_back((std::uint8_t)((v >> 8) & 0xFF));
		out.push_back((std::uint8_t)((v >> 16) & 0xFF));
		out.push_back((std::uint8_t)((v >> 24) & 0xFF));
	}
	inline void PutI32(std::vector<std::uint8_t>& out, std::int32_t v)
	{
		PutU32(out, (std::uint32_t)v);
	}
	inline void PutF32(std::vector<std::uint8_t>& out, float f)
	{
		std::uint32_t u;
		std::memcpy(&u, &f, 4);
		PutU32(out, u);
	}
	inline void PutBytes(std::vector<std::uint8_t>& out, const std::uint8_t* p, int n)
	{
		out.insert(out.end(), p, p + n);
	}
	inline void PutName(std::vector<std::uint8_t>& out, const std::string& name, int fixedLen)
	{
		char buf[16] = { 0 };
		std::memcpy(buf, name.c_str(), std::min((size_t)fixedLen, name.size()));
		PutBytes(out, (const std::uint8_t*)buf, fixedLen);
	}

	// 单 section 的 body 数据
	struct SectionBody
	{
		std::vector<std::int32_t> spanStart;   // sizeX*sizeY
		std::vector<std::int32_t> spanEnd;     // sizeX*sizeY
		std::vector<std::uint8_t> spanData;    // span 数据
	};

	// 把 section 的体素编码成 span 数据。
	// 返回 false 表示数据非法（坐标越界/重复 z 等）。
	bool BuildSectionBody(const VxlSection& sec, SectionBody& body)
	{
		if (sec.sizeX <= 0 || sec.sizeY <= 0 || sec.sizeZ <= 0)
			return false;
		if (sec.sizeX > 255 || sec.sizeY > 255 || sec.sizeZ > 255)
			return false;

		const int n = sec.sizeX * sec.sizeY;
		body.spanStart.assign(n, -1);
		body.spanEnd.assign(n, -1);
		body.spanData.clear();

		// 建立 (x,y) → 体素列 的查找（span 可能乱序）
		std::vector<std::vector<VxlVoxel>> cells(n);
		for (const auto& span : sec.spans)
		{
			if (span.x < 0 || span.x >= sec.sizeX || span.y < 0 || span.y >= sec.sizeY)
				continue;
			int idx = span.y * sec.sizeX + span.x;
			cells[idx].insert(cells[idx].end(), span.voxels.begin(), span.voxels.end());
		}

		// 逐格编码（y 外层、x 内层，与解码器一致）
		for (int y = 0; y < sec.sizeY; ++y)
		{
			for (int x = 0; x < sec.sizeX; ++x)
			{
				int idx = y * sec.sizeX + x;
				auto& voxels = cells[idx];
				if (voxels.empty())
					continue;  // spanStart/spanEnd 保持 -1

				// 按 z 排序（生成器通常已有序，这里防御性排序）
				std::sort(voxels.begin(), voxels.end(),
					[](const VxlVoxel& a, const VxlVoxel& b) { return a.z < b.z; });

				body.spanStart[idx] = (std::int32_t)body.spanData.size();

				int z = 0;
				size_t i = 0;
				bool ok = true;
				while (i < voxels.size() && ok)
				{
					// 校验坐标
					if (voxels[i].x != x || voxels[i].y != y ||
						voxels[i].z < 0 || voxels[i].z >= sec.sizeZ)
					{
						ok = false;
						break;
					}
					int skip = voxels[i].z - z;
					if (skip < 0 || skip > 255)  // 重复 z 或越界
					{
						ok = false;
						break;
					}

					// 连续体素段
					int count = 1;
					while (i + (size_t)count < voxels.size() &&
						voxels[i + (size_t)count].z == voxels[i].z + count)
						++count;
					// [建议/C12] count 写成 1 字节 (uint8_t)，依赖 sizeZ<=255 的上限才不溢出（单列最大
					//   连续 run ≤ sizeZ）。此不变量在 Encode 开头校验了 sizeZ<=255，安全；
					//   但 skip/count 的 255 上限与 bodySize 用 32 位累加都应视为隐性约束，建议在此注释固化。

					body.spanData.push_back((std::uint8_t)skip);
					body.spanData.push_back((std::uint8_t)count);
					for (int j = 0; j < count; ++j)
					{
						body.spanData.push_back(voxels[i + (size_t)j].colorIndex);
						body.spanData.push_back(voxels[i + (size_t)j].normalIndex);
					}
					body.spanData.push_back((std::uint8_t)count);  // 重复 count（引擎要求）

					z = voxels[i].z + count;
					i += (size_t)count;
				}
				if (!ok)
					return false;

				// 结尾标记：[剩余Z][0]
				int remaining = sec.sizeZ - z;
				if (remaining < 0 || remaining > 255)
					return false;
				body.spanData.push_back((std::uint8_t)remaining);
				body.spanData.push_back(0);

				body.spanEnd[idx] = (std::int32_t)body.spanData.size() - 1;
			}
		}

		return true;
	}
}

bool VxlEncoder::Encode(const std::vector<VxlSection>& sections,
	const std::vector<std::uint8_t>& palette,
	std::vector<std::uint8_t>& out_data)
{
	// [修复 C2] 删除原 fileName 死参数。VXL 头的前 16 字节是固定的文件类型标识
	//   "Voxel Animation"，并不存在"写入文件名"的字段；原参数从未被使用、具误导性。
	out_data.clear();

	if (sections.empty() || sections.size() > 64)
		return false;
	if (palette.size() < 768)
		return false;

	// 1) 构建各 section 的 body
	std::vector<SectionBody> bodies(sections.size());
	for (size_t i = 0; i < sections.size(); ++i)
	{
		if (!BuildSectionBody(sections[i], bodies[i]))
			return false;
	}

	// 2) 计算 bodySize 与各 section 的 body 偏移
	std::vector<std::uint32_t> bodyOffset(sections.size());
	std::uint32_t bodySize = 0;
	for (size_t i = 0; i < sections.size(); ++i)
	{
		bodyOffset[i] = bodySize;
		std::uint32_t n = (std::uint32_t)(sections[i].sizeX * sections[i].sizeY);
		bodySize += n * 4 + n * 4 + (std::uint32_t)bodies[i].spanData.size();
	}

	// 3) Header（802 字节）
	PutName(out_data, "Voxel Animation", 16);
	PutU32(out_data, 1);                    // unknown（恒 1）
	PutU32(out_data, (std::uint32_t)sections.size());  // section 数
	PutU32(out_data, (std::uint32_t)sections.size());  // section 数（重复）
	PutU32(out_data, bodySize);
	PutU8(out_data, 16);                    // remapStart
	PutU8(out_data, 31);                    // remapEnd
	PutBytes(out_data, palette.data(), 768);

	// 4) Section 头（28 字节 × N）
	for (size_t i = 0; i < sections.size(); ++i)
	{
		PutName(out_data, sections[i].name, 16);
		PutU32(out_data, (std::uint32_t)i);  // number（从 0 递增）
		PutU32(out_data, 1);                 // unknown（通常 1）
		PutU32(out_data, 0);                 // unknown2（通常 0 或 2）
	}

	// 5) Body
	for (size_t i = 0; i < sections.size(); ++i)
	{
		const SectionBody& b = bodies[i];
		for (std::int32_t v : b.spanStart) PutI32(out_data, v);
		for (std::int32_t v : b.spanEnd)   PutI32(out_data, v);
		PutBytes(out_data, b.spanData.data(), (int)b.spanData.size());
	}

	// 6) Tailer（92 字节 × N）
	for (size_t i = 0; i < sections.size(); ++i)
	{
		const VxlSection& sec = sections[i];
		std::uint32_t n = (std::uint32_t)(sec.sizeX * sec.sizeY);

		PutU32(out_data, bodyOffset[i]);                 // spanStartOffset
		PutU32(out_data, bodyOffset[i] + n * 4);         // spanEndOffset
		PutU32(out_data, bodyOffset[i] + n * 8);         // spanDataOffset
		PutF32(out_data, sec.hvaMultiplier);             // scale（RA2 通常 0.083333）

		// transform：单位矩阵（3×4，行主序）
		float ident[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
		for (int m = 0; m < 12; ++m) PutF32(out_data, ident[m]);

		for (int c = 0; c < 3; ++c) PutF32(out_data, sec.minBounds[c]);
		for (int c = 0; c < 3; ++c) PutF32(out_data, sec.maxBounds[c]);

		PutU8(out_data, (std::uint8_t)sec.sizeX);
		PutU8(out_data, (std::uint8_t)sec.sizeY);
		PutU8(out_data, (std::uint8_t)sec.sizeZ);
		PutU8(out_data, (std::uint8_t)sec.normalsMode);
	}

	return true;
}
