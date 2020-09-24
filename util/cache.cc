// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb {

Cache::~Cache() {}

namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.
//
// LRU Cache 实现
// Cache条目有一个 “in_cache” bool字段表示Cache是否有一个指向该条目的引用。不将该
// 条目传递给它的 “deleter” 就能让该字段变false的方法包括：通过Erase()，通过Insert()
// 插入包含已插入的key的元素，或者Cache的析构函数。
// 该Cache为其中的 item 维护两个链表。Cache中的所有 item 在某一链表中，不会同时存在
// 于两个链表之中。仍然被client引用但已经从Cache中删除的item不在任何链表中。两个链表
// 分别是：
// - in-use：包含当前被client引用的item，无序。（该链表用于不变检查(invariant checking)。
//    如果我们移除这个检查，那么原本在此链表上的元素可以保留为断链的单个lists。）
// - LRU：包含当前没有被client引用的item，按照LRU顺序。
// 当发现Cache中的一个元素获取或丢失其唯一的外部引用(external reference)时，通过Ref()
// 和Unref()方法将元素在这两个链表之间移动，

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
// 条目是一个变长的堆中申请的结构。
// 条目保存在按访问时间排序的循环双向链表（通过尾插）
struct LRUHandle {
  void* value;  // 真是数据存储在外部，并非构造handle是分配的空间
  void (*deleter)(const Slice&, void* value);
  // 仅用于哈希表
  LRUHandle* next_hash;
  LRUHandle* next;
  LRUHandle* prev;
  size_t charge;  // TODO(opt): Only allow uint32_t?
  size_t key_length;
  bool in_cache;     // Whether entry is in the cache.
  uint32_t refs;     // References, including cache reference, if present.
  // Hash of key(); used for fast sharding and comparisons
  // key()的哈希值，用于快速分片和比较
  uint32_t hash;
  // Beginning of key
  // 用于占位的一个字节。
  // 与value对比，key_data在构造handle时分配了空间。
  char key_data[1];

  Slice key() const {
    // next_ is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    // 如果该LRU句柄是空链表的表头，next_只能等于this。
    // 表头永远没有有意义的key。
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
// 我们提供自己的简单哈希表，因为它移除了一大堆的移植技巧，并且也比我们测试
// 过的一些编译/运行组合中的内建哈希表实现更快。例如，随机读的速度比g++4.4.3
// 内建哈希表快约5%。
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  // 析构函数只负责释放list_中指向每个bucket表头元素的指针，
  // 不负责释放bucket中的内容（堆中申请的所有元素）
  ~HandleTable() { delete[] list_; }

  // 返回指向hash对应bucket中与key一致的slot的指针。
  // 如果没有该Cache条目，返回指向hash对应的bucket中末尾slot的指针。
  // 【说明】返回“指针的指针”，就可以通过修改返回值来向bucket末尾追加新元素了。
  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  // 向哈希表中插入条目h。
  // 如果哈希表中已有h对应的条目，则用h替换，并返回指向被替换的条目的指针；
  // 否则，在对应bucket末尾插入条目h，并维护元素计数，最后返回nullptr。
  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    if (old == nullptr) {
      // 说明哈希表中原本没有条目h
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        // 由于每个Cache条目都很大，我们的目标是让链表（bucket）平均长度较小（<=1）。
        Resize();
      }
    }
    return old;
  }

  // 从hash对应的bucket中删除与key匹配的元素，并返回指向该元素的指针。
  // 如果哈希表中没有匹配的元素，则返回nullptr。
  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      // 说明有匹配的元素，将其从所在Bucket中删除
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  // The table consists of an array of buckets where each bucket is
  // a linked list of cache entries that hash into the bucket.
  // 该哈希表由一个bucket数组构成，其中每个bucket（桶）是一个Cache条目
  // 的链表。
  // length_是bucket数组的长度
  uint32_t length_;
  // elems_是哈希表中元素的数量
  uint32_t elems_;
  // list_是bucket（next_hash构成链表的表头指针）的数组
  LRUHandle** list_;

  // Return a pointer to slot that points to a cache entry that
  // matches key/hash.  If there is no such cache entry, return a
  // pointer to the trailing slot in the corresponding linked list.
  // 返回指向hash对应bucket中与key一致的slot的指针。
  // 如果没有该Cache条目，返回指向hash对应的bucket中末尾slot的指针。
  // 【说明】返回“指针的指针”，就可以通过修改返回值来向bucket末尾追加新元素了。
  //     或者说，返回“指针的地址”比较好理解，其实还是返回“指针的引用”更容易理解
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    // 【说明】先比较hash，因为比较hash比比较Slice要高效的多
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      // 哈希值不同，或者key不同，查看bucket中的下一个slot
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  // 尝试扩大bucket数组长度（确保是2的整数幂），确保length_不小于哈希表中元素数量
  // 扩大长度之后，list_中每个bucket中所有元素需要重新分配到新的对应的bucket中。
  void Resize() {
    // 新长度最小为4
    uint32_t new_length = 4;
    // 新长度翻倍，直到不小于当前哈希表中的元素数量
    while (new_length < elems_) {
      new_length *= 2;
    }
    // 从堆中分配 新的bucket数组
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      // 遍历list_中第i个bucket中所有slot对应的元素，逐个将元素分配到扩大后的new_list
      // 中正确的bucket中。
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        // 根据bucket中遍历的当前元素的哈希值重新分配到new_list中对应的bucket中。
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        // 在new_list中对应的bucket中，采用头插法。
        h->next_hash = *ptr;
        *ptr = h;
        h = next;
        count++;
      }
    }
    assert(elems_ == count);
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
// 分片Cache的单个分片
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  // 从构造函数中分离出来，这样调用者就可以很容易地创建LRUCache数组了。
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  // Like Cache methods, but with an extra "hash" parameter.
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  // 存储的所有handle的总charge
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Initialized before use.
  // 容量，能容纳的handle->charge总和
  size_t capacity_;

  // mutex_ protects the following state.
  mutable port::Mutex mutex_;
  // 受mutex_保护
  // 使用率，当前存储的所有handle的charge总和
  size_t usage_ GUARDED_BY(mutex_);

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  // LRU链表的表头。
  // lru_.prev 指向最新条目，lru_.next指向最旧的条目。【说明】因为是尾插。
  // LRU链表中的条目，refs==1，in_cache==true。
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  // in-use链表的表头。
  // in-use链表中的条目，正在被client使用，refs>=2，in_cache==true。
  LRUHandle in_use_ GUARDED_BY(mutex_);

  // 哈希表
  HandleTable table_ GUARDED_BY(mutex_);
};

