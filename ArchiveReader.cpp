#include "ArchiveReader.h"
#include "ArchiveTypes.h"
#include "StringUtil.h"
#include "FloarLog.h"
#include "VirtualPath.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>
#include <dxgiformat.h>

#ifndef FLOAR_ENABLE_COMMON_ARCHIVES
#define FLOAR_ENABLE_COMMON_ARCHIVES 1
#endif

#if FLOAR_ENABLE_COMMON_ARCHIVES
#include <archive.h>
#include <archive_entry.h>
#endif

#if __has_include(<zlib.h>)
#include <zlib.h>
#define FLOAR_HAS_ZLIB 1
#else
#define FLOAR_HAS_ZLIB 0
#endif

#include "lz4.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#if !FLOAR_HAS_ZLIB
extern "C"
{
    typedef unsigned char Bytef;
    typedef unsigned long uLong;
    typedef unsigned long uLongf;
    int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen);
}
#endif

namespace Floar
{
namespace
{
    constexpr int kZlibOk = 0;
    constexpr uint32_t kMaxBa2InflateBytes = 1024u * 1024u * 1024u;

    std::wstring Utf8ToWide(const char* utf8Str)
    {
        if (!utf8Str || !*utf8Str)
            return {};

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, nullptr, 0);
        if (wideLen <= 0)
            return {};

        std::wstring result(wideLen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, result.data(), wideLen);
        return result;
    }

    std::wstring Utf8ToWide(const char* utf8Str, size_t len)
    {
        if (!utf8Str || len == 0)
        {
            return {};
        }

        int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str, static_cast<int>(len), nullptr, 0);
        if (wideLen <= 0)
        {
            return {};
        }

        std::wstring result(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Str, static_cast<int>(len), result.data(), wideLen);
        return result;
    }

    std::string WideToUtf8(const std::wstring& wideStr)
    {
        if (wideStr.empty())
            return {};

        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len <= 0)
            return {};

        std::string result(utf8Len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, result.data(), utf8Len, nullptr, nullptr);
        return result;
    }

#if FLOAR_ENABLE_COMMON_ARCHIVES
    // Code page libarchive uses for its narrow diagnostics (including %ls
    // paths). Mirrors get_current_codepage() in archive_string.c: the "C"
    // locale stores each UTF-16 unit <= 0xFF as one byte (Latin-1) and '?'
    // otherwise; ".utf8"/".UTF-8" is UTF-8; ".NNN" is code page NNN; anything
    // else is the ANSI code page.
    unsigned LibArchiveDiagnosticCodePage()
    {
        constexpr unsigned kLatin1 = 28591;
        const char* locale = setlocale(LC_CTYPE, nullptr);
        if (!locale)
        {
            return GetACP();
        }
        if (locale[0] == 'C' && locale[1] == '\0')
        {
            return kLatin1;
        }
        const char* dot = std::strrchr(locale, '.');
        if (!dot)
        {
            return GetACP();
        }
        if (std::strcmp(dot + 1, "utf8") == 0 || std::strcmp(dot + 1, "UTF-8") == 0)
        {
            return CP_UTF8;
        }
        const int codePage = std::atoi(dot + 1);
        return codePage > 0 ? static_cast<unsigned>(codePage) : GetACP();
    }

    // archive_error_string() as UTF-8 for the log sink.
    std::string ArchiveErrorUtf8(struct archive* a)
    {
        const char* message = archive_error_string(a);
        if (!message)
        {
            return "Unknown error";
        }
        return Log::CodePageToUtf8(message, LibArchiveDiagnosticCodePage());
    }
#endif

    template <typename T>
    bool ReadValue(std::ifstream& stream, T& outValue)
    {
        stream.read(reinterpret_cast<char*>(&outValue), sizeof(T));
        return stream.good();
    }

    bool ReadBytes(std::ifstream& stream, void* data, size_t size)
    {
        stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
        return stream.good();
    }

    std::wstring NormalizeArchivePath(std::wstring path)
    {
        std::replace(path.begin(), path.end(), L'\\', L'/');
        return path;
    }

    // Directory keys (ArchiveInnerPathKey) of every explicit directory entry and
    // every parent of an entry, built while a reader scans its table.
    class DirectoryIndex
    {
    public:
        void Add(const ArchiveEntry& entry)
        {
            const std::wstring key = ArchiveInnerPathKey(entry.name);
            if (entry.isDirectory && !key.empty())
            {
                m_directories.insert(key);
            }
            for (size_t pos = key.find(L'/'); pos != std::wstring::npos; pos = key.find(L'/', pos + 1))
            {
                m_directories.insert(key.substr(0, pos));
            }
        }

        bool Contains(const std::wstring& dirPath) const
        {
            return m_directories.find(ArchiveInnerPathKey(dirPath)) != m_directories.end();
        }

    private:
        std::unordered_set<std::wstring> m_directories {};
    };

    std::atomic<size_t> g_statusQueries { 0 };
    std::atomic<size_t> g_opens { 0 };

    std::wstring ExtensionFromFourCC(const char(&ext)[4])
    {
        std::wstring out;
        for (char ch : ext)
        {
            if (ch == '\0' || ch == ' ')
            {
                break;
            }
            out.push_back(static_cast<wchar_t>(ch));
        }
        return out;
    }

    uint32_t BytesPerBlock(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16;
        default:
            return 0;
        }
    }

    uint32_t BytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
            return 1;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R16_FLOAT:
            return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16;
        default:
            return 0;
        }
    }

    uint32_t CalcLinearSize(uint32_t width, uint32_t height, DXGI_FORMAT format)
    {
        const uint32_t blockBytes = BytesPerBlock(format);
        if (blockBytes != 0)
        {
            const uint32_t bw = (width + 3u) / 4u;
            const uint32_t bh = (height + 3u) / 4u;
            return bw * bh * blockBytes;
        }

        const uint32_t bpp = BytesPerPixel(format);
        if (bpp != 0)
        {
            return width * bpp;
        }

        return 0;
    }

    std::vector<uint8_t> DecompressZlibRaw(
        const std::vector<uint8_t>& compressed,
        uint32_t expectedSize)
    {
#if !FLOAR_HAS_ZLIB
        (void)compressed;
        (void)expectedSize;
        return {};
#else
        std::vector<uint8_t> output;
        if (compressed.empty() || expectedSize == 0 || expectedSize > kMaxBa2InflateBytes)
        {
            FLOAR_LOG_WARN("[BA2] Raw inflate skipped: invalid sizes.");
            return output;
        }

        output.resize(expectedSize);
        z_stream stream {};
        stream.next_in = const_cast<Bytef*>(compressed.data());
        stream.avail_in = static_cast<uInt>(compressed.size());
        stream.next_out = output.data();
        stream.avail_out = static_cast<uInt>(output.size());

        const int initResult = inflateInit2(&stream, -MAX_WBITS);
        if (initResult != Z_OK)
        {
            FLOAR_LOG_WARN("[BA2] Raw inflate init failed.");
            output.clear();
            return output;
        }

        const int inflateResult = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);

        if (inflateResult != Z_STREAM_END || stream.total_out == 0 || stream.total_out > expectedSize)
        {
            FLOAR_LOG_WARN("[BA2] Raw inflate failed or size mismatch.");
            output.clear();
            return output;
        }

        output.resize(stream.total_out);
        return output;
