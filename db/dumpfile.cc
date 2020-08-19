// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/dumpfile.h"

#include <cstdio>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/write_batch.h"
#include "util/logging.h"

namespace leveldb {

namespace {

// 猜测文件fname的文件类型
bool GuessType(const std::string& fname, FileType* type) {
  size_t pos = fname.rfind('/');
  std::string basename;
  if (pos == std::string::npos) {
    basename = fname;
  } else {
    basename = std::string(fname.data() + pos + 1, fname.size() - pos - 1);
  }
  uint64_t ignored;
  return ParseFileName(basename, &ignored, type);
}

// Notified when log reader encounters corruption.
// 当log reader遇到corruption时，通知到可写文件dst_
class CorruptionReporter : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    std::string r = "corruption: ";
    AppendNumberTo(&r, bytes);
    r += " bytes; ";
    r += status.ToString();
    r.push_back('\n');
    dst_->Append(r);
  }

  WritableFile* dst_;
};

// Print contents of a log file. (*func)() is called on every record.
// 打印一个log文件的内容，到可写文件dst
// param[in] env : ???
// param[in] fname : 要读取的文件名字
// param[in] func : 对从fname中读取的每个记录调用的方法
// param[in] dst : 将日志打印到可写文件dst中
Status PrintLogContents(Env* env, const std::string& fname,
                        void (*func)(uint64_t, Slice, WritableFile*),
                        WritableFile* dst) {
  SequentialFile* file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  CorruptionReporter reporter;
  reporter.dst_ = dst;
  // checksum为true，验证校验和
  // initial_offset为0，从file的开始位置读取记录
  log::Reader reader(file, &reporter, true, 0);
  Slice record;
  std::string scratch;
  // 读取一个record，并调用func方法，将记录写入到dst中
  while (reader.ReadRecord(&record, &scratch)) {
    (*func)(reader.LastRecordOffset(), record, dst);
  }
  delete file;
  return Status::OK();
}

// Called on every item found in a WriteBatch.
// 将Batch中的操作记录以人类可读的格式追加到一个可写文件dst_中
class WriteBatchItemPrinter : public WriteBatch::Handler {
 public:
  // 向dst_中追加 "  put 'key' 'value'\n"，有些字符可能需要转义
  void Put(const Slice& key, const Slice& value) override {
    std::string r = "  put '";
    AppendEscapedStringTo(&r, key);
    r += "' '";
    AppendEscapedStringTo(&r, value);
    r += "'\n";
    dst_->Append(r);
  }
  // 向dst_中追加 "  del 'key'\n"，有些字符可能需要转义
  void Delete(const Slice& key) override {
    std::string r = "  del '";
    AppendEscapedStringTo(&r, key);
    r += "'\n";
    dst_->Append(r);
  }

  WritableFile* dst_;
};

// Called on every log record (each one of which is a WriteBatch)
// found in a kLogFile.
// 对kLogFile中的每个日志记录（每一个都是一个WriteBatch）调用。
// param[in] pos : ???
// param[in] record : 所有的操作记录，可以作为WriteBatch的内容
// param[in] dst : 向可写文件dst中追加日志记录
// 如果record<12（8字节序列号，4字节count），太小，dst文件追加：
//   "--- offset #pos#; log record length #record_size# is too small\n"
// 否则，dst文件追加：
//   "--- offset #pos#; sequence #sequence#\n"
//   后面再追加record中的每个操作记录
//   如果中途处理失败，dst后追加：
//     "  error: #errinfo#\n"
static void WriteBatchPrinter(uint64_t pos, Slice record, WritableFile* dst) {
  std::string r = "--- offset ";
  AppendNumberTo(&r, pos);
  r += "; ";
  // 这个12应该是与kHeader是一致的
  if (record.size() < 12) {
    r += "log record length ";
    AppendNumberTo(&r, record.size());
    r += " is too small\n";
    dst->Append(r);
    return;
  }
  WriteBatch batch;
  WriteBatchInternal::SetContents(&batch, record);
  r += "sequence ";
  AppendNumberTo(&r, WriteBatchInternal::Sequence(&batch));
  r.push_back('\n');
  dst->Append(r);
  // 遍历batch，将其中所有记录按照顺序按照人类可读的格式追加到dst中
  WriteBatchItemPrinter batch_item_printer;
  batch_item_printer.dst_ = dst;
  Status s = batch.Iterate(&batch_item_printer);
  if (!s.ok()) {
    dst->Append("  error: " + s.ToString() + "\n");
  }
}