// 初始 lru_ 和 in_use_ 都是空的循环链表，哈希表也为空表。
LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

// 将lru_中所有handle释放
LRUCache::~LRUCache() {
  // 如果调用者有未释放的handle，则有问题。
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    // lru_链表中的不变式
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

// 为e增加一个引用计数。增加之前，
// 如果引用计数为1，说明本次肯定是有client要使用该handle，因此，将其转移到in_use_
// 链表中。
// 【注意】in_use_ 受 mutex_ 保护，上层需要获取锁
void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    // 说明有client要使用e
    // 将e从当前所在链表中删除，追加到in_use_链表中
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

// 为e减少一个引用计数。减少后，
// 如果引用计数为0，必然非in_cache，说明是lru_链表中的handle释放引用计数，导致其
//   被淘汰，用其key和value调用其deleter函数指针，并释放handle申请的内存空间。
// 如果引用计数为1且in_cache，说明不再有client使用，将其转移到lru_链表，等待淘汰。
// 如果引用计数为1且非in_cache，既然已经不在Cache中，不需要执行其他操作，就等待下
//   一次Unref()释放资源。
// 如果引用计数大于1，引用计数还很多，不需要执行其他操作。
// 【注意】lru_ 受 mutex_ 保护，上层需要获取锁
void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    // 以handle中的key和value调用其deleter函数指针。
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    // 将e从当前所在链表中删除，追加到lru_链表中
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

// 将handle从当前所在的链表中删除。
void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

// 将handle e 追加到双向链表 list 末尾。
void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

// 通过哈希表查找匹配 key/hash 的handle。
// 如果LRUCache中有，返回指向handle的指针，并增加其引用计数，必然转到in_use_链表中；
// 如果LRUCache中没有，返回nullptr。
Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    // 有这个handle，引用计数+1
    Ref(e);
  }
  // 强转为Cache::Handle指针，为通用接口使用
  return reinterpret_cast<Cache::Handle*>(e);
}

// 减少handle的引用计数
void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

// 向LRUCache中插入handle。
// 支持没有容量（不在LRUCache中缓存）的情况。
// 如果LRUCache中容量满了，按照LRU策略，优先删除lru_链表中旧的handle。
// 返回指向新插入的handle的指针。
Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 为 e 在堆中申请一段内存
  LRUHandle* e =
      // LRUHandle结构定义中，只有用于占位的key的第一个字节
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) {
    // LRUCache有容量，增加引用计数，设置in_cache，并追加到in_use_链表中。
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    LRU_Append(&in_use_, e);
    usage_ += charge;
    // 向哈希表中插入e，如果哈希表中已存在key/hash匹配的handle，则用e替换，并通过FinishErase()
    // 从LRUCache中删除被替换的handle
    FinishErase(table_.Insert(e));
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    // next is read by key() in an assert, so it must be initialized
    // 不缓存。（支持容量为0，这样会关闭缓存。）
    // next在key()中被assert调用读取，所以必须初始化，以说明它不是空表的表头
    // 这种情况，在调用者使用完e之后，调用LRUCache::Release()释放e，依然会释放其资源。
    e->next = nullptr;
  }
  // 如果存储的handle总charge超过容量，且lru_链表中有handle，则从旧到新删除lru_链表中的handle，
  // 直到从charge在容量范围内。
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    // 先从哈希表删除，在从LRUCache中完成删除操作。
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

// If e != nullptr, finish removing *e from the cache; it has already been
// removed from the hash table.  Return whether e != nullptr.
// 如果 e 不为 nullptr，完成将 *e 从LRUCache中移除；它已经从哈希表中移除。
// 返回是否真正的删除了一个handle（而非因为e为nullptr，未执行删除）。
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    // 标记为不在Cache中，引用基数为0时可以释放资源
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }
  return e != nullptr;
}

// 从LRUCache中删除key/hash匹配的元素。
void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  // 先从哈希表中删除，删除的过程中，通过哈希快速的找到了匹配元素，再将元素从
  // 所在链表中删除，并释放引用计数。
  // 先执行哈希表的操作，效率更高。
  FinishErase(table_.Remove(key, hash));
}

// 修剪操作：
// 将lru_链表中所有元素，从LRUCache中删除，并释放资源
void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

// 分片的 LRUCache
class ShardedLRUCache : public Cache {
 private:
  // 每个handle根据其key的哈希值来决定，分配到哪个分片
  LRUCache shard_[kNumShards];
  port::Mutex id_mutex_;
  uint64_t last_id_;

  // 计算Slice的哈希值
  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  // 根据hash值计算分片，hash值的最高kNumShardBits位，作为对应的分片
  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    // 设置每个切片的容量（向上取整）
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }
  // 修剪：删除所有未被活跃使用的Cache条目。内存受限的应用程序可能希望调用这个方法来减少
  // 内存使用量。
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
