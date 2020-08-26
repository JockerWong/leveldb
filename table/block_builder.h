// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

namespace leveldb {

struct Options;

class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Reset the contents as if the BlockBuilder was just constructed.
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  // 向Block中添加一个kv对
  // 要求：上次Reset()之后没有调用过Finish()。
  // 要求：key比所有已添加的key都“大”。
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // 结束构造block，并返回一个引用block内容的Slice。
  // 返回的Slice在该builder生命内，或者直到调用Reset()之前，保持有效
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  // 返回正在构建的block的当前（未压缩）大小的估值
  size_t CurrentSizeEstimate() const;

  // Return true iff no entries have been added since the last Reset()
  bool empty() const { return buffer_.empty(); }

 private:
  const Options* options_;
  // Destination buffer
  // 目标buffer。
  // 保存kv对数据，末尾还保存所有restart点位置，和restart点数量
  std::string buffer_;
  // Restart points
  // 重新开始压缩的点（buffer_中的下标）
  // 每Options::block_restart_interval次添加key之后，重新开始压缩
  std::vector<uint32_t> restarts_;
  // Number of entries emitted since restart
  // restart之后发布的条目数量
  int counter_;
  bool finished_;                   // Has Finish() been called?
  // 最后一个添加的key
  std::string last_key_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
