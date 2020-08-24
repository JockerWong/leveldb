// Copyright (c) 2014 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_INCLUDE_DUMPFILE_H_
#define STORAGE_LEVELDB_INCLUDE_DUMPFILE_H_

#include <string>

#include "leveldb/env.h"
#include "leveldb/export.h"
#include "leveldb/status.h"

namespace leveldb {

// Dump the contents of the file named by fname in text format to
// *dst.  Makes a sequence of dst->Append() calls; each call is passed
// the newline-terminated text corresponding to a single item found
// in the file.
//
// Returns a non-OK result if fname does not name a leveldb storage
// file, or if the file cannot be read.
// 将名为fname的文件的内容以文本格式转储到（可写文件）*dst。
// 导致一系列的dst->Append()调用；每次调用都传递与文件中找到的一项相对应的
// 换行符结束的文本。
// 如果fname不是leveldb存储文件的名字，或者该文件不可读，则返回非OK结果。
LEVELDB_EXPORT Status DumpFile(Env* env, const std::string& fname,
                               WritableFile* dst);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_DUMPFILE_H_
