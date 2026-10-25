#include "TestHarness.h"

#include "ArchiveReader.h"

#include <windows.h>
#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// BA2 DX10 extraction: a multi-chunk texture (compressed and stored chunks)
// extracts to the same bytes as before the single-stream rewrite, and a
// payload that already starts with a DDS header is returned unchanged.

namespace
{
    using namespace Floar;

    template<typename T>
    void Put(std::vector<uint8_t>& out, T value)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    void PutBytes(std::vector<uint8_t>& out, const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i)
        {
            data[i] = static_cast<uint8_t>((i * 31 + seed * 7 + (i >> 5)) & 0xFF);
        }
        return data;
    }

    std::vector<uint8_t> Deflate(const std::vector<uint8_t>& data)
    {
        uLongf size = compressBound(static_cast<uLong>(data.size()));
        std::vector<uint8_t> out(size);
        if (compress2(out.data(), &size, data.data(), static_cast<uLong>(data.size()), 9) != Z_OK)
        {
            return {};
        }
        out.resize(size);
        return out;
    }

    struct Dx10Chunk
    {
        std::vector<uint8_t> data;
        bool compressed { false };
    };

    // One DX10 texture named `name` made of `chunks`.
    std::vector<uint8_t> BuildDx10Ba2(const std::string& name, const std::vector<Dx10Chunk>& chunks)
    {
        constexpr uint64_t kHeaderSize = 24;
        constexpr uint64_t kRecordSize = 24;
        constexpr uint64_t kChunkSize = 24;
        uint64_t dataOffset = kHeaderSize + kRecordSize + kChunkSize * chunks.size();

        std::vector<std::vector<uint8_t>> stored;
        for (const auto& chunk : chunks)
        {
            stored.push_back(chunk.compressed ? Deflate(chunk.data) : chunk.data);
        }
        uint64_t nameTableOffset = dataOffset;
        for (const auto& bytes : stored)
        {
            nameTableOffset += bytes.size();
        }

        std::vector<uint8_t> out;
        PutBytes(out, "BTDX", 4);
        Put<uint32_t>(out, 1);
        PutBytes(out, "DX10", 4);
        Put<uint32_t>(out, 1);
        Put<uint64_t>(out, nameTableOffset);

        Put<uint32_t>(out, 0x12345678);                 // name hash
        PutBytes(out, "dds\0", 4);
        Put<uint32_t>(out, 0x9ABCDEF0);                 // dir hash
        Put<uint8_t>(out, 0);
        Put<uint8_t>(out, static_cast<uint8_t>(chunks.size()));
        Put<uint16_t>(out, 24);                         // chunk header size
        Put<uint16_t>(out, 64);                         // height
        Put<uint16_t>(out, 128);                        // width
        Put<uint8_t>(out, 4);                           // mips
        Put<uint8_t>(out, 71);                          // DXGI_FORMAT_BC1_UNORM
        Put<uint16_t>(out, 0);

        uint64_t offset = dataOffset;
        for (size_t i = 0; i < chunks.size(); ++i)
        {
            Put<uint64_t>(out, offset);
            Put<uint32_t>(out, chunks[i].compressed ? static_cast<uint32_t>(stored[i].size()) : 0);
            Put<uint32_t>(out, static_cast<uint32_t>(chunks[i].data.size()));
            Put<uint16_t>(out, static_cast<uint16_t>(i));
            Put<uint16_t>(out, static_cast<uint16_t>(i));
            Put<uint32_t>(out, 0xBAADF00D);
            offset += stored[i].size();
        }
        for (const auto& bytes : stored)
        {
            PutBytes(out, bytes.data(), bytes.size());
        }
        Put<uint16_t>(out, static_cast<uint16_t>(name.size()));
        PutBytes(out, name.data(), name.size());
        return out;
    }

    std::filesystem::path WriteTemp(const std::wstring& stem, const std::vector<uint8_t>& bytes)
    {
        wchar_t tempPath[MAX_PATH] {};
        GetTempPathW(MAX_PATH, tempPath);
        const std::filesystem::path path = std::filesystem::path(tempPath) /
            (L"FloarBa2_" + std::to_wstring(GetCurrentProcessId()) + L"_" + stem + L".ba2");
        std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return path;
    }

    uint64_t Fnv1a(const std::vector<uint8_t>& data)
    {
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (uint8_t byte : data)
        {
            hash = (hash ^ byte) * 0x100000001b3ULL;
        }
        return hash;
    }

    std::vector<Dx10Chunk> ThreeChunks()
    {
        return {
            { Pattern(4096, 1), true },
            { Pattern(1024, 2), false },
            { Pattern(256, 3), true },
        };
    }
}

