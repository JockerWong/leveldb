// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>

#include "leveldb/iterator.h"

namespace leveldb {

struct BlockContents;
class Comparator;

class Block {
 public:
  // Initialize the block with the specified contents.
  explicit Block(const BlockContents& contents);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  ~Block();

  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  class Iter;

  uint32_t NumRestarts() const;

  // 内容与BlockBuilder::buffer_对应
  const char* data_;
  size_t size_;
  uint32_t restart_offset_;  // Offset in data_ of restart array
  // Block owns data_[]
  // Block 是否拥有 data_[]的所有权，比如PosixMmapReadableFile，它有文件的一段内存映射，
  // 在没有压缩的情况下，可以直接使用其中的数据，不需要Block自身在持有一份
  bool owned_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