#endif
    }

    std::vector<uint8_t> DecompressZlib(
        const std::vector<uint8_t>& compressed,
        uint32_t expectedSize)
    {
        std::vector<uint8_t> output;
        if (compressed.empty() || expectedSize == 0 || expectedSize > kMaxBa2InflateBytes)
        {
            FLOAR_LOG_WARN("[BA2] Zlib inflate skipped: invalid sizes.");
            return output;
        }

        output.resize(expectedSize);
        uLongf outSize = expectedSize;
        const int result = uncompress(
            output.data(),
            &outSize,
            compressed.data(),
            static_cast<uLong>(compressed.size()));
        if (result == kZlibOk && outSize > 0 && outSize <= expectedSize)
        {
            output.resize(outSize);
            return output;
        }

        FLOAR_LOG_WARN("[BA2] Zlib inflate failed, trying raw deflate.");
        auto raw = DecompressZlibRaw(compressed, expectedSize);
        if (!raw.empty())
        {
            return raw;
        }

        FLOAR_LOG_WARN("[BA2] Raw deflate also failed.");
        return {};
    }

    // Skyrim SE v105 BSAs store their compressed payloads in the LZ4 FRAME
    // format (magic 0x184D2204: frame descriptor + a sequence of independent
    // or linked data blocks), not as a single raw LZ4 block. lz4frame.c is
    // not vendored, but the frame layout is simple enough to walk by hand
    // and decode block-wise with the block API.
    std::vector<uint8_t> DecompressLz4Frame(
        const std::vector<uint8_t>& compressed,
        uint32_t expectedSize)
    {
        const uint8_t* src = compressed.data();
        const size_t srcSize = compressed.size();
        size_t pos = 4; // past the magic checked by the caller

        auto readU32 = [&](uint32_t& v) -> bool
        {
            if (pos + 4 > srcSize)
                return false;
            v = static_cast<uint32_t>(src[pos]) | (static_cast<uint32_t>(src[pos + 1]) << 8)
                | (static_cast<uint32_t>(src[pos + 2]) << 16) | (static_cast<uint32_t>(src[pos + 3]) << 24);
            pos += 4;
            return true;
        };

        if (pos + 3 > srcSize)
            return {};
        const uint8_t flg = src[pos++];
        pos++; // BD byte (max block size) - block sizes are validated per-block below
        if ((flg >> 6) != 0x1)
        {
            FLOAR_LOG_WARN("[BSA] LZ4 frame: unsupported version.");
            return {};
        }
        const bool blockIndep = (flg & 0x20) != 0;
        const bool blockChecksum = (flg & 0x10) != 0;
        if ((flg & 0x08) != 0) pos += 8;  // Content Size field
        if ((flg & 0x01) != 0) pos += 4;  // Dictionary ID field
        pos += 1;                         // header checksum byte

        std::vector<uint8_t> output(expectedSize);
        size_t outPos = 0;
        for (;;)
        {
            uint32_t blockSize = 0;
            if (!readU32(blockSize))
            {
                FLOAR_LOG_WARN("[BSA] LZ4 frame: truncated block header.");
                return {};
            }
            if (blockSize == 0) // EndMark
                break;
            const bool storedUncompressed = (blockSize & 0x80000000u) != 0;
            blockSize &= 0x7FFFFFFFu;
            if (pos + blockSize > srcSize)
            {
                FLOAR_LOG_WARN("[BSA] LZ4 frame: block overruns payload.");
                return {};
            }
            if (storedUncompressed)
            {
                if (outPos + blockSize > output.size())
                {
                    FLOAR_LOG_WARN("[BSA] LZ4 frame: output overflow.");
                    return {};
                }
                std::memcpy(output.data() + outPos, src + pos, blockSize);
                outPos += blockSize;
            }
            else
            {
                const int capacity = static_cast<int>(output.size() - outPos);
                int decoded;
                if (blockIndep || outPos == 0)
                {
                    decoded = LZ4_decompress_safe(
                        reinterpret_cast<const char*>(src + pos),
                        reinterpret_cast<char*>(output.data() + outPos),
                        static_cast<int>(blockSize), capacity);
                }
                else
                {
                    // Linked blocks: previous output doubles as dictionary
                    // (the block API uses at most the trailing 64 KiB).
                    decoded = LZ4_decompress_safe_usingDict(
                        reinterpret_cast<const char*>(src + pos),
                        reinterpret_cast<char*>(output.data() + outPos),
                        static_cast<int>(blockSize), capacity,
                        reinterpret_cast<const char*>(output.data()),
                        static_cast<int>(outPos));
                }
                if (decoded < 0)
                {
                    FLOAR_LOG_WARN("[BSA] LZ4 frame: block decode failed.");
                    return {};
                }
                outPos += static_cast<size_t>(decoded);
            }
            pos += blockSize;
            if (blockChecksum)
                pos += 4;
        }

        output.resize(outPos);
        return output;
    }

    std::vector<uint8_t> DecompressLz4(
        const std::vector<uint8_t>& compressed,
        uint32_t expectedSize)
    {
        std::vector<uint8_t> output;
        if (compressed.empty() || expectedSize == 0 || expectedSize > kMaxBa2InflateBytes)
        {
            FLOAR_LOG_WARN("[BSA] LZ4 decompress skipped: invalid sizes.");
            return output;
        }

        constexpr uint32_t kLz4FrameMagic = 0x184D2204u;
        if (compressed.size() >= 4)
        {
            const uint32_t magic = static_cast<uint32_t>(compressed[0]) | (static_cast<uint32_t>(compressed[1]) << 8)
                | (static_cast<uint32_t>(compressed[2]) << 16) | (static_cast<uint32_t>(compressed[3]) << 24);
            if (magic == kLz4FrameMagic)
                return DecompressLz4Frame(compressed, expectedSize);
        }

        // Not a frame: treat as one raw LZ4 block.
        output.resize(expectedSize);
        const int decoded = LZ4_decompress_safe(
            reinterpret_cast<const char*>(compressed.data()),
            reinterpret_cast<char*>(output.data()),
            static_cast<int>(compressed.size()),
            static_cast<int>(expectedSize));
        if (decoded <= 0 || static_cast<uint32_t>(decoded) > expectedSize)
        {
            FLOAR_LOG_WARN("[BSA] LZ4 decompress failed.");
            output.clear();
            return output;
        }

        output.resize(static_cast<size_t>(decoded));
        return output;
    }

    bool ReadBzString(std::ifstream& stream, std::string& out)
    {
        uint8_t len = 0;
        if (!ReadValue(stream, len))
        {
            return false;
        }
        if (len == 0)
        {
            out.clear();
            return true;
        }

        out.assign(static_cast<size_t>(len), '\0');
        if (!ReadBytes(stream, out.data(), out.size()))
        {
            return false;
        }
        if (!out.empty() && out.back() == '\0')
        {
            out.pop_back();
        }
        return true;
    }
}

