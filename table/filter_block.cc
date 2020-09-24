// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
// 每2KB数据生成一个新filter
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

// param[in] block_offset : （下一个）block开始位置的偏移量
// 思路：先计算下一个data block的开始位置偏移，对应的filter索引，来判断当前是否
//       生成新filter。如果需要生成，那么keys_中保存的所有key信息全部用于生成接
//       下来的filter，如果由于导致执行到这里的、“刚刚写入到文件的data block”太
//       大，使得需要一次性生成多个filter，那么也只要第一个filter与该“data block”
//       的开始偏移量有关系，后面生成的filter为空filter。
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // 以2KB为base，偏移量block_offset对应filter的索引
  // 【说明】以Options::block_size对data block划分，每个block大小为4KB，因此，
  //     4KB的block数据触发一次TableBuilder::Flush()，进而执行到这里。而base是
  //     2KB，因此可能需要执行多次GenerateFilter()。
  //     然而，一个block的开始位置偏移量block_offset只能落在一个base范围内，也
  //     就是，只有block_offset落在的base，对应一个有效的filter，其他的filter
  //     是由于一个data block跨多个base而导致的空filter。
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  // 将
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

// 新添加一个key
void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

// 构建filter block结束：
// 1. 向filter block中最后一次生成filter
// 2. 向filter block中追加每个filter的offset
// 3. 向filter block中追加filter offset数组的偏移量
// 4. 向filter block中追加base的对数
// 返回的Slice在该builder生命期内，保持有效
Slice FilterBlockBuilder::Finish() {
  // 当前keys_中有key数据，最后生成一个filter
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  // result_末尾追加每个filter的偏移量
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  // result_末尾追加filter offset数组的偏移量
  PutFixed32(&result_, array_offset);
  // 最后，result_末尾追加base的对数
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

// 为当前keys_中的所有key生成一个filter，追加到result_中，
// 结束时清空keys_，start_
void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  // 将“下一个key”的开始位置加进来，为了简化每个key的长度计算，反正结束时会清空
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  // 为当前keys_中的所有key，生成一个filter，并追加到result_末尾。
  filter_offsets_.push_back(result_.size());
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n - 1];
  // 解析 filter offset数组的偏移量
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  // filter offset数组的偏移量 不可能超过n-5
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

// 判断 block_offset 对应的 filter i 中是否有关键字 key
bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  // 计算block_offset（开始位置）对应的filter索引
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // filter i 在data_中的偏移量
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    // filter i+1 在data_中的偏移量。
    // 特殊情况：最后一个filter时，limit解析出来的是filter 0 offset的偏移量，
    //    也就是最后一个filter的结束的下一个位置。
    // 总结：limit 为 filter i 的结束的下一个位置。
    //     filter i 的数据在 [start, limit)范围内。
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    // offset_-data_ 是filter 0 offset的偏移量，即最后一个filter结束的下一个位置。
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      // 用 policy_ 判断 filter i 中是否有key
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      // 空filter，数据中没有以block_offset开始的data block，也就不会匹配到key
      return false;
    }
  }
  // Errors are treated as potential matches
  // 有问题，当做可能会匹配来处理
  return true;
}

}  // namespace leveldb