FLOAR_TEST(Ba2_Dx10MultiChunkOutputIsByteIdentical)
{
    const auto chunks = ThreeChunks();
    const auto path = WriteTemp(L"multi", BuildDx10Ba2("textures/stone.dds", chunks));
    auto reader = ArchiveReaderFactory::Open(path);
    FLOAR_REQUIRE(reader != nullptr);

    const size_t handlesBefore = ArchiveReaderFactory::DataHandleOpenCountForTesting();
    const std::vector<uint8_t> output = reader->ExtractToMemory(L"Textures\\Stone.dds");
    // One file handle for all three chunks.
    FLOAR_CHECK(ArchiveReaderFactory::DataHandleOpenCountForTesting() == handlesBefore + 1);
    std::vector<uint8_t> payload;
    for (const auto& chunk : chunks)
    {
        payload.insert(payload.end(), chunk.data.begin(), chunk.data.end());
    }
    constexpr size_t kDdsHeaderSize = 4 + 124 + 20;   // magic, DDS_HEADER, DDS_HEADER_DXT10
    FLOAR_REQUIRE(output.size() == kDdsHeaderSize + payload.size());
    FLOAR_CHECK(std::memcmp(output.data(), "DDS ", 4) == 0);
    FLOAR_CHECK(std::memcmp(output.data() + kDdsHeaderSize, payload.data(), payload.size()) == 0);
    uint32_t height = 0;
    uint32_t width = 0;
    std::memcpy(&height, output.data() + 12, 4);
    std::memcpy(&width, output.data() + 16, 4);
    FLOAR_CHECK(height == 64 && width == 128);
    // Recorded from the extractor before the single-stream rewrite.
    FLOAR_CHECK(Fnv1a(output) == 0xb513b563386684dfULL);

    // Random-access batch extraction: each entry directly, in request order.
    std::vector<std::wstring> order;
    FLOAR_CHECK(reader->ExtractBatch({ L"missing.dds", L"textures/stone.dds" }, {}, [&](const std::wstring& path, std::vector<uint8_t>&& data)
    {
        order.push_back(path);
        FLOAR_CHECK(path == L"missing.dds" ? data.empty() : data == output);
    }));
    FLOAR_CHECK(order == (std::vector<std::wstring> { L"missing.dds", L"textures/stone.dds" }));

    // Concurrent extractions from one reader produce the same bytes.
    std::vector<std::vector<uint8_t>> results(4);
    std::vector<std::thread> threads;
    for (auto& result : results)
    {
        threads.emplace_back([&] { result = reader->ExtractToMemory(L"textures/stone.dds"); });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    for (const auto& result : results)
    {
        FLOAR_CHECK(result == output);
    }

    reader.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

FLOAR_TEST(Ba2_Dx10PayloadWithDdsHeaderIsReturnedAsIs)
{
    std::vector<uint8_t> first = Pattern(512, 4);
    std::memcpy(first.data(), "DDS ", 4);
    const std::vector<Dx10Chunk> chunks = { { first, true }, { Pattern(300, 5), false } };
    const auto path = WriteTemp(L"prefixed", BuildDx10Ba2("a.dds", chunks));
    auto reader = ArchiveReaderFactory::Open(path);
    FLOAR_REQUIRE(reader != nullptr);

    std::vector<uint8_t> expected = chunks[0].data;
    expected.insert(expected.end(), chunks[1].data.begin(), chunks[1].data.end());
    FLOAR_CHECK(reader->ExtractToMemory(L"a.dds") == expected);
    FLOAR_CHECK(reader->ExtractToMemory(L"missing.dds").empty());

    reader.reset();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