class Ba2Reader : public IArchiveReader
{
    struct Ba2Chunk
    {
        uint64_t offset = 0;
        uint32_t packedSize = 0;
        uint32_t unpackedSize = 0;
    };

    struct Ba2FileInfo
    {
        std::wstring name;
        std::wstring extension;
        bool isDX10 = false;
        uint64_t offset = 0;
        uint32_t packedSize = 0;
        uint32_t unpackedSize = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint8_t mipCount = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::vector<Ba2Chunk> chunks;
    };

    struct Ba2Header
    {
        char magic[4] {};
        uint32_t version = 0;
        char type[4] {};
        uint32_t fileCount = 0;
        uint64_t nameTableOffset = 0;
    };

    std::filesystem::path m_path;
    std::vector<ArchiveEntry> m_entries;
    std::unordered_map<std::wstring, size_t> m_entryMap;
    DirectoryIndex m_directories;
    std::vector<Ba2FileInfo> m_files;
    std::wstring m_formatName { L"BA2" };

public:
    explicit Ba2Reader(const std::filesystem::path& path)
        : m_path(path)
    {
        ScanArchive();
    }

    const std::vector<ArchiveEntry>& Entries() const override
    {
        return m_entries;
    }

    bool HasDirectory(const std::wstring& dirPath) const override
    {
        return m_directories.Contains(dirPath);
    }

    std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override
    {
        const std::wstring key = ArchiveInnerPathKey(entryPath);
        auto it = m_entryMap.find(key);
        if (it == m_entryMap.end())
        {
            return {};
        }

        const Ba2FileInfo& info = m_files[it->second];
        if (info.isDX10)
        {
            return ExtractDx10(info);
        }

        return ExtractGnrl(info);
    }

    bool HasEntry(const std::wstring& entryPath) override
    {
        return m_entryMap.find(ArchiveInnerPathKey(entryPath)) != m_entryMap.end();
    }

    std::wstring GetFormatName() const override
    {
        return m_formatName;
    }

private:
    bool ReadAt(uint64_t offset, void* data, size_t size) const
    {
        std::ifstream stream(m_path, std::ios::binary);
        if (!stream)
        {
            return false;
        }

        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream.good())
        {
            return false;
        }