// 顺序读取fname文件，每读取一个record（对应一个WriteBatch）调用一次
// WriteBatchPrinter方法，将记录以日志的形式写入到可写文件dst中
Status DumpLog(Env* env, const std::string& fname, WritableFile* dst) {
  return PrintLogContents(env, fname, WriteBatchPrinter, dst);
}

// Called on every log record (each one of which is a WriteBatch)
// found in a kDescriptorFile.
static void VersionEditPrinter(uint64_t pos, Slice record, WritableFile* dst) {
  std::string r = "--- offset ";
  AppendNumberTo(&r, pos);
  r += "; ";
  VersionEdit edit;
  Status s = edit.DecodeFrom(record);
  if (!s.ok()) {
    r += s.ToString();
    r.push_back('\n');
  } else {
    r += edit.DebugString();
  }
  dst->Append(r);
}

Status DumpDescriptor(Env* env, const std::string& fname, WritableFile* dst) {
  return PrintLogContents(env, fname, VersionEditPrinter, dst);
}

Status DumpTable(Env* env, const std::string& fname, WritableFile* dst) {
  uint64_t file_size;
  RandomAccessFile* file = nullptr;
  Table* table = nullptr;
  Status s = env->GetFileSize(fname, &file_size);
  if (s.ok()) {
    s = env->NewRandomAccessFile(fname, &file);
  }
  if (s.ok()) {
    // We use the default comparator, which may or may not match the
    // comparator used in this database. However this should not cause
    // problems since we only use Table operations that do not require
    // any comparisons.  In particular, we do not call Seek or Prev.
    s = Table::Open(Options(), file, file_size, &table);
  }
  if (!s.ok()) {
    delete table;
    delete file;
    return s;
  }

  ReadOptions ro;
  ro.fill_cache = false;
  Iterator* iter = table->NewIterator(ro);
  std::string r;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    r.clear();
    ParsedInternalKey key;
    if (!ParseInternalKey(iter->key(), &key)) {
      r = "badkey '";
      AppendEscapedStringTo(&r, iter->key());
      r += "' => '";
      AppendEscapedStringTo(&r, iter->value());
      r += "'\n";
      dst->Append(r);
    } else {
      r = "'";
      AppendEscapedStringTo(&r, key.user_key);
      r += "' @ ";
      AppendNumberTo(&r, key.sequence);
      r += " : ";
      if (key.type == kTypeDeletion) {
        r += "del";
      } else if (key.type == kTypeValue) {
        r += "val";
      } else {
        AppendNumberTo(&r, key.type);
      }
      r += " => '";
      AppendEscapedStringTo(&r, iter->value());
      r += "'\n";
      dst->Append(r);
    }
  }
  s = iter->status();
  if (!s.ok()) {
    dst->Append("iterator error: " + s.ToString() + "\n");
  }

  delete iter;
  delete table;
  delete file;
  return Status::OK();
}

}  // namespace

Status DumpFile(Env* env, const std::string& fname, WritableFile* dst) {
  FileType ftype;
  if (!GuessType(fname, &ftype)) {
    return Status::InvalidArgument(fname + ": unknown file type");
  }
  switch (ftype) {
    case kLogFile:
      return DumpLog(env, fname, dst);
    case kDescriptorFile:
      return DumpDescriptor(env, fname, dst);
    case kTableFile:
      return DumpTable(env, fname, dst);
    default:
      break;
  }
  return Status::InvalidArgument(fname + ": not a dump-able file type");
}

}  // namespace leveldb
