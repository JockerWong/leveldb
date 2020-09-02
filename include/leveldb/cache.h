// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A Cache is an interface that maps keys to values.  It has internal
// synchronization and may be safely accessed concurrently from
// multiple threads.  It may automatically evict entries to make room
// for new entries.  Values have a specified charge against the cache
// capacity.  For example, a cache where the values are variable
// length strings, may use the length of the string as the charge for
// the string.
//
// A builtin cache implementation with a least-recently-used eviction
// policy is provided.  Clients may use their own implementations if
// they want something more sophisticated (like scan-resistance, a
// custom eviction policy, variable cache sizing, etc.)
// Cache是将key映射到value的接口。它有内部同步，可以安全的从多个线程并发访问。
// 它能自动淘汰条目，为新条目腾出空间。value对Cache容量有指定的收费。例如，一个
// Cache，其中的value是变长字符串，可以使用字符串长度作为字符串的费用。
// 提供了有LRU淘汰策略的内建Cache实现。如果client想要更复杂的实现（比如抗扫描、定
// 制淘汰策略、可变Cache大小等），他们可以使用自己的实现。
#ifndef STORAGE_LEVELDB_INCLUDE_CACHE_H_
#define STORAGE_LEVELDB_INCLUDE_CACHE_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/slice.h"

namespace leveldb {

class LEVELDB_EXPORT Cache;

// Create a new cache with a fixed size capacity.  This implementation
// of Cache uses a least-recently-used eviction policy.
// 以固定大小的容量创建一个Cache。
// 该Cache的实现使用 LRU 的淘汰策略。
LEVELDB_EXPORT Cache* NewLRUCache(size_t capacity);

class LEVELDB_EXPORT Cache {
 public:
  Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Destroys all existing entries by calling the "deleter"
  // function that was passed to the constructor.
  // 通过调用（Insert()中传递的）deleter函数销毁所有现存条目。
  virtual ~Cache();

  // Opaque handle to an entry stored in the cache.
  // 存储在Cache中的一个条目的句柄
  struct Handle {};

  // Insert a mapping from key->value into the cache and assign it
  // the specified charge against the total cache capacity.
  //
  // Returns a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  //
  // When the inserted entry is no longer needed, the key and
  // value will be passed to "deleter".
  // 向Cache添加key->value的映射，并赋予它对于Cache总容量的指定费用。
  // 返回对应这个映射的句柄。不再需要返回的映射时，调用者必须调用this->Release(handle)。
  // 当插入的条目不再需要时，这个key和value将被传递给deleter函数。
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // If the cache has no mapping for "key", returns nullptr.
  //
  // Else return a handle that corresponds to the mapping.  The caller
  // must call this->Release(handle) when the returned mapping is no
  // longer needed.
  // 如果Cache没有对key的映射，返回nullptr。
  // 否则，返回与映射对应的句柄。当不再需要返回的映射时，调用者必须调用
  // this->Release(handle)。
  virtual Handle* Lookup(const Slice& key) = 0;

  // Release a mapping returned by a previous Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 释放前面Lookup()方法返回的映射。
  // 要求：句柄一定还没有被释放。
  // 要求：句柄必须是由*this的方法返回的。
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle returned by a
  // successful Lookup().
  // REQUIRES: handle must not have been released yet.
  // REQUIRES: handle must have been returned by a method on *this.
  // 返回一个Lookup()的成功调用返回的句柄中封装的value。
  // 要求：句柄一定还没有被释放。
  // 要求：句柄必须是由*this的方法返回的。
  virtual void* Value(Handle* handle) = 0;

  // If the cache contains entry for key, erase it.  Note that the
  // underlying entry will be kept around until all existing handles
  // to it have been released.
  // 如果Cache中包含key的条目，删除掉。
  // 注意，底层的条目一直保留到它的所有现存句柄全部被释放掉。
  virtual void Erase(const Slice& key) = 0;

  // Return a new numeric id.  May be used by multiple clients who are
  // sharing the same cache to partition the key space.  Typically the
  // client will allocate a new id at startup and prepend the id to
  // its cache keys.
  // 返回一个新的数值ID。可能被共享同一Cache的多个client使用，以划分key空间。
  // 通常client在启动时分配一个新ID，并将该ID插到每个缓存key的开始部分。
  virtual uint64_t NewId() = 0;

  // Remove all cache entries that are not actively in use.  Memory-constrained
  // applications may wish to call this method to reduce memory usage.
  // Default implementation of Prune() does nothing.  Subclasses are strongly
  // encouraged to override the default implementation.  A future release of
  // leveldb may change Prune() to a pure abstract method.
  // 修剪：删除所有未被活跃使用的Cache条目。内存受限的应用程序可能希望调用这个方法来减少
  // 内存使用量。Prune()的默认实现什么都不做。非常鼓励子类override这个默认实现。leveldb
  // 未来的发布版本可能将Prune()改为纯抽象方法。
  virtual void Prune() {}

  // Return an estimate of the combined charges of all elements stored in the
  // cache.
  // 返回Cache存储的所有元素的总费用的估值。
  virtual size_t TotalCharge() const = 0;

 private:
  void LRU_Remove(Handle* e);
  void LRU_Append(Handle* e);
  void Unref(Handle* e);

  // 没有找到这个Rep的定义和rep_的使用
  // 这里可参照TableBuilder和Table的设计，它们都是在源文件中定义类内Rep结构的。
  struct Rep;
  Rep* rep_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_CACHE_H_