        return ReadBytes(stream, data, size);
    }

    std::vector<uint8_t> ReadRange(uint64_t offset, size_t size) const
    {
        std::vector<uint8_t> data;
        if (size == 0)
        {
            return data;
        }

        data.resize(size);
        if (!ReadAt(offset, data.data(), size))
        {
            data.clear();
        }
        return data;
    }

    std::vector<uint8_t> ExtractGnrl(const Ba2FileInfo& info) const
    {
        const uint32_t packedSize = info.packedSize;
        const uint32_t unpackedSize = info.unpackedSize;
        if (unpackedSize == 0)
        {
            FLOAR_LOG_WARN("[BA2] GNRL entry has zero unpacked size.");
            return {};
        }

        if (packedSize != 0 && packedSize != unpackedSize)
        {
            const auto compressed = ReadRange(info.offset, packedSize);
            if (compressed.empty())
            {
                FLOAR_LOG_WARN("[BA2] GNRL failed to read compressed data.");
                return {};
            }
            auto data = DecompressZlib(compressed, unpackedSize);
            if (data.empty())
            {
                FLOAR_LOG_WARN("[BA2] GNRL decompression failed.");
            }
            return data;
        }

        const size_t readSize = (packedSize != 0) ? packedSize : unpackedSize;
        auto data = ReadRange(info.offset, readSize);
        if (data.empty())
        {
            FLOAR_LOG_WARN("[BA2] GNRL failed to read uncompressed data.");
        }
        return data;
    }

    std::vector<uint8_t> ExtractDx10(const Ba2FileInfo& info) const
    {
        std::vector<uint8_t> result;
        if (info.unpackedSize > 0)
        {
            result.reserve(info.unpackedSize);
        }
        for (const auto& chunk : info.chunks)
        {
            if (chunk.packedSize != 0 && chunk.packedSize != chunk.unpackedSize)
            {
                const auto compressed = ReadRange(chunk.offset, chunk.packedSize);
                if (compressed.empty())
                {
                    FLOAR_LOG_WARN("[BA2] DX10 failed to read compressed chunk.");
                    return {};
                }
                auto decompressed = DecompressZlib(compressed, chunk.unpackedSize);
                if (decompressed.empty())
                {
                    FLOAR_LOG_WARN("[BA2] DX10 chunk decompression failed.");
                    return {};
                }
                result.insert(result.end(), decompressed.begin(), decompressed.end());
                continue;
            }

            const size_t readSize = (chunk.packedSize != 0) ? chunk.packedSize : chunk.unpackedSize;
            auto chunkData = ReadRange(chunk.offset, readSize);
            if (chunkData.empty())
            {
                FLOAR_LOG_WARN("[BA2] DX10 failed to read chunk data.");
                return {};
            }
            result.insert(result.end(), chunkData.begin(), chunkData.end());
        }

        if (result.size() >= 4 && result[0] == 'D' && result[1] == 'D' && result[2] == 'S' && result[3] == ' ')
        {
            return result;
        }

        auto header = BuildDdsHeader(info.width, info.height, info.mipCount, info.format);
        if (header.empty())
        {
            FLOAR_LOG_WARN("[BA2] DX10 failed to build DDS header.");
            return {};
        }

        header.insert(header.end(), result.begin(), result.end());
        return header;
    }

    std::vector<uint8_t> BuildDdsHeader(uint16_t width, uint16_t height, uint8_t mipCount, DXGI_FORMAT format) const
    {
        if (format == DXGI_FORMAT_UNKNOWN || width == 0 || height == 0)
        {
            return {};
        }

        constexpr uint32_t kDdsMagic = 0x20534444u;
        constexpr uint32_t DDSD_CAPS = 0x1u;
        constexpr uint32_t DDSD_HEIGHT = 0x2u;
        constexpr uint32_t DDSD_WIDTH = 0x4u;
        constexpr uint32_t DDSD_PIXELFORMAT = 0x1000u;
        constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000u;
        constexpr uint32_t DDSD_LINEARSIZE = 0x80000u;

        constexpr uint32_t DDSCAPS_COMPLEX = 0x8u;
        constexpr uint32_t DDSCAPS_TEXTURE = 0x1000u;
        constexpr uint32_t DDSCAPS_MIPMAP = 0x400000u;

        constexpr uint32_t DDS_FOURCC = 0x4u;
        constexpr uint32_t DDS_DX10 = 0x30315844u;

#pragma pack(push, 1)
        struct DdsPixelFormat
        {
            uint32_t size;
            uint32_t flags;
            uint32_t fourCC;
            uint32_t rgbBitCount;
            uint32_t rBitMask;
            uint32_t gBitMask;
            uint32_t bBitMask;
            uint32_t aBitMask;
        };

        struct DdsHeader
        {
            uint32_t size;
            uint32_t flags;
            uint32_t height;
            uint32_t width;
            uint32_t pitchOrLinearSize;
            uint32_t depth;
            uint32_t mipMapCount;
            uint32_t reserved1[11];
            DdsPixelFormat ddspf;
            uint32_t caps;
            uint32_t caps2;
            uint32_t caps3;
            uint32_t caps4;
            uint32_t reserved2;
        };

        struct DdsHeaderDx10
        {
            DXGI_FORMAT dxgiFormat;
            uint32_t resourceDimension;
            uint32_t miscFlag;
            uint32_t arraySize;
            uint32_t miscFlags2;
        };
#pragma pack(pop)

        DdsHeader header {};
        header.size = sizeof(DdsHeader);
        header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE;
        header.height = height;
        header.width = width;
        header.pitchOrLinearSize = CalcLinearSize(width, height, format);
        header.depth = 0;
        header.mipMapCount = (mipCount > 0) ? mipCount : 1;
        if (header.mipMapCount > 1)
        {
            header.flags |= DDSD_MIPMAPCOUNT;
        }
        header.ddspf.size = sizeof(DdsPixelFormat);
        header.ddspf.flags = DDS_FOURCC;
        header.ddspf.fourCC = DDS_DX10;
        header.caps = DDSCAPS_TEXTURE;
        if (header.mipMapCount > 1)
        {
            header.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
        }

        DdsHeaderDx10 headerDx10 {};
        headerDx10.dxgiFormat = format;
        headerDx10.resourceDimension = 3;
        headerDx10.miscFlag = 0;
        headerDx10.arraySize = 1;
        headerDx10.miscFlags2 = 0;

        std::vector<uint8_t> data;
        data.resize(sizeof(kDdsMagic) + sizeof(DdsHeader) + sizeof(DdsHeaderDx10));
        size_t offset = 0;
        std::memcpy(data.data() + offset, &kDdsMagic, sizeof(kDdsMagic));
        offset += sizeof(kDdsMagic);
        std::memcpy(data.data() + offset, &header, sizeof(header));
        offset += sizeof(header);
        std::memcpy(data.data() + offset, &headerDx10, sizeof(headerDx10));
        return data;
    }

    void ScanArchive()
    {
        std::ifstream stream(m_path, std::ios::binary);
        if (!stream)
        {
            FLOAR_LOG_WARN("[BA2] Failed to open archive file.");
            return;
        }

        Ba2Header header {};
        if (!ReadBytes(stream, header.magic, sizeof(header.magic)))
        {
            FLOAR_LOG_WARN("[BA2] Failed to read archive header magic.");
            return;
        }
        if (!ReadValue(stream, header.version) ||
            !ReadBytes(stream, header.type, sizeof(header.type)) ||
            !ReadValue(stream, header.fileCount) ||
            !ReadValue(stream, header.nameTableOffset))
        {
            FLOAR_LOG_WARN("[BA2] Failed to read archive header fields.");
            return;
        }

        const std::string magic(header.magic, header.magic + 4);
        if (magic != "BTDX" && magic != "BDTX")
        {
            FLOAR_LOG_WARN("[BA2] Invalid BA2 magic.");
            return;
        }

        const std::string type(header.type, header.type + 4);
        const bool isDx10 = (type == "DX10");
        const bool isGnrl = (type == "GNRL");
        if (!isDx10 && !isGnrl)
        {
            FLOAR_LOG_WARN("[BA2] Unsupported BA2 type.");
            return;
        }

        m_files.reserve(header.fileCount);
        m_entries.reserve(header.fileCount);
        for (uint32_t i = 0; i < header.fileCount; ++i)
        {
            Ba2FileInfo info {};
            info.isDX10 = isDx10;

            if (isGnrl)
            {
                if (!ReadGnrlRecord(stream, info))
                {
                    FLOAR_LOG_WARN("[BA2] Failed to read GNRL record.");
                    break;
                }
            }
            else
            {
                if (!ReadDx10Record(stream, info))
                {
                    FLOAR_LOG_WARN("[BA2] Failed to read DX10 record.");
                    break;
                }
            }

            m_files.push_back(info);
        }

        std::vector<std::wstring> names = ReadNameTable(stream, header.nameTableOffset, header.fileCount);
        for (size_t i = 0; i < m_files.size(); ++i)
        {
            Ba2FileInfo& info = m_files[i];

            std::wstring name;
            if (i < names.size() && !names[i].empty())
            {
                name = names[i];
            }
            else
            {
                name = L"file_" + std::to_wstring(i);
                if (!info.extension.empty())
                {
                    name += L"." + info.extension;
                }
            }

            name = NormalizeArchivePath(name);
            info.name = name;

            ArchiveEntry entry {};
            entry.name = info.name;
            entry.size = info.unpackedSize;
            entry.compressedSize = info.packedSize;
            entry.isDirectory = false;
            entry.modTime = 0;

            m_entryMap[ArchiveInnerPathKey(info.name)] = i;
            m_directories.Add(entry);
            m_entries.push_back(entry);
        }
    }

    std::vector<std::wstring> ReadNameTable(std::ifstream& stream, uint64_t offset, uint32_t count)
    {
        std::vector<std::wstring> names;
        if (offset == 0 || count == 0)
        {
            FLOAR_LOG_WARN("[BA2] Name table missing or empty.");
            return names;
        }

        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream.good())
        {
            FLOAR_LOG_WARN("[BA2] Name table seek failed.");
            return names;
        }

        names.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint16_t len = 0;
            if (!ReadValue(stream, len))
            {
                FLOAR_LOG_WARN("[BA2] Name table length read failed.");
                break;
            }

            if (len == 0)
            {
                names.emplace_back();
                continue;
            }

            std::string utf8(static_cast<size_t>(len), '\0');
            if (!ReadBytes(stream, utf8.data(), utf8.size()))
            {
                FLOAR_LOG_WARN("[BA2] Name table string read failed.");
                break;
            }

            names.push_back(Utf8ToWide(utf8.data(), utf8.size()));
        }

        return names;
    }

    bool ReadGnrlRecord(std::ifstream& stream, Ba2FileInfo& outInfo)
    {
        uint32_t nameHash = 0;
        char ext[4] {};
        uint32_t dirHash = 0;
        uint32_t flags = 0;
        uint64_t offset = 0;
        uint32_t packedSize = 0;
        uint32_t unpackedSize = 0;
        uint32_t terminator = 0;

        if (!ReadValue(stream, nameHash) ||
            !ReadBytes(stream, ext, sizeof(ext)) ||
            !ReadValue(stream, dirHash) ||
            !ReadValue(stream, flags) ||
            !ReadValue(stream, offset) ||
            !ReadValue(stream, packedSize) ||
            !ReadValue(stream, unpackedSize) ||
            !ReadValue(stream, terminator))
        {
            return false;
        }

        outInfo.offset = offset;
        outInfo.packedSize = packedSize;
        outInfo.unpackedSize = unpackedSize;
        outInfo.extension = ExtensionFromFourCC(ext);
        return true;
    }

    bool ReadDx10Record(std::ifstream& stream, Ba2FileInfo& outInfo)
    {
        uint32_t nameHash = 0;
        char ext[4] {};
        uint32_t dirHash = 0;
        uint8_t unknown = 0;
        uint8_t chunkCount = 0;
        uint16_t chunkHeaderSize = 0;
        uint16_t height = 0;
        uint16_t width = 0;
        uint8_t mipCount = 0;
        uint8_t format = 0;
        uint16_t unknown2 = 0;

        if (!ReadValue(stream, nameHash) ||
            !ReadBytes(stream, ext, sizeof(ext)) ||
            !ReadValue(stream, dirHash) ||
            !ReadValue(stream, unknown) ||
            !ReadValue(stream, chunkCount) ||
            !ReadValue(stream, chunkHeaderSize) ||
            !ReadValue(stream, height) ||
            !ReadValue(stream, width) ||
            !ReadValue(stream, mipCount) ||
            !ReadValue(stream, format) ||
            !ReadValue(stream, unknown2))
        {
            return false;
        }

        outInfo.width = width;
        outInfo.height = height;
        outInfo.mipCount = mipCount;
        outInfo.format = static_cast<DXGI_FORMAT>(format);
        outInfo.extension = ExtensionFromFourCC(ext);

        outInfo.chunks.clear();
        outInfo.chunks.reserve(chunkCount);
        uint64_t packedTotal = 0;
        uint64_t unpackedTotal = 0;
        for (uint8_t i = 0; i < chunkCount; ++i)
        {
            Ba2Chunk chunk {};
            uint16_t startMip = 0;
            uint16_t endMip = 0;
            uint32_t terminator = 0;

            if (!ReadValue(stream, chunk.offset) ||
                !ReadValue(stream, chunk.packedSize) ||
                !ReadValue(stream, chunk.unpackedSize) ||
                !ReadValue(stream, startMip) ||
                !ReadValue(stream, endMip) ||
                !ReadValue(stream, terminator))
            {
                return false;
            }

            if (chunkHeaderSize > 24)
            {
                stream.seekg(static_cast<std::streamoff>(chunkHeaderSize - 24), std::ios::cur);
            }

            outInfo.chunks.push_back(chunk);
            packedTotal += chunk.packedSize;
            unpackedTotal += chunk.unpackedSize;
        }

        outInfo.packedSize = (packedTotal > std::numeric_limits<uint32_t>::max())
            ? 0
            : static_cast<uint32_t>(packedTotal);
        outInfo.unpackedSize = (unpackedTotal > std::numeric_limits<uint32_t>::max())
            ? 0
            : static_cast<uint32_t>(unpackedTotal);
        return true;
    }
};

