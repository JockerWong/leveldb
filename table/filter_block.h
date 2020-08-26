// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A filter block is stored near the end of a Table file.  It contains
// filters (e.g., bloom filters) for all data blocks in the table combined
// into a single filter block.

#ifndef STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

class FilterPolicy;

// A FilterBlockBuilder is used to construct all of the filters for a
// particular Table.  It generates a single string which is stored as
// a special block in the Table.
//
// The sequence of calls to FilterBlockBuilder must match the regexp:
//      (StartBlock AddKey*)* Finish
// FilterBlockBuilder用于为指定Table构造所有filter。
// 它生成一个string，作为一个特殊block存储在Table中。
// 对FilterBlockBuilder的一系列调用必须匹配：
//      (StartBlock AddKey*)* Finish
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy*);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  // Flattened key contents
  // 扁平地保存了所有的key
  std::string keys_;
  // Starting index in keys_ of each key
  // 每个key在keys_中的开始索引
  // 其数量代表，keys_中当前还未用于生成filter的key的数量
  std::vector<size_t> start_;
  // Filter data computed so far
  // 到目前为止，计算的过滤器数据
  std::string result_;
  // policy_->CreateFilter() argument
  // 底层数据还是存储在keys_中
  std::vector<Slice> tmp_keys_;
  // 生成的每个filter在result_开始位置的偏移量
  // 其数量代表，已生成的filter的数量
  std::vector<uint32_t> filter_offsets_;
};

class FilterBlockReader {
 public:
  // REQUIRES: "contents" and *policy must stay live while *this is live.
  // param[in] policy : 过滤策略
  // param[in] contents : filter block内容
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);
  bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  const FilterPolicy* policy_;
  // Pointer to filter data (at block-start)
  // 指向filter 0（在filter block的开头）
  const char* data_;
  // Pointer to beginning of offset array (at block-end)
  // 指向filter offset数组的开头，也就是filter 0 的offset。
  // （在filter block的末尾，后面仅有一个base的对数）
  const char* offset_;
  // Number of entries in offset array
  // filter offset数量，对应filter数量
  size_t num_;
  // Encoding parameter (see kFilterBaseLg in .cc file)
  // base的对数
  size_t base_lg_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
