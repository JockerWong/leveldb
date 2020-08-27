// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

// 用于删除TableCache::cache_内的条目
static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

// 用于Cache释放一个Handle
// param[in] arg1 : 指向Cache的指针
// param[in] arg2 : 指向要释放的Handle的指针
static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

// param[in] dbname : 数据库名
// param[in] options : 选项
// param[in] entries : 内部cache_的容量
TableCache::TableCache(const std::string& dbname, const Options& options,
                       int entries)
    : env_(options.env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

// 从cache_中找到文件序号file_number，对应的映射，并将映射句柄存储到*handle。
// 如果cache_中没有该映射条目，则打开存储在file_number对应文件前file_size字节
// 范围的Table，并将对应的TableAndFile插入cache_。
// 如果成功，则返回OK；如果在插入cache_的过程中发生错误，则返回一个非OK状态。
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  // 从cache_中查找，有没有文件序号file_number对应的映射
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {
    // cache_中没有对应映射，就插入一个

    // 文件名："${dbname_}/${file_number}.ldb"
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      // 随机读取文件的对象创建失败，可能是因为文件名是旧版本的*.sst
      // "${dbname_}/${file_number}.sst"
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    // 随机读取文件的对象创建成功，打开存储在文件前file_size字节内的Table
    if (s.ok()) {
      s = Table::Open(options_, file, file_size, &table);
    }

    if (!s.ok()) {
      // 打开Table失败，释放资源
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
      // 我们不缓存错误结果，所以如果错误是暂时的，或有人修复了文件，我们会自动恢复。
    } else {
      // 打开Table成功，将随机读取文件的对象 和 打开的Table 绑定，
      // 以费用“1”插入到cache_中，并制定删除方法。
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  // 在cache_内找到file_number对应的映射
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  // 向迭代器注册一个清除三元组，
  // 在迭代器销毁时，调用 UnrefEntry(cache_, handle)，让cache_释放该Handle。
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  // 在cache_内找到file_number对应的映射
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    // 在Table中找到这个k，执行调用(*handle_result)(arg, k, v)
    // 然后释放这个Handle
    s = t->InternalGet(options, k, arg, handle_result);
    cache_->Release(handle);
  }
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