class BsaReader : public IArchiveReader
{
    static constexpr uint32_t kHeaderSize = 36;
    static constexpr uint32_t kSizeCompressionFlip = 0x40000000u;
    static constexpr uint32_t kFlagIncludeDirectoryNames = 0x1u;
    static constexpr uint32_t kFlagIncludeFileNames = 0x2u;
    static constexpr uint32_t kFlagDefaultCompressed = 0x4u;
    static constexpr uint32_t kFlagXboxArchive = 0x40u;
    static constexpr uint32_t kFlagEmbedFileNames = 0x100u;
    static constexpr uint32_t kFlagXMemCodec = 0x200u;

    struct BsaFileInfo
    {
        std::wstring name;
        uint64_t offset = 0;
        uint32_t rawSizeField = 0;
        uint32_t reportedSize = 0;
        bool compressed = false;
    };

    std::filesystem::path m_path;
    std::vector<ArchiveEntry> m_entries;
    std::unordered_map<std::wstring, size_t> m_entryMap;
    DirectoryIndex m_directories;
    std::vector<BsaFileInfo> m_files;
    uint32_t m_version = 0;
    uint32_t m_archiveFlags = 0;
    std::wstring m_formatName { L"BSA" };

public:
    explicit BsaReader(const std::filesystem::path& path)
        : m_path(path)
    {
        ScanArchive();
    }

    const std::vector<ArchiveEntry>& Entries() const override
    {
        return m_entries;
    }

    bool HasDirectory(const std::wstring& dirPath) const override
    {
        return m_directories.Contains(dirPath);
    }

    std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override
    {
        const std::wstring key = ArchiveInnerPathKey(entryPath);
        auto it = m_entryMap.find(key);
        if (it == m_entryMap.end())
        {
            return {};
        }
        return ExtractFile(m_files[it->second]);
    }

    bool HasEntry(const std::wstring& entryPath) override
    {
        return m_entryMap.find(ArchiveInnerPathKey(entryPath)) != m_entryMap.end();
    }

    std::wstring GetFormatName() const override
    {
        return m_formatName;
    }

private:
    bool EmbedFileNames() const
    {
        // Embedded names appear in v104/v105 file data blocks when bit 9 is set.
        return m_version >= 104 && (m_archiveFlags & kFlagEmbedFileNames) != 0;
    }

