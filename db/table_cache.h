// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Thread-safe (provides internal synchronization)

#ifndef STORAGE_LEVELDB_DB_TABLE_CACHE_H_
#define STORAGE_LEVELDB_DB_TABLE_CACHE_H_

#include <cstdint>
#include <string>

#include "db/dbformat.h"
#include "leveldb/cache.h"
#include "leveldb/table.h"
#include "port/port.h"

namespace leveldb {

class Env;

class TableCache {
 public:
  TableCache(const std::string& dbname, const Options& options, int entries);
  ~TableCache();

  // Return an iterator for the specified file number (the corresponding
  // file length must be exactly "file_size" bytes).  If "tableptr" is
  // non-null, also sets "*tableptr" to point to the Table object
  // underlying the returned iterator, or to nullptr if no Table object
  // underlies the returned iterator.  The returned "*tableptr" object is owned
  // by the cache and should not be deleted, and is valid for as long as the
  // returned iterator is live.
  // 为指定的文件序号（对应的文件长度必须是file_size字节）返回一个迭代器。如果tableptr
  // 非空，设置*tableptr指向返回迭代器下的Table对象，如果返回迭代器下没有Table对象，设
  // 置*tableptr为nullptr。返回的*tableptr对象属于该Cache，不应该被删除，且只要返回的
  // 迭代器还存活，它就有效。
  // 【理解】这应该是基于LRU淘汰机制，而迭代器存活，说明该Table还在被使用。
  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number,
                        uint64_t file_size, Table** tableptr = nullptr);

  // If a seek to internal key "k" in specified file finds an entry,
  // call (*handle_result)(arg, found_key, found_value).
  // 在指定文件中搜索内部key “k”，如果找到了，则调用
  // handle_result(arg, 找到的key, 找到的value)
  // 【实现】在cache_中找到file_number对应的映射，并得到对应的Table。
  //     在Table中找到这个k，执行(*handle_result)(arg, k, v)
  Status Get(const ReadOptions& options, uint64_t file_number,
             uint64_t file_size, const Slice& k, void* arg,
             void (*handle_result)(void*, const Slice&, const Slice&));

  // Evict any entry for the specified file number
  // 为指定文件淘汰所有条目
  void Evict(uint64_t file_number);

 private:
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

  Env* const env_;
  const std::string dbname_;
  const Options& options_;
  // 一个Cache，构造函数中构造了一个LRU淘汰策略的Cache（ShardedLRUCache）
  // key: file number（对应dbname_目录下的*.ldb|*.sst）
  // value: 堆中TableAndFile的地址（其中的file和table也指向堆中内存）
  Cache* cache_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_TABLE_CACHE_H_
