// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/memtable.h"
#include "db/dbformat.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "util/coding.h"

namespace leveldb {

// param[in] data: 有“长度前缀”的数据首地址
// return: data中根据“长度前缀”读取到的数据Slice
static Slice GetLengthPrefixedSlice(const char* data) {
  uint32_t len;
  const char* p = data;
  // 读取变长的数值len
  p = GetVarint32Ptr(p, p + 5, &len);  // +5: we assume "p" is not corrupted
  return Slice(p, len);
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), refs_(0), table_(comparator_, &arena_) {}

MemTable::~MemTable() { assert(refs_ == 0); }

// 近似内存使用量
size_t MemTable::ApproximateMemoryUsage() { return arena_.MemoryUsage(); }

// param[in] aptr,bptr: 两个包含“长度前缀”的内部key数据首地址
// return: 内部key的比价结果（a>b则为正；a<b则为负）
int MemTable::KeyComparator::operator()(const char* aptr,
                                        const char* bptr) const {
  // Internal keys are encoded as length-prefixed strings.
  // 从两个参数中分别读取出内部key
  Slice a = GetLengthPrefixedSlice(aptr);
  Slice b = GetLengthPrefixedSlice(bptr);
  return comparator.Compare(a, b);	// 通过内部key比较器对两个内部key进行比较
}

// Encode a suitable internal key target for "target" and return it.
// Uses *scratch as scratch space, and the returned pointer will point
// into this scratch space.
// 在“草稿”scratch中编码target，并返回
static const char* EncodeKey(std::string* scratch, const Slice& target) {
  scratch->clear();
  PutVarint32(scratch, target.size());				// 变长的压入target的长度
  scratch->append(target.data(), target.size());	// 压入target内容
  return scratch->data();
}

// 一个代理类
class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

  MemTableIterator(const MemTableIterator&) = delete;
  MemTableIterator& operator=(const MemTableIterator&) = delete;

  ~MemTableIterator() override = default;

  bool Valid() const override { return iter_.Valid(); }
  void Seek(const Slice& k) override { iter_.Seek(EncodeKey(&tmp_, k)); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }
  // 长度前缀后面的真实数据（internal_key）
  Slice key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  // internal_key后面的value
  Slice value() const override {
    Slice key_slice = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(key_slice.data() + key_slice.size());
  }

  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;	// MemTable内部SkipList的迭代器
  std::string tmp_;  // For passing to EncodeKey
};

Iterator* MemTable::NewIterator() { return new MemTableIterator(&table_); }

// 向内部SkipList中插入key<用户key长度+8|用户key|序列号和值类型信息|用户value长度|用户value>
void MemTable::Add(SequenceNumber s, ValueType type, const Slice& key,
                   const Slice& value) {
  // Format of an entry is concatenation of:
  //  key_size     : varint32 of internal_key.size()
  //  key bytes    : char[internal_key.size()]
  //  value_size   : varint32 of value.size()
  //  value bytes  : char[value.size()]
  size_t key_size = key.size();
  size_t val_size = value.size();
  size_t internal_key_size = key_size + 8;	// 8字节用于存储序列号和值类型
  // 变长的压缩存储internal_key_size值的长度 + internal_key_size长度的数据长度 +
  // 变长的压缩存储val_size值的长度 + val_size长度的数据长度
  const size_t encoded_len = VarintLength(internal_key_size) +
                             internal_key_size + VarintLength(val_size) +
                             val_size;
  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);	// 变长的压入internal_key_size
  std::memcpy(p, key.data(), key_size);				// 压入key值
  p += key_size;
  EncodeFixed64(p, (s << 8) | type);				// 压入8字节的：序列号（高7字节）和值类型（低1字节）
  p += 8;
  p = EncodeVarint32(p, val_size);					// 变长的压入val_size
  std::memcpy(p, value.data(), val_size);			// 压入value值
  assert(p + val_size == buf + encoded_len);
  table_.Insert(buf);	// SkipList中存储的是将kv对，以及序列号等信息一起序列化的数据
}

// param[in] key : 要查找的key
// param[out] value : 如果能找到value，存储到value中，返回true
// param[out] s : 如果能找到删除操作，存储NotFound到s中，并返回true
// reutrn : 其他情况返回false
bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) {
  Slice memkey = key.memtable_key();// 适用于MemTable查找的key<user_key_size+8|internal_key>
  Table::Iterator iter(&table_);	// 内部SkipList的迭代器
  iter.Seek(memkey.data());
  if (iter.Valid()) {
  	// 内部SkipList中有该key
    // entry format is:
    //    klength  varint32
    //    userkey  char[klength]
    //    tag      uint64
    //    vlength  varint32
    //    value    char[vlength]
    // Check that it belongs to same user key.  We do not check the
    // sequence number since the Seek() call above should have skipped
    // all entries with overly large sequence numbers.
    // 不检查序列号，因为上面Seek()调用已经跳过了所有序列号过大的条目
    const char* entry = iter.key();	// internal_key
    uint32_t key_length;			// internal_key length
    const char* key_ptr = GetVarint32Ptr(entry, entry + 5, &key_length);// 解出变长的internal_key_length
	// 比较user_key
	if (comparator_.comparator.user_comparator()->Compare(
            Slice(key_ptr, key_length - 8), key.user_key()) == 0) {
      // Correct user key
      // 是正确的user_key
      const uint64_t tag = DecodeFixed64(key_ptr + key_length - 8);	// 序列号及值类型
      switch (static_cast<ValueType>(tag & 0xff)) {
        case kTypeValue: {
          Slice v = GetLengthPrefixedSlice(key_ptr + key_length);	// user_value
          value->assign(v.data(), v.size());
          return true;
        }
        case kTypeDeletion:
	      // 删除操作，则返回没有找到
          *s = Status::NotFound(Slice());
          return true;
      }
    }
  }
  return false;
}

}  // namespace leveldb
