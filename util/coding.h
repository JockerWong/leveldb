// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Endian-neutral encoding:
// * Fixed-length numbers are encoded with least-significant byte first
// * In addition we support variable length "varint" encoding
// * Strings are encoded prefixed by their length in varint format
//
// 字节序无关的编码：
// 固定长度的数字，最低有效字节在最前面
// 此外，我们支持变长的“varint”编码
// 字符串编码，以变长格式的长度作为前缀

#ifndef STORAGE_LEVELDB_UTIL_CODING_H_
#define STORAGE_LEVELDB_UTIL_CODING_H_

#include <cstdint>
#include <cstring>
#include <string>

#include "leveldb/slice.h"
#include "port/port.h"

namespace leveldb {

// Standard Put... routines append to a string
// 标准的 Put... 例程，追加到一个string

// 向dst追加一个32位定长的value，低有效字节在前
void PutFixed32(std::string* dst, uint32_t value);
// 向dst追加一个64位定长的value，低有效字节在前
void PutFixed64(std::string* dst, uint64_t value);
// 向dst追加一个32位变长的value
void PutVarint32(std::string* dst, uint32_t value);
// 向dst追加一个64位变长的value
void PutVarint64(std::string* dst, uint64_t value);
// 向dst追加一个带“32位变长长度前缀”的Slice数据
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);

// Standard Get... routines parse a value from the beginning of a Slice
// and advance the slice past the parsed value.
// 标准Get...例程，从Slice的开头解析出一个value，并将Slice向前推进，越过已解析
// 的value。

// 从input的开头读取一个32位变长的value，并将input向前推进
// 成功：返回true；失败：返回false，input无变化
bool GetVarint32(Slice* input, uint32_t* value);
// 从input的开头读取一个64位变长的value，并将input向前推进
// 成功：返回true；失败：返回false，input无变化
bool GetVarint64(Slice* input, uint64_t* value);
// 从input的开头读取一个带“32位变长长度前缀”的Slice到result，并将input向前推进
// 成功：返回true；失败：返回false，此时input可能向前推进了一个长度前缀
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

// Pointer-based variants of GetVarint...  These either store a value
// in *v and return a pointer just past the parsed value, or return
// nullptr on error.  These routines only look at bytes in the range
// [p..limit-1]
// GetVarint... 基于指针的变种，它们要么在*v存储一个value并返回指向（数据p）
// 被解析数据之后地址的指针，要么在遇到错误时返回nullptr。
// 这些例程只访问 [p..limit) 范围内的字节。

// 从 [p, limit) 返回（开头）读取32位变长的v，并返回下一地址
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* v);
// 从 [p, limit) 返回（开头）读取64位变长的v，并返回下一地址
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* v);

// Returns the length of the varint32 or varint64 encoding of "v"
// 返回 v 的 varint32 或 varint64 编码的长度
int VarintLength(uint64_t v);

// Lower-level versions of Put... that write directly into a character buffer
// and return a pointer just past the last byte written.
// REQUIRES: dst has enough space for the value being written
// Put... 的低级版本，直接写入到字节buffer，并返回指向最后写入字节之后的指针。
// 要求：dst有足够的空间用于写入value。
// 从二进制最低位开始，每7位存入一个字节，如果高位还有有效位则该字节最高位置"1"，否则置"0"。
// 返回dst中最后写入字节之后的地址
// 因此一个4字节的v最坏情况，需要存到5字节中（4B/7b向上取整）
//     一个8字节的v最坏情况，需要存到10字节中（8B/7b向上取整）
// 并且，依然是低有效字节在前

// 向dst中存储变长32位的value，返回下一地址
char* EncodeVarint32(char* dst, uint32_t value);
// 向dst中存储变长64位的value，返回下一地址
char* EncodeVarint64(char* dst, uint64_t value);

// Lower-level versions of Put... that write directly into a character buffer
// REQUIRES: dst has enough space for the value being written
// 直接向字节buffer中写入的Put...的低级版本，
// 要求：dst有足够空间

// 向dst中存储定长32位的value，低有效字节在前
inline void EncodeFixed32(char* dst, uint32_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

  // Recent clang and gcc optimize this to a single mov / str instruction.
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
}

// 向dst中存储定长64位的value，低有效字节在前
inline void EncodeFixed64(char* dst, uint64_t value) {
  uint8_t* const buffer = reinterpret_cast<uint8_t*>(dst);

  // Recent clang and gcc optimize this to a single mov / str instruction.
  // 最近的clang和gcc将其优化为一条单独的 mov / str 指令
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  buffer[4] = static_cast<uint8_t>(value >> 32);
  buffer[5] = static_cast<uint8_t>(value >> 40);
  buffer[6] = static_cast<uint8_t>(value >> 48);
  buffer[7] = static_cast<uint8_t>(value >> 56);
}

// Lower-level versions of Get... that read directly from a character buffer
// without any bounds checking.
// Get... 的低级版本，直接从字符buffer读取，没有任何边界检查

// 从ptr中读取定长32位的整数，并返回。低有效字节在前
inline uint32_t DecodeFixed32(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

// 从ptr中读取定长32位的整数，并返回。低有效字节在前
inline uint64_t DecodeFixed64(const char* ptr) {
  const uint8_t* const buffer = reinterpret_cast<const uint8_t*>(ptr);

  // Recent clang and gcc optimize this to a single mov / ldr instruction.
  return (static_cast<uint64_t>(buffer[0])) |
         (static_cast<uint64_t>(buffer[1]) << 8) |
         (static_cast<uint64_t>(buffer[2]) << 16) |
         (static_cast<uint64_t>(buffer[3]) << 24) |
         (static_cast<uint64_t>(buffer[4]) << 32) |
         (static_cast<uint64_t>(buffer[5]) << 40) |
         (static_cast<uint64_t>(buffer[6]) << 48) |
         (static_cast<uint64_t>(buffer[7]) << 56);
}

// Internal routine for use by fallback path of GetVarint32Ptr
// GetVarint32Ptr() 的后备路径使用的内部例程
// 从 [p,limit) 地址范围，读取32位变长的value，并返回被解析数据的下一地址
// 如果读取失败，返回nullptr
// 【说明】因为32位变长的数值，大多数都在 2^7 以内，可以用1个字节编码，这样就可以将
//     GetVarint32Ptr() 定义为inline，减少一次函数调用的开销，如果超过1个字节，则
//     通过调用较为复杂的 GetVarint32PtrFallback() 实现
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value);
inline const char* GetVarint32Ptr(const char* p, const char* limit,
                                  uint32_t* value) {
  if (p < limit) {
    uint32_t result = *(reinterpret_cast<const uint8_t*>(p));
    if ((result & 128) == 0) {
	    // 第一个最高位为0的字节为压缩编码的最高字节，说明这个变长value只有1字节
      *value = result;
      return p + 1;
    }
  }
  return GetVarint32PtrFallback(p, limit, value);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_CODING_H_
