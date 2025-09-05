#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quarry/batch.hpp"
#include "quarry/schema.hpp"
#include "quarry/status.hpp"
#include "quarry/storage.hpp"

namespace quarry {

// SegmentFile is what the footer yields: the chunk table a scan needs in order to
// decide whether to open the segment, plus the column names so a reader can hand
// back batches whose schema matches the table's.
struct SegmentFile {
    SegmentMeta meta;
    Schema schema;
};

// ReadSegmentFile recovers metadata by reading the tail of the file and then the
// footer it points at — two reads, no scan, independent of segment size. This is
// the call a catalog makes at startup, and it is deliberately separate from
// opening a reader so that pruning a segment costs nothing but the metadata the
// catalog already holds.
//
// Anything that does not add up — bad magic, a footer offset outside the file, a
// footer whose checksum fails, a chunk range that runs past the end — is reported
// as kCorruption rather than producing a half-believable SegmentMeta.
Result<SegmentFile> ReadSegmentFile(const std::string& path);

// FileSegmentReader streams one segment's projected columns as batches.
//
// It reads exactly the byte ranges of the projected chunks and nothing else: the
// header for validation, then one pread per projected column. A column that was
// not asked for is never touched, which is the only reason bytes_read() can be
// used as evidence that projection pushdown is real.
class FileSegmentReader final : public SegmentReader {
public:
    // schema may be empty, in which case columns are named by position. Passing
    // the schema from ReadSegmentFile is what makes the output batch usable by
    // name.
    FileSegmentReader(SegmentMeta meta, std::vector<size_t> projection, Schema schema = Schema());
    ~FileSegmentReader() override;

    FileSegmentReader(const FileSegmentReader&) = delete;
    FileSegmentReader& operator=(const FileSegmentReader&) = delete;

    Status Open() override;

    // Next fills out with up to kBatchSize rows. done is set when the segment is
    // exhausted, at which point out holds no rows; a caller therefore never has to
    // distinguish "last batch" from "one past the end".
    Status Next(Batch& out, bool& done) override;

    Status Close() override;

    uint64_t bytes_read() const override { return bytes_read_; }

    // The schema of the batches Next produces, available after construction.
    const Schema& output_schema() const { return output_schema_; }

private:
    SegmentMeta meta_;
    std::vector<size_t> projection_;
    Schema output_schema_;

    // One fully decoded column per projected index. Decoding a whole column at
    // open time trades memory for a single sequential read per column; a segment
    // is sized to be a unit of work, so the peak is bounded by the segment, not by
    // the table.
    std::vector<Vector> decoded_;

    uint64_t position_ = 0;
    uint64_t bytes_read_ = 0;
    bool open_ = false;
};

}  // namespace quarry
