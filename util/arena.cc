// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

static const int kBlockSize = 4096;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

Arena::~Arena() {
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}

// 申请bytes大小内存的应急接口
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    // 大小超过1/4块大小，则单独申请，避免剩余字节浪费空间
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  // （当前剩余不足）浪费当前块的剩余空间，重新申请一块内存
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

// 起始地址对齐地，申请bytes大小内存
char* Arena::AllocateAligned(size_t bytes) {
  // 地址要求至少8字节对齐，且是2的整数次幂
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1); // 当前指针模align取余
  // 由于当前没有对齐，这次为了对齐起始地址需要额外申请的内存
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);
  size_t needed = bytes + slop;		// 对齐之后，实际申请的内存大小
  char* result;
  if (needed <= alloc_bytes_remaining_) {
  	// 当前剩余内存足够
    result = alloc_ptr_ + slop;	// 对齐起始地址
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
  	// 当前剩余内存不足
    // AllocateFallback always returned aligned memory
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

// new申请一个新块
char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  // 原子的无锁加
  // ??? 为什么要多一个指针大小 ???
  // 【猜测】是应为blocks_容器中多了一个指针吗
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace leveldb
