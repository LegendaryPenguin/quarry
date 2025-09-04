#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"

namespace quarry {

// The on-disk segment layout, owned by the writer and read back by the reader.
//
//   [ header 32 bytes ]
//   [ column 0 chunk ] [ column 1 chunk ] ... [ column n-1 chunk ]
//   [ footer payload ] [ footer crc32 u32 ] [ footer offset u64 ]
//
// Each column chunk is a self-describing encoded blob followed by its own crc32,
// and chunks are contiguous so one column is one sequential read. The footer
// offset is the last eight bytes of the file, which is what lets a reader get
// the chunk table without scanning: read the tail, seek once, done. Putting it
// at the front instead would mean either a fixed-size table or a second pass to
// rewrite the header after the data is sized.
namespace segfmt {

// 8 bytes, no terminator: a magic that reads as text makes a truncated or
// mis-pathed file obvious in a hex dump.
constexpr char kMagic[8] = {'Q', 'U', 'A', 'R', 'R', 'Y', 'S', '1'};
constexpr uint32_t kVersion = 1;

constexpr uint64_t kHeaderBytes = 32;

// crc32 plus the footer offset.
constexpr uint64_t kFooterTrailerBytes = 12;

constexpr char kFooterMagic[4] = {'Q', 'F', 'T', 'R'};

// Each chunk's bytes end with a crc32 over everything before it in the chunk, so
// ColumnChunk::length covers payload + checksum.
constexpr uint64_t kChunkChecksumBytes = 4;

}  // namespace segfmt

// FileSegmentWriter accumulates batches in memory and writes one segment file on
// Finish.
//
// Buffering the whole segment is what makes a single encoding decision per column
// possible: the encoding is chosen from the measured properties of all the rows,
// which cannot be known while they are still arriving. It also means a segment is
// never partially visible — the file appears complete or not at all.
class FileSegmentWriter final : public SegmentWriter {
public:
    FileSegmentWriter(std::string path, Schema schema, uint64_t id);
    ~FileSegmentWriter() override;

    FileSegmentWriter(const FileSegmentWriter&) = delete;
    FileSegmentWriter& operator=(const FileSegmentWriter&) = delete;

    Status Append(const Batch& batch) override;
    Result<SegmentMeta> Finish() override;

    const Schema& schema() const { return schema_; }

private:
    struct ColumnAccum;

    std::string path_;
    Schema schema_;
    uint64_t id_;
    uint64_t row_count_ = 0;
    bool finished_ = false;

    std::vector<std::unique_ptr<ColumnAccum>> columns_;
};

}  // namespace quarry