    bool DefaultCompressed() const
    {
        return (m_archiveFlags & kFlagDefaultCompressed) != 0;
    }

    std::vector<uint8_t> ExtractFile(const BsaFileInfo& info) const
    {
        std::ifstream stream(m_path, std::ios::binary);
        if (!stream)
        {
            FLOAR_LOG_WARN("[BSA] Failed to open archive for extract.");
            return {};
        }

        stream.seekg(static_cast<std::streamoff>(info.offset), std::ios::beg);
        if (!stream.good())
        {
            FLOAR_LOG_WARN("[BSA] Seek to file data failed.");
            return {};
        }

        uint32_t remaining = info.reportedSize;
        if (EmbedFileNames())
        {
            // The embedded name is a bstring (1 length byte + N chars, NO
            // trailing NUL - verified against Skyrim SE's own archives), so
            // account for exactly the bytes consumed off the stream rather
            // than assuming a bzstring's extra terminator; overcounting by 1
            // truncated the tail byte of every compressed payload.
            const std::streamoff nameStart = stream.tellg();
            std::string embedded;
            if (!ReadBzString(stream, embedded))
            {
                FLOAR_LOG_WARN("[BSA] Failed to skip embedded file name.");
                return {};
            }
            const uint32_t onDiskNameBytes = static_cast<uint32_t>(stream.tellg() - nameStart);
            if (remaining < onDiskNameBytes)
            {
                FLOAR_LOG_WARN("[BSA] Embedded name larger than file size field.");
                return {};
            }
            remaining -= onDiskNameBytes;
        }

        if (!info.compressed)
        {
            if (remaining == 0)
            {
                return {};
            }
            std::vector<uint8_t> data(remaining);
            if (!ReadBytes(stream, data.data(), data.size()))
            {
                FLOAR_LOG_WARN("[BSA] Failed to read uncompressed file data.");
                return {};
            }
            return data;
        }

        if (remaining < sizeof(uint32_t))
        {
            FLOAR_LOG_WARN("[BSA] Compressed block too small for original size.");
            return {};
        }

        uint32_t originalSize = 0;
        if (!ReadValue(stream, originalSize))
        {
            FLOAR_LOG_WARN("[BSA] Failed to read original size.");
            return {};
        }
        remaining -= sizeof(uint32_t);

        if (originalSize == 0 || originalSize > kMaxBa2InflateBytes || remaining == 0)
        {
            FLOAR_LOG_WARN("[BSA] Invalid compressed payload sizes.");
            return {};
        }

        std::vector<uint8_t> compressed(remaining);
        if (!ReadBytes(stream, compressed.data(), compressed.size()))
        {
            FLOAR_LOG_WARN("[BSA] Failed to read compressed payload.");
            return {};
        }

        if (m_version >= 105)
        {
            return DecompressLz4(compressed, originalSize);
        }
        return DecompressZlib(compressed, originalSize);
    }

