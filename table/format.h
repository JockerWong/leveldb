// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_FORMAT_H_
#define STORAGE_LEVELDB_TABLE_FORMAT_H_

#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/table_builder.h"

namespace leveldb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// BlockHandle is a pointer to the extent of a file that stores a data
// block or a meta block.
// BlockHandle是一个指向存储一个data block或一个meta block的文件范围的指针。
class BlockHandle {
 public:
  // Maximum encoding length of a BlockHandle
  enum { kMaxEncodedLength = 10 + 10 };

  BlockHandle();

  // The offset of the block in the file.
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  // 初始化为全1（无效）
  uint64_t offset_;
  // 初始化为全1（无效），这个大小算block内容的大小，不算block的压缩类型和校验和
  uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
// Footer 封装了存储在每个Table文件（*.sst|*.ldb）末尾的固定格式
class Footer {
 public:
  // Encoded length of a Footer.  Note that the serialization of a
  // Footer will always occupy exactly this many bytes.  It consists
  // of two block handles and a magic number.
  // Footer的编码长度。注意，Footer的序列化始终占用这么多字节（48字节）。它包含两个
  // block句柄（每个20字节）和一个魔法数字（8字节）。
  enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

  Footer() = default;

  // The block handle for the metaindex block of the table
  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  // The block handle for the index block of the table
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// kTableMagicNumber was picked by running
//    echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

// 1-byte type + 32-bit crc
static const size_t kBlockTrailerSize = 5;

struct BlockContents {
  // Actual contents of data
  // 解压缩后（如果需要）的，不包含type和crc的，真实数据内容
  Slice data;
  // True iff data can be cached
  // 比如PosixMmapReadableFile，它有文件的一段内存映射，所以，如果是未经压缩
  // 的话，就可以直接使用这段内存中的数据，因此也就不需要在Cache中缓存。
  bool cachable;
  // True iff caller should delete[] data.data()
  // 如果在RandomAccessFile::Read()中，没有将内容读取到调用者传递的堆中申请
  // 的一段内存中，就说明没有使用调用者申请的堆中内存，因此设置为false；
  // 反之，如果读取到调用者传递的堆中申请的一段内存中，说明这段内存在不需要使
  // 用时，需要调用者通过delete[]来释放，因此设置为true。
  bool heap_allocated;
};

// Read the block identified by "handle" from "file".  On failure
// return non-OK.  On success fill *result and return OK.
// 从“file”中读取“handle”标识的block。如果失败，则返回非OK。如果成功，则填充
// *result 并返回OK。
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

// Implementation details follow.  Clients should ignore,

inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FORMAT_H_
