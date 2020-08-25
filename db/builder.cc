// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  // 文件名：“${dbname}/${meta->number}.ldb”
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file;
    // 创建一个新文件，以及一个向其写入的WritableFile
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }

    TableBuilder* builder = new TableBuilder(options, file);
    // 第一个位置的内部key，是最小内部key
    meta->smallest.DecodeFrom(iter->key());
    // 用iter执行遍历，将所有kv对添加到builder中
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      meta->largest.DecodeFrom(key);
      builder->Add(key, iter->value());
    }

    // Finish and check for builder errors
    s = builder->Finish();
    if (s.ok()) {
      // 最终生成有序表文件（*.sst|*.ldb）的大小
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
      // 刷到持久化介质中
      s = file->Sync();
    }
    if (s.ok()) {
      // 关闭可写文件（对应的磁盘文件）
      s = file->Close();
    }
    delete file;
    file = nullptr;

    if (s.ok()) {
      // Verify that the table is usable
      // 验证Table是否可用
      Iterator* it = table_cache->NewIterator(ReadOptions(), meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->RemoveFile(fname);
  }
  return s;
}

}  // namespace leveldb
