// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// An iterator yields a sequence of key/value pairs from a source.
// The following class defines the interface.  Multiple implementations
// are provided by this library.  In particular, iterators are provided
// to access the contents of a Table or a DB.
//
// Multiple threads can invoke const methods on an Iterator without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same Iterator must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_ITERATOR_H_
#define STORAGE_LEVELDB_INCLUDE_ITERATOR_H_

#include "leveldb/export.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class LEVELDB_EXPORT Iterator {
 public:
  Iterator();

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual ~Iterator();

  // An iterator is either positioned at a key/value pair, or
  // not valid.  This method returns true iff the iterator is valid.
  virtual bool Valid() const = 0;

  // Position at the first key in the source.  The iterator is Valid()
  // after this call iff the source is not empty.
  // 放到source中第一个key处。
  // 当且仅当source非空时，该调用之后迭代器有效。
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source.  The iterator is
  // Valid() after this call iff the source is not empty.
  // 放到source中最后一个key处。
  // 当且仅当source非空时，该调用之后迭代器有效。
  virtual void SeekToLast() = 0;

  // Position at the first key in the source that is at or past target.
  // The iterator is Valid() after this call iff the source contains
  // an entry that comes at or past target.
  // 放到在source中等于或晚于target的第一个key处。
  // 在该调用之后，当且仅当source中包含了一个等于或晚于target的条目时，迭代器Valid()。
  virtual void Seek(const Slice& target) = 0;

  // Moves to the next entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the last entry in the source.
  // REQUIRES: Valid()
  // 移动到source中的下一个条目。在该调用之后，当且仅当该迭代器不在source中最后一
  // 个条目时，Valid()为true。
  // 要求：Valid()
  virtual void Next() = 0;

  // Moves to the previous entry in the source.  After this call, Valid() is
  // true iff the iterator was not positioned at the first entry in source.
  // REQUIRES: Valid()
  // 移动到source中的前一个条目。在该调用之后，当且仅当该迭代器不在source中第一个
  // 条目时，Valid()为true。
  // 要求：Valid()
  virtual void Prev() = 0;

  // Return the key for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  // 返回当前条目的（内部）key。
  // 仅在该迭代器下一次修改之前，返回的Slice的底层存储有效。
  // 要求：Valid()
  virtual Slice key() const = 0;

  // Return the value for the current entry.  The underlying storage for
  // the returned slice is valid only until the next modification of
  // the iterator.
  // REQUIRES: Valid()
  // 返回当前条目的value。
  // 仅在该迭代器下一次修改之前，返回的Slice的底层存储有效。
  // 要求：Valid()
  virtual Slice value() const = 0;

  // If an error has occurred, return it.  Else return an ok status.
  virtual Status status() const = 0;

  // Clients are allowed to register function/arg1/arg2 triples that
  // will be invoked when this iterator is destroyed.
  //
  // Note that unlike all of the preceding methods, this method is
  // not abstract and therefore clients should not override it.
  // 允许client注册 function/arg1/arg2 三元组，在该迭代器销毁时调用。
  // 注意：不像之前的方法，这个方法不是抽象的，因此client不应该override它。
  using CleanupFunction = void (*)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);

 private:
  // Cleanup functions are stored in a single-linked list.
  // The list's head node is inlined in the iterator.
  // 清除函数存储在一个单向链表中。链表的head节点内联到迭代器内。
  struct CleanupNode {
    // True if the node is not used. Only head nodes might be unused.
    // 如果该节点没有被使用，则返回true。只有head节点可能会未被使用。
    bool IsEmpty() const { return function == nullptr; }
    // Invokes the cleanup function.
    // 调用清除函数。
    void Run() {
      assert(function != nullptr);
      (*function)(arg1, arg2);
    }

    // The head node is used if the function pointer is not null.
    // 如果function指针非空，则head节点是被使用的。
    CleanupFunction function;
    void* arg1;
    void* arg2;
    CleanupNode* next;
  };
  // head节点可能是“空”的，即未被使用的。
  CleanupNode cleanup_head_;
};

// Return an empty iterator (yields nothing).
// 返回一个OK状态的EmptyIterator
LEVELDB_EXPORT Iterator* NewEmptyIterator();

// Return an empty iterator with the specified status.
// 返回一个指定状态的EmptyIterator
LEVELDB_EXPORT Iterator* NewErrorIterator(const Status& status);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_ITERATOR_H_
