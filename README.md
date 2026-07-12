# Floar — Floyd's Archive Reader

Virtual path / virtual file system and archive readers (BA2, ZIP, 7z, RAR).

## Features

- `Floar::VirtualPath` — filesystem or archive-inner path
- `Floar::VirtualFileSystem` — list / read / is-directory across disk and archives
- `Floar::ArchiveReaderFactory` — BSA (v103/v104/v105) + BA2 (hand-rolled) + optional libarchive ZIP/7z/RAR
- `Floar::Log::SetSink` — optional log sink (no-op until installed)

## Build

```cmake
add_subdirectory(Floar)
target_link_libraries(your_target PRIVATE Floar)
```

Option: `FLOAR_ENABLE_COMMON_ARCHIVES` (default ON) enables ZIP/7z/RAR via libarchive.

Nested submodules: `external/zlib`, `external/xz`, `external/libarchive`.
