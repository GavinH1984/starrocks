// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/olap/rowset/segment_v2/segment.h

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>
#include <memory> // for unique_ptr
#include <string>
#include <vector>

#include "common/statusor.h"
#include "gen_cpp/olap_file.pb.h"
#include "gen_cpp/segment.pb.h"
#include "gutil/macros.h"
#include "storage/fs/block_manager.h"
#include "storage/rowset/page_handle.h"
#include "storage/rowset/page_pointer.h"
#include "storage/short_key_index.h"
#include "storage/tablet_schema.h"
#include "util/faststring.h"
#include "util/once.h"

namespace starrocks {

class TabletSchema;
class ShortKeyIndexDecoder;
class Schema;
class StorageReadOptions;

namespace fs {
class BlockManager;
}

namespace vectorized {
class ChunkIterator;
class Schema;
class SegmentIterator;
class SegmentReadOptions;
} // namespace vectorized

class BitmapIndexIterator;
class ColumnReader;
class ColumnIterator;
class Segment;
using SegmentSharedPtr = std::shared_ptr<Segment>;
using ChunkIteratorPtr = std::shared_ptr<vectorized::ChunkIterator>;

// A Segment is used to represent a segment in memory format. When segment is
// generated, it won't be modified, so this struct aimed to help read operation.
// It will prepare all ColumnReader to create ColumnIterator as needed.
// And user can create a RowwiseIterator through new_iterator function.
//
// NOTE: This segment is used to a specified TabletSchema, when TabletSchema
// is changed, this segment can not be used any more. For example, after a schema
// change finished, client should disable all cached Segment for old TabletSchema.
class Segment : public std::enable_shared_from_this<Segment> {
    struct private_type;

public:
    static StatusOr<std::shared_ptr<Segment>> open(MemTracker* mem_tracker, std::shared_ptr<fs::BlockManager> blk_mgr,
                                                   const std::string& filename, uint32_t segment_id,
                                                   const TabletSchema* tablet_schema,
                                                   size_t* footer_length_hint = nullptr,
                                                   const FooterPointerPB* partial_rowset_footer = nullptr);

    static Status parse_segment_footer(fs::ReadableBlock* rblock, SegmentFooterPB* footer, size_t* footer_length_hint,
                                       const FooterPointerPB* partial_rowset_footer);

    Segment(const private_type&, std::shared_ptr<fs::BlockManager> blk_mgr, std::string fname, uint32_t segment_id,
            const TabletSchema* tablet_schema, MemTracker* mem_tracker);

    ~Segment() = default;

    // Returns `EndOfFile` if |read_options| has predicate and no record in this segment
    // matched with the predicate.
    StatusOr<ChunkIteratorPtr> new_iterator(const vectorized::Schema& schema,
                                            const vectorized::SegmentReadOptions& read_options);

    uint64_t id() const { return _segment_id; }

    // TODO: remove this method, create `ColumnIterator` via `ColumnReader`.
    Status new_column_iterator(uint32_t cid, ColumnIterator** iter);

    Status new_bitmap_index_iterator(uint32_t cid, BitmapIndexIterator** iter);

    size_t num_short_keys() const { return _tablet_schema->num_short_key_columns(); }

    uint32_t num_rows_per_block() const {
        DCHECK(_load_index_once.has_called() && _load_index_once.stored_result().ok());
        return _sk_index_decoder->num_rows_per_block();
    }

    ShortKeyIndexIterator lower_bound(const Slice& key) const {
        DCHECK(_load_index_once.has_called() && _load_index_once.stored_result().ok());
        return _sk_index_decoder->lower_bound(key);
    }

    ShortKeyIndexIterator upper_bound(const Slice& key) const {
        DCHECK(_load_index_once.has_called() && _load_index_once.stored_result().ok());
        return _sk_index_decoder->upper_bound(key);
    }

    // This will return the last row block in this segment.
    // NOTE: Before call this function , client should assure that
    // this segment is not empty.
    uint32_t last_block() const {
        DCHECK(_load_index_once.has_called() && _load_index_once.stored_result().ok());
        DCHECK(num_rows() > 0);
        return _sk_index_decoder->num_items() - 1;
    }

    size_t num_columns() const { return _column_readers.size(); }

    const ColumnReader* column(size_t i) const { return _column_readers[i].get(); }

    int64_t mem_usage() {
        int64_t size = sizeof(Segment) + _sk_index_handle.mem_usage();
        if (_sk_index_decoder != nullptr) {
            size += _sk_index_decoder->mem_usage();
        }
        return size;
    }

    MemTracker* mem_tracker() const { return _mem_tracker; }

    fs::BlockManager* block_manager() const { return _block_mgr.get(); }

    bool keep_in_memory() const { return _tablet_schema->is_in_memory(); }

    const std::string& file_name() const { return _fname; }

    uint32_t num_rows() const { return _num_rows; }

private:
    Segment(const Segment&) = delete;
    const Segment& operator=(const Segment&) = delete;

    struct private_type {
        explicit private_type(int) {}
    };

    // open segment file and read the minimum amount of necessary information (footer)
    Status _open(MemTracker* mem_tracker, size_t* footer_length_hint, const FooterPointerPB* partial_rowset_footer);
    Status _create_column_readers(MemTracker* mem_tracker, SegmentFooterPB* footer);
    // Load and decode short key index.
    // May be called multiple times, subsequent calls will no op.
    Status _load_index(MemTracker* mem_tracker);

    StatusOr<ChunkIteratorPtr> _new_iterator(const vectorized::Schema& schema,
                                             const vectorized::SegmentReadOptions& read_options);

    void _prepare_adapter_info();

    friend class SegmentIterator;
    friend class vectorized::SegmentIterator;

    std::shared_ptr<fs::BlockManager> _block_mgr;
    std::string _fname;
    const TabletSchema* _tablet_schema;
    uint32_t _segment_id = 0;
    uint32_t _num_rows = 0;
    PagePointer _short_key_index_page;
    MemTracker* _mem_tracker;

    // ColumnReader for each column in TabletSchema. If ColumnReader is nullptr,
    // This means that this segment has no data for that column, which may be added
    // after this segment is generated.
    std::vector<std::unique_ptr<ColumnReader>> _column_readers;

    // used to guarantee that short key index will be loaded at most once in a thread-safe way
    StarRocksCallOnce<Status> _load_index_once;
    // used to hold short key index page in memory
    PageHandle _sk_index_handle;
    // short key index decoder
    std::unique_ptr<ShortKeyIndexDecoder> _sk_index_decoder;

    // Actual storage type for each column, used to rewrite the input readoptions
    std::unique_ptr<std::vector<FieldType>> _column_storage_types;
    // When reading old type format data this will be set to true.
    bool _needs_chunk_adapter = false;
    // When the storage types is different with TabletSchema
    bool _needs_block_adapter = false;
};

} // namespace starrocks
