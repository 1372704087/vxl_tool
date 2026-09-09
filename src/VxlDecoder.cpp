#include <VxlDecoder.h>

#include <cstring>

// 小端读取辅助
static std::uint32_t rd32(const std::uint8_t* p)
{
	return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) | ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
}
static float rdf32(const std::uint8_t* p)
{
	std::uint32_t u = rd32(p);
	float f;
	std::memcpy(&f, &u, 4);
	return f;
}

bool VxlDecoder::Decode(const std::uint8_t* data, int size, std::vector<VxlSection>& out_sections,
	int& out_voxelCount)
{
	out_sections.clear();
	out_voxelCount = 0;

	if (data == nullptr || size < 802)
		return false;
	// [修复 C10] 固定头总长 802 字节（含 768 调色板），最小合法文件也必然 >= 802。
	//   原先仅校验 size>=32，在 32<=size<802 时会靠后面逐条尾检兜底，路径冗长且让
	//   bodySize(经 int 转换)出现负值/超大值等不确定分支。此处提前拦截，语义更清晰。

	// ---- VxlHeader (32 字节) ----
	int pos = 0;
	pos += 16;                              // fileName
	[[maybe_unused]] std::uint32_t paletteCount = rd32(data + pos); pos += 4;
	std::uint32_t headerCount = rd32(data + pos); pos += 4;
	std::uint32_t tailerCount = rd32(data + pos); pos += 4;
	std::uint32_t bodySize = rd32(data + pos);   pos += 4;
	pos += 1;                               // remapStart
	pos += 1;                               // remapEnd
	pos += 768;                             // palette (跳过; 头共 34 字节)

	if (headerCount == 0 || tailerCount == 0 || tailerCount != headerCount)
		return false;
	if (headerCount > 64)
		return false;

	// ---- Section 头（每个 28 字节: name(16) + 3×uint32）----
	struct SectionHeaderInfo { std::string name; };
	std::vector<SectionHeaderInfo> headers;
	headers.reserve(headerCount);
	for (std::uint32_t i = 0; i < headerCount; ++i)
	{
		if (pos + 28 > size)
			return false;
		char name[17] = { 0 };
		std::memcpy(name, data + pos, 16);
		pos += 16;
		pos += 12;  // 3×uint32
		headers.push_back({ name });
	}

	// body 起始位置（section 头之后）
	int bodyStart = pos;
	if (pos + (int)bodySize > size)
		bodySize = size - pos;

	// ---- Section Tailer ----
	const int TAILER_SIZE = 92;
	struct TailerInfo
	{
		std::uint32_t startingSpanOffset, endingSpanOffset, dataSpanOffset;
		float hvaMultiplier;
		float minBounds[3], maxBounds[3];
		int sizeX, sizeY, sizeZ, normalsMode;
	};
	std::vector<TailerInfo> tailers;
	tailers.reserve(tailerCount);

	int tailerPos = bodyStart + (int)bodySize;
	for (std::uint32_t i = 0; i < tailerCount; ++i)
	{
		if (tailerPos + TAILER_SIZE > size)
			return false;
		TailerInfo t;
		t.startingSpanOffset = rd32(data + tailerPos); tailerPos += 4;
		t.endingSpanOffset = rd32(data + tailerPos);   tailerPos += 4;
		t.dataSpanOffset = rd32(data + tailerPos);     tailerPos += 4;
		t.hvaMultiplier = rdf32(data + tailerPos);     tailerPos += 4;
		for (int m = 0; m < 12; ++m)  // transfMatrix(48 字节, 4×4 float)
			tailerPos += 4;
		t.minBounds[0] = rdf32(data + tailerPos); tailerPos += 4;
		t.minBounds[1] = rdf32(data + tailerPos); tailerPos += 4;
		t.minBounds[2] = rdf32(data + tailerPos); tailerPos += 4;
		t.maxBounds[0] = rdf32(data + tailerPos); tailerPos += 4;
		t.maxBounds[1] = rdf32(data + tailerPos); tailerPos += 4;
		t.maxBounds[2] = rdf32(data + tailerPos); tailerPos += 4;
		t.sizeX = data[tailerPos];
		t.sizeY = data[tailerPos + 1];
		t.sizeZ = data[tailerPos + 2];
		t.normalsMode = data[tailerPos + 3];
		tailerPos += 4;
		tailers.push_back(t);
	}

	// ---- Span 数据 ----
	out_sections.reserve(headerCount);
	for (std::uint32_t i = 0; i < headerCount; ++i)
	{
		VxlSection section;
		section.name = headers[i].name;
		const TailerInfo& t = tailers[i];
		section.sizeX = t.sizeX;
		section.sizeY = t.sizeY;
		section.sizeZ = t.sizeZ;
		section.normalsMode = t.normalsMode;
		section.hvaMultiplier = t.hvaMultiplier;
		for (int b = 0; b < 3; ++b) {
			section.minBounds[b] = t.minBounds[b];
			section.maxBounds[b] = t.maxBounds[b];
		}

		// 起始/结束 span 表：sizeY × sizeX 个 int32
		int spanTableBytes = t.sizeX * t.sizeY * 4;
		int startBase = bodyStart + t.startingSpanOffset;
		int endBase = bodyStart + t.endingSpanOffset;
		int dataBase = bodyStart + t.dataSpanOffset;

		if (startBase < 0 || startBase + spanTableBytes > size ||
		endBase < 0 || endBase + spanTableBytes > size ||
		dataBase < 0 || dataBase > size)
		{
			// [修复 C6] 越界说明 section 头/tailer 与文件体不一致（文件已损坏），
			//   原实现 continue 会"少读部分体素接着返回 true"，导致上层把损坏数据当成功产物。
			//   此处改为 return false，让 verify/render 明确报告解析失败。
			return false;
		}

		// 读取起始/结束 span 偏移表
		std::vector<std::int32_t> starts(t.sizeX * t.sizeY);
		std::vector<std::int32_t> ends(t.sizeX * t.sizeY);
		for (int idx = 0; idx < t.sizeX * t.sizeY; ++idx)
		{
			starts[idx] = (std::int32_t)rd32(data + startBase + idx * 4);
			ends[idx] = (std::int32_t)rd32(data + endBase + idx * 4);
		}

		// 读取每个格的 span voxels
		for (int y = 0; y < t.sizeY; ++y)
		{
			for (int x = 0; x < t.sizeX; ++x)
			{
				int idx = y * t.sizeX + x;
				std::int32_t startOff = starts[idx];
				std::int32_t endOff = ends[idx];

				VxlSpan span;
				span.x = x;
				span.y = y;

				if (startOff != -1 && endOff != -1)
				{
					// 读取该格的 span 数据
					int p = dataBase + startOff;
					int z = 0;
					while (z < t.sizeZ && p < size)
					{
						z += data[p++];              // 跳过 N
						if (p >= size) break;
						std::uint8_t count = data[p++];
						for (int v = 0; v < count && z < t.sizeZ && p + 1 < size; ++v)
						{
							VxlVoxel voxel;
							voxel.x = x;
							voxel.y = y;
							voxel.z = z++;
							voxel.colorIndex = data[p++];
							voxel.normalIndex = data[p++];
							span.voxels.push_back(voxel);
						}
						if (p < size)
							p++;  // 跳过尾部字节
					}
				}

				out_voxelCount += (int)span.voxels.size();
				section.spans.push_back(std::move(span));
			}
		}

		out_sections.push_back(std::move(section));
	}

	return !out_sections.empty();
}

void VxlDecoder::GetVoxels(const VxlSection& section, std::vector<VxlVoxel>& out_voxels)
{
	out_voxels.clear();
	for (const auto& span : section.spans)
	{
		out_voxels.insert(out_voxels.end(), span.voxels.begin(), span.voxels.end());
	}
}

bool VxlDecoder::GetPalette(const std::uint8_t* data, int size, std::uint8_t* out_rgb768)
{
	if (!data || !out_rgb768 || size < 802)
		return false;

	std::memcpy(out_rgb768, data + 34, 768);
	return true;
}