    void ScanArchive()
    {
        std::ifstream stream(m_path, std::ios::binary);
        if (!stream)
        {
            FLOAR_LOG_WARN("[BSA] Failed to open archive file.");
            return;
        }

        char magic[4] {};
        uint32_t version = 0;
        uint32_t folderOffset = 0;
        uint32_t archiveFlags = 0;
        uint32_t folderCount = 0;
        uint32_t fileCount = 0;
        uint32_t totalFolderNameLength = 0;
        uint32_t totalFileNameLength = 0;
        uint16_t fileFlags = 0;
        uint16_t padding = 0;

        if (!ReadBytes(stream, magic, sizeof(magic)) ||
            !ReadValue(stream, version) ||
            !ReadValue(stream, folderOffset) ||
            !ReadValue(stream, archiveFlags) ||
            !ReadValue(stream, folderCount) ||
            !ReadValue(stream, fileCount) ||
            !ReadValue(stream, totalFolderNameLength) ||
            !ReadValue(stream, totalFileNameLength) ||
            !ReadValue(stream, fileFlags) ||
            !ReadValue(stream, padding))
        {
            FLOAR_LOG_WARN("[BSA] Failed to read header.");
            return;
        }

        if (!(magic[0] == 'B' && magic[1] == 'S' && magic[2] == 'A' && magic[3] == '\0'))
        {
            FLOAR_LOG_WARN("[BSA] Invalid BSA magic.");
            return;
        }

        if (version != 103 && version != 104 && version != 105)
        {
            FLOAR_LOG_WARN("[BSA] Unsupported BSA version {}.", version);
            return;
        }

        if ((archiveFlags & kFlagXboxArchive) != 0 || (archiveFlags & kFlagXMemCodec) != 0)
        {
            FLOAR_LOG_WARN("[BSA] Xbox/XMem BSA archives are not supported.");
            return;
        }

        if (folderOffset != kHeaderSize)
        {
            FLOAR_LOG_WARN("[BSA] Unexpected folder record offset {}.", folderOffset);
            // Continue anyway; some tools tolerate this.
        }

        m_version = version;
        m_archiveFlags = archiveFlags;
        m_formatName = L"BSA v" + std::to_wstring(version);

        struct FolderRecord
        {
            uint64_t nameHash = 0;
            uint32_t count = 0;
            uint32_t offset = 0;
        };

        std::vector<FolderRecord> folders;
        folders.reserve(folderCount);
        for (uint32_t i = 0; i < folderCount; ++i)
        {
            FolderRecord folder {};
            if (!ReadValue(stream, folder.nameHash) || !ReadValue(stream, folder.count))
            {
                FLOAR_LOG_WARN("[BSA] Failed to read folder record.");
                return;
            }

            if (version >= 105)
            {
                uint32_t pad = 0;
                if (!ReadValue(stream, pad))
                {
                    FLOAR_LOG_WARN("[BSA] Failed to read v105 folder padding.");
                    return;
                }
            }

            if (!ReadValue(stream, folder.offset))
            {
                FLOAR_LOG_WARN("[BSA] Failed to read folder offset.");
                return;
            }

            if (version >= 105)
            {
                uint32_t pad = 0;
                if (!ReadValue(stream, pad))
                {
                    FLOAR_LOG_WARN("[BSA] Failed to read v105 folder offset padding.");
                    return;
                }
            }

            folders.push_back(folder);
        }

        const bool includeDirNames = (archiveFlags & kFlagIncludeDirectoryNames) != 0;
        const bool includeFileNames = (archiveFlags & kFlagIncludeFileNames) != 0;

        struct PendingFile
        {
            uint64_t nameHash = 0;
            uint32_t sizeField = 0;
            uint32_t offset = 0;
            std::wstring folderPath;
        };
        std::vector<PendingFile> pending;
        pending.reserve(fileCount);

        for (const FolderRecord& folder : folders)
        {
            const uint64_t recordsOffset =
                static_cast<uint64_t>(folder.offset) - static_cast<uint64_t>(totalFileNameLength);
            stream.seekg(static_cast<std::streamoff>(recordsOffset), std::ios::beg);
            if (!stream.good())
            {
                FLOAR_LOG_WARN("[BSA] Seek to folder file records failed.");
                return;
            }

            std::wstring folderPath;
            if (includeDirNames)
            {
                std::string folderName;
                if (!ReadBzString(stream, folderName))
                {
                    FLOAR_LOG_WARN("[BSA] Failed to read folder name.");
                    return;
                }
                folderPath = Utf8ToWide(folderName.data(), folderName.size());
                folderPath = NormalizeArchivePath(folderPath);
                while (!folderPath.empty() && (folderPath.back() == L'/' || folderPath.back() == L'\\'))
                {
                    folderPath.pop_back();
                }
            }

            for (uint32_t fi = 0; fi < folder.count; ++fi)
            {
                PendingFile file {};
                if (!ReadValue(stream, file.nameHash) ||
                    !ReadValue(stream, file.sizeField) ||
                    !ReadValue(stream, file.offset))
                {
                    FLOAR_LOG_WARN("[BSA] Failed to read file record.");
                    return;
                }
                file.folderPath = folderPath;
                pending.push_back(file);
            }
        }

        std::vector<std::wstring> fileNames;
        fileNames.reserve(fileCount);
        if (includeFileNames && totalFileNameLength > 0)
        {
            std::string nameBlob(static_cast<size_t>(totalFileNameLength), '\0');
            if (!ReadBytes(stream, nameBlob.data(), nameBlob.size()))
            {
                FLOAR_LOG_WARN("[BSA] Failed to read file name block.");
                return;
            }

            size_t pos = 0;
            while (pos < nameBlob.size() && fileNames.size() < pending.size())
            {
                const size_t end = nameBlob.find('\0', pos);
                if (end == std::string::npos)
                {
                    fileNames.push_back(Utf8ToWide(nameBlob.data() + pos, nameBlob.size() - pos));
                    break;
                }
                fileNames.push_back(Utf8ToWide(nameBlob.data() + pos, end - pos));
                pos = end + 1;
            }
        }

        m_files.reserve(pending.size());
        m_entries.reserve(pending.size());
        for (size_t i = 0; i < pending.size(); ++i)
        {
            const PendingFile& src = pending[i];
            BsaFileInfo info {};
            info.offset = src.offset;
            info.rawSizeField = src.sizeField;

            const bool flip = (src.sizeField & kSizeCompressionFlip) != 0;
            info.reportedSize = src.sizeField & ~kSizeCompressionFlip;
            info.compressed = DefaultCompressed() ^ flip;

            std::wstring fileName;
            if (i < fileNames.size() && !fileNames[i].empty())
            {
                fileName = fileNames[i];
            }
            else
            {
                fileName = L"file_" + std::to_wstring(i);
            }

            if (!src.folderPath.empty())
            {
                info.name = NormalizeArchivePath(src.folderPath + L"/" + fileName);
            }
            else
            {
                info.name = NormalizeArchivePath(fileName);
            }

            ArchiveEntry entry {};
            entry.name = info.name;
            entry.size = info.compressed ? 0 : info.reportedSize;
            entry.compressedSize = info.compressed ? info.reportedSize : 0;
            entry.isDirectory = false;
            entry.modTime = 0;

            m_entryMap[ArchiveInnerPathKey(info.name)] = m_files.size();
            m_files.push_back(info);
            m_directories.Add(entry);
            m_entries.push_back(entry);
        }

        FLOAR_LOG_INFO("[BSA] Opened {} (version {}, {} entries).",
            Log::ToUtf8(m_path), m_version, m_entries.size());
    }
};

#if FLOAR_ENABLE_COMMON_ARCHIVES
class LibArchiveReader : public IArchiveReader
{
    std::filesystem::path m_path;
    std::vector<ArchiveEntry> m_entries;
    std::unordered_map<std::wstring, size_t> m_entryMap;
    DirectoryIndex m_directories;
    std::wstring m_formatName;

public:
    LibArchiveReader(const std::filesystem::path& path)
        : m_path(path)
    {
        FLOAR_LOG_INFO("[LibArchiveReader] Opening: {}", Log::ToUtf8(path));
        ScanArchive();
        FLOAR_LOG_INFO("[LibArchiveReader] Scanned, found {} entries", m_entries.size());
    }

    const std::vector<ArchiveEntry>& Entries() const override
    {
        return m_entries;
    }

    bool HasDirectory(const std::wstring& dirPath) const override
    {
        return m_directories.Contains(dirPath);
    }

    std::vector<uint8_t> ExtractToMemory(const std::wstring& entryPath) override
    {
        FLOAR_LOG_DEBUG("[LibArchiveReader] ExtractToMemory: {}", Log::ToUtf8(entryPath));

        auto it = m_entryMap.find(ArchiveInnerPathKey(entryPath));
        if (it == m_entryMap.end())
        {
            FLOAR_LOG_WARN("[LibArchiveReader] Entry not found in map");
            return {};
        }

        const ArchiveEntry& entry = m_entries[it->second];
        if (entry.isDirectory)
        {
            return {};
        }

        // Open archive for extraction
        struct archive* a = archive_read_new();
        if (!a)
        {
            return {};
        }

#if FLOAR_ENABLE_COMMON_ARCHIVES
        // Enable only the formats we need
        archive_read_support_format_zip(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);

        // Enable only essential compression filters for ZIP/7z/RAR
        archive_read_support_filter_none(a);    // Uncompressed
        archive_read_support_filter_gzip(a);    // ZIP deflate/gzip
        archive_read_support_filter_lzma(a);    // 7z LZMA
        archive_read_support_filter_xz(a);      // 7z XZ
#endif

        // Wide open: libarchive reads a narrow path in the active code page on
        // Windows, which cannot name files outside it.
        int r = archive_read_open_filename_w(a, m_path.c_str(), 10240);
        if (r != ARCHIVE_OK)
        {
            archive_read_free(a);
            return {};
        }

        std::vector<uint8_t> result;
        struct archive_entry* archiveEntry;
        // Use the original entry name (with correct case) from m_entries
        std::string targetUtf8 = WideToUtf8(entry.name);

        while (archive_read_next_header(a, &archiveEntry) == ARCHIVE_OK)
        {
            // UTF-8 view, independent of the CRT locale (see ScanArchive).
            const char* name = archive_entry_pathname_utf8(archiveEntry);
            if (!name)
                continue;

            if (std::string(name) == targetUtf8)
            {
                // Found the entry, extract it
                la_int64_t size = archive_entry_size(archiveEntry);

                if (size > 0 && size < 1024 * 1024 * 1024) // Max 1GB
                {
                    result.resize(static_cast<size_t>(size));
                    size_t totalRead = 0;
                    while (totalRead < result.size())
                    {
                        la_ssize_t chunkRead = archive_read_data(
                            a,
                            result.data() + totalRead,
                            result.size() - totalRead);
                        if (chunkRead <= 0)
                        {
                            break;
                        }
                        totalRead += static_cast<size_t>(chunkRead);
                    }

                    if (totalRead != result.size())
                    {
                        result.clear();
                    }
                }
                else if (size < 0)
                {
                    // Some archive entries report unknown size. Read as a stream.
                    constexpr size_t kReadChunk = 64 * 1024;
                    std::vector<uint8_t> streamBuf(kReadChunk);
                    size_t totalRead = 0;

                    while (true)
                    {
                        la_ssize_t chunkRead = archive_read_data(a, streamBuf.data(), streamBuf.size());
                        if (chunkRead == 0)
                        {
                            break;
                        }
                        if (chunkRead < 0)
                        {
                            result.clear();
                            break;
                        }

                        totalRead += static_cast<size_t>(chunkRead);
                        if (totalRead > 1024ull * 1024ull * 1024ull)
                        {
                            result.clear();
                            break;
                        }

                        result.insert(
                            result.end(),
                            streamBuf.begin(),
                            streamBuf.begin() + static_cast<std::ptrdiff_t>(chunkRead));
                    }
                }
                break;
            }
        }

        archive_read_free(a);
        return result;
    }

