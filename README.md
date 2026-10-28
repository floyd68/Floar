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

## Forward cursors (solid 7z, RAR)

Reading one entry of a solid archive decompresses every entry before it in its
solid block. `IArchiveReader::HasExpensiveRandomAccess()` reports such readers
(libarchive 7z and RAR; ZIP, BA2 and BSA return false), and they extract on
forward cursors from an `ArchiveCursorPool` (`ArchiveCursorPool::Shared()`
unless `ArchiveReaderFactory::Open` is given another pool):

- A cursor is an open archive handle positioned after the entry headers it
  consumed. After an extraction it is kept, idle, at that position; a later
  request of the same reader whose first entry (in `ArchiveOrder`) is at or
  after it continues from there, without reopening the archive or
  decompressing the entries before it again. A request behind every retained
  cursor opens a new one (a restart). A cursor belongs to one extraction call
  at a time and is never shared, or closed while in use.
- `ExtractForward(paths, stop, sink, options)` is `ExtractBatch` with an early
  end and a priority: the sink returns false to end the pass after an entry
  (the cursor stays after it), and `ExtractPriority::High` admits a request the
  user waits on (a main image) past Normal (thumbnail) cursors.
  `VirtualFileSystem::ReadFile` has an overload taking these options.
- Cursor states: after a completed pass, a sink-ended pass or a stop seen
  between libarchive calls the cursor is kept at its position; after a failed
  or stopped libarchive call (a stop inside a block read, an I/O error, a
  decompression error), EOF before a requested entry, a header that is not the
  scanned one, or a throwing callback it is closed. A cursor that reached the
  last entry is closed. Missing and directory entries do not move it.
- Archive replacement: a reader remembers the size and write time of the file
  it scanned (the change check `ArchiveReaderCache` uses). A retained cursor is
  refused once the path's identity differs, and a newly opened handle whose
  file differs is not read: an old reader returns empty data rather than the
  new file's content.
- Limits (`ArchiveCursorLimits`, `SetLimits`): active cursors process-wide
  (default 3) and per reader (2), of which `reservedForHigh` (1) only High
  requests may use, so Normal work gets 2 and 1; idle cursors process-wide (2)
  and per reader (2); estimated memory of active and idle cursors (256 MiB),
  closing idle cursors first; idle timeout (15 s, checked at pool operations,
  `TrimIdle` closes all). At capacity, requests wait in FIFO order (High
  first) on their stop token; a stopped waiter leaves without taking capacity.
  The pool lock is never held while reading, opening or closing a cursor or
  running a callback. A nested extraction on a thread that already holds a
  cursor is admitted at once.
- An idle cursor keeps its archive open with read/write/delete sharing: while
  it is retained, renaming another file over the archive fails
  (ERROR_ACCESS_DENIED); rewriting or deleting it succeeds.

Memory: libarchive does not report a cursor's dictionary size, so each reader
estimates it once as its unpacked size rounded up to a power of two between
1 MiB and 64 MiB (7-Zip's and WinRAR's default dictionaries stop at 64 MiB and
are never larger than the data), plus 2 MiB of buffers.
