// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/filename.h"

#include <cassert>
#include <cstdio>

#include "db/dbformat.h"
#include "leveldb/env.h"
#include "util/logging.h"

namespace leveldb {

// A utility routine: write "data" to the named file and Sync() it.
Status WriteStringToFileSync(Env* env, const Slice& data,
                             const std::string& fname);

// "${dbname}/" + 最低6位的文件编号 + ".${suffix}"
// 例："database/123456.ldb"
static std::string MakeFileName(const std::string& dbname, uint64_t number,
                                const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

std::string LogFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "ldb");
}

std::string SSTTableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "dbtmp");
}

std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

// Return the name of the old info log file for "dbname".
std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

// Owned filenames have the form:
//    dbname/CURRENT                // 当前文件
//    dbname/LOCK                   // 用文件实现的锁
//    dbname/LOG                    // 给人看的信息日志
//    dbname/LOG.old                // 每一次DB::Open()，将LOG重命名为LOG.old
//    dbname/MANIFEST-[0-9]+        // 清单文件
//    dbname/[0-9]+.(log|sst|ldb)   // .log WAL_Log
//                                  // .ldb|.sst 每一层的SSTable文件
// param[in] filename : 要分析的文件名字
// param[out] number : 文件的序列号，有序表文件、预写日志文件、清单文件都需要序列号
// param[out] type : 文件类型
// return : 文件名分析是否成功
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type) {
  Slice rest(filename);
  if (rest == "CURRENT") {
    *number = 0;
    *type = kCurrentFile;
  } else if (rest == "LOCK") {
    *number = 0;
    *type = kDBLockFile;
  } else if (rest == "LOG" || rest == "LOG.old") {
    *number = 0;
    *type = kInfoLogFile;
  } else if (rest.starts_with("MANIFEST-")) {
    rest.remove_prefix(strlen("MANIFEST-"));
    uint64_t num;
    // 读取清单文件MANIFEST-后缀的序列号num，并且要求读取之后filename没有剩余字符
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    if (!rest.empty()) {
      return false;
    }
    *type = kDescriptorFile;
    *number = num;
  } else {
    // Avoid strtoull() to keep filename format independent of the
    // current locale
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    Slice suffix = rest;
    if (suffix == Slice(".log")) {
      *type = kLogFile;
    } else if (suffix == Slice(".sst") || suffix == Slice(".ldb")) {
      *type = kTableFile;
    } else if (suffix == Slice(".dbtmp")) {
      *type = kTempFile;
    } else {
      return false;
    }
    *number = num;
  }
  return true;
}

Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number) {
  // Remove leading "dbname/" and add newline to manifest file name
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  Slice contents = manifest;
  assert(contents.starts_with(dbname + "/"));
  contents.remove_prefix(dbname.size() + 1);
  std::string tmp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFileSync(env, contents.ToString() + "\n", tmp);
  if (s.ok()) {
    s = env->RenameFile(tmp, CurrentFileName(dbname));
  }
  if (!s.ok()) {
    env->RemoveFile(tmp);
  }
  return s;
}

}  // namespace leveldb