    bool HasEntry(const std::wstring& entryPath) override
    {
        return m_entryMap.find(ArchiveInnerPathKey(entryPath)) != m_entryMap.end();
    }

    std::wstring GetFormatName() const override
    {
        return m_formatName;
    }

private:
    void ScanArchive()
    {
        struct archive* a = archive_read_new();
        if (!a)
        {
            return;
        }

#if FLOAR_ENABLE_COMMON_ARCHIVES
        // Enable only the formats we need
        archive_read_support_format_zip(a);
        archive_read_support_format_7zip(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);

        // Enable only essential compression filters for ZIP/7z/RAR
        archive_read_support_filter_none(a);    // Uncompressed
        archive_read_support_filter_gzip(a);    // ZIP deflate/gzip
        archive_read_support_filter_lzma(a);    // 7z LZMA
        archive_read_support_filter_xz(a);      // 7z XZ
#endif

        // Wide open: libarchive reads a narrow path in the active code page on
        // Windows, which cannot name files outside it.
        int r = archive_read_open_filename_w(a, m_path.c_str(), 10240);
        if (r != ARCHIVE_OK)
        {
            FLOAR_LOG_ERROR("[LibArchiveReader] ERROR: Failed to open {}: {}",
                Log::ToUtf8(m_path), ArchiveErrorUtf8(a));
            archive_read_free(a);
            return;
        }

        // Get format name
        const char* formatName = archive_format_name(a);
        if (formatName)
        {
            m_formatName = Utf8ToWide(formatName);
        }

        struct archive_entry* entry;
        while (true)
        {
            int r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF)
            {
                break;
            }
            if (r != ARCHIVE_OK)
            {
                FLOAR_LOG_ERROR("[LibArchiveReader] ERROR reading header: {}", ArchiveErrorUtf8(a));
                break;
            }

            // archive_entry_pathname() is in the CRT locale's code page (Latin-1
            // or '?' in the "C" locale); the UTF-8 view keeps every name intact.
            const char* name = archive_entry_pathname_utf8(entry);
            if (!name)
            {
                continue;
            }

            ArchiveEntry archEntry;
            archEntry.name = Utf8ToWide(name);
            archEntry.size = archive_entry_size(entry);
            archEntry.compressedSize = 0; // libarchive doesn't always provide this
            archEntry.isDirectory = (archive_entry_filetype(entry) == AE_IFDIR);
            archEntry.modTime = archive_entry_mtime(entry);

            size_t index = m_entries.size();
            m_directories.Add(archEntry);
            m_entries.push_back(archEntry);
            m_entryMap[ArchiveInnerPathKey(archEntry.name)] = index;
        }

        archive_read_free(a);
    }
};
#endif

// ArchiveReaderFactory implementation

bool ArchiveReaderFactory::IsArchiveFile(const std::filesystem::path& path)
{
    // Extension first: browsing calls this for every file, and a status query
    // can be slow (network paths).
    if (!HasArchiveExtension(path.filename().wstring()))
        return false;

    ++g_statusQueries;
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::status(path, ec)) && !ec;
}

size_t ArchiveReaderFactory::StatusQueryCountForTesting()
{
    return g_statusQueries.load();
}

size_t ArchiveReaderFactory::OpenCountForTesting()
{
    return g_opens.load();
}

std::unique_ptr<IArchiveReader> ArchiveReaderFactory::Open(const std::filesystem::path& path)
{
    ++g_opens;
    if (!IsArchiveFile(path))
        return nullptr;

    try
    {
        const std::wstring ext = Floar::StringUtil::ToLower(path.extension().wstring());
        if (ext == L".ba2")
        {
            auto reader = std::make_unique<Ba2Reader>(path);
            if (reader->Entries().empty())
            {
                return nullptr;
            }
            return reader;
        }
        if (ext == L".bsa")
        {
            auto reader = std::make_unique<BsaReader>(path);
            if (reader->Entries().empty())
            {
                return nullptr;
            }
            return reader;
        }

#if FLOAR_ENABLE_COMMON_ARCHIVES
        auto reader = std::make_unique<LibArchiveReader>(path);
        if (reader->Entries().empty())
        {
            return nullptr;
        }

        return reader;
#else
        return nullptr;
#endif
    }
    catch (...)
    {
        return nullptr;
    }
}

bool ArchiveReaderFactory::HasArchiveExtension(const std::wstring& filename)
{
    auto ext = std::filesystem::path(filename).extension().wstring();
    if (ext.empty())
        return false;

    ext = Floar::StringUtil::ToLower(ext);

    const auto& supportedExts = ArchiveTypes::SupportedReadExtensions();
    return std::find(supportedExts.begin(), supportedExts.end(), ext) != supportedExts.end();
}

std::vector<std::wstring> ArchiveReaderFactory::GetSupportedExtensions()
{
    return ArchiveTypes::SupportedReadExtensions();
}

} // namespace Floar
