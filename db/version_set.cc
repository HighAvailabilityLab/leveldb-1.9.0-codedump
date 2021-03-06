// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_set.h"

#include <algorithm>
#include <stdio.h>
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "leveldb/env.h"
#include "leveldb/table_builder.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// compact 过程中， level-0 中的 sstable 由 memtable 直接 dump 生成，不做大小限制
// 非 level-0 中的 sstable 的大小设定为 kTargetFileSize
static const int kTargetFileSize = 2 * 1048576;

// Maximum bytes of overlaps in grandparent (i.e., level+2) before we
// stop building a single file in a level->level+1 compaction.
// compact过程中，允许level-n与level-n+2之间产生overlap的数据size
static const int64_t kMaxGrandParentOverlapBytes = 10 * kTargetFileSize;

// Maximum number of bytes in all compacted files.  We avoid expanding
// the lower level file set of a compaction if it would make the
// total compaction cover more than this many bytes.
static const int64_t kExpandedCompactionByteSizeLimit = 25 * kTargetFileSize;

// 计算一个level的最大Bytes
static double MaxBytesForLevel(int level) {
  // Note: the result for level zero is not really used since we set
  // the level-0 compaction threshold based on number of files.
  // 0,1级的大小是10M
  double result = 10 * 1048576.0;  // Result for both level-0 and level-1
  while (level > 1) {
    // 每多一级，大小翻10倍
    result *= 10;
    level--;
  }
  return result;
}

// 返回每层最大的文件大小
static uint64_t MaxFileSizeForLevel(int level) {
  return kTargetFileSize;  // We could vary per level to reduce number of files?
}

// 返回这些文件集合的大小之和
static int64_t TotalFileSize(const std::vector<FileMetaData*>& files) {
  int64_t sum = 0;
  for (size_t i = 0; i < files.size(); i++) {
    sum += files[i]->file_size;
  }
  return sum;
}

namespace {
std::string IntSetToString(const std::set<uint64_t>& s) {
  std::string result = "{";
  for (std::set<uint64_t>::const_iterator it = s.begin();
       it != s.end();
       ++it) {
    result += (result.size() > 1) ? "," : "";
    result += NumberToString(*it);
  }
  result += "}";
  return result;
}
}  // namespace

Version::~Version() {
  assert(refs_ == 0);

  // Remove from linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;

  // Drop references to files
  // 遍历所有level的file,减ref,如果到0就delete
  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData* f = files_[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        delete f;
      }
    }
  }
}

// 找到最小的数组索引i,满足 files[i]->largest >= key
// 二分法查找
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files,
             const Slice& key) {
  uint32_t left = 0;
  uint32_t right = files.size();
  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData* f = files[mid];
    if (icmp.InternalKeyComparator::Compare(f->largest.Encode(), key) < 0) {
      // Key at "mid.largest" is < "target".  Therefore all
      // files at or before "mid" are uninteresting.
      left = mid + 1;
    } else {
      // Key at "mid.largest" is >= "target".  Therefore all files
      // after "mid" are uninteresting.
      right = mid;
    }
  }
  return right;
}

// 返回user_key是否在FileMetaData之后
static bool AfterFile(const Comparator* ucmp,
                      const Slice* user_key, const FileMetaData* f) {
  // NULL user_key occurs before all keys and is therefore never after *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}

// 返回user_key是否在FileMetaData之前
static bool BeforeFile(const Comparator* ucmp,
                       const Slice* user_key, const FileMetaData* f) {
  // NULL user_key occurs after all keys and is therefore never before *f
  return (user_key != NULL &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

// 返回files里面的文件，是否与[smallest,largest]范围有重叠
bool SomeFileOverlapsRange(
    const InternalKeyComparator& icmp,
    bool disjoint_sorted_files,
    const std::vector<FileMetaData*>& files,
    const Slice* smallest_user_key,
    const Slice* largest_user_key) {
  const Comparator* ucmp = icmp.user_comparator();
  // 如果不是排序过而且没有交集的文件集合，那需要遍历所有文件
  if (!disjoint_sorted_files) {
    // Need to check against all files
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return true;  // Overlap
      }
    }
    return false;
  }

  // 否则这些文件都是进行排序的，二分查找就可以了
  // Binary search over file list
  uint32_t index = 0;
  if (smallest_user_key != NULL) {
    // 在smallest_user_key不为NULL的情况下，先从这些文件中找到比smallest_user_key大的最小索引
    // Find the earliest possible internal key for smallest_user_key
    InternalKey small(*smallest_user_key, kMaxSequenceNumber,kValueTypeForSeek);
    index = FindFile(icmp, files, small.Encode());
  }

  // 如果索引值比文件集合的数量大，直接返回false了
  if (index >= files.size()) {
    // beginning of range is after all files, so no overlap.
    return false;
  }

  // 接着从index开始找是否有比largest_user_key小的文件
  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

// An internal iterator.  For a given version/level pair, yields
// information about the files in the level.  For a given entry, key()
// is the largest key that occurs in the file, and value() is an
// 16-byte value containing the file number and file size, both
// encoded using EncodeFixed64.
// LevelFileNumIterator提供了遍历一组排序好的FileMetaData(也就是sstable文件)的迭代器
// key()：文件中的最大key
// value()：16byte保存了文件号以及文件大小的值，两个数字都是fixed64类型
class Version::LevelFileNumIterator : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<FileMetaData*>* flist)
      : icmp_(icmp),
        flist_(flist),
        index_(flist->size()) {        // Marks as invalid
  }
  virtual bool Valid() const {
    return index_ < flist_->size();
  }
  virtual void Seek(const Slice& target) {
    index_ = FindFile(icmp_, *flist_, target);
  }
  // SeekToFirst直接将index置为0
  virtual void SeekToFirst() { index_ = 0; }
  // SeekToLast直接将index置为最后的index
  virtual void SeekToLast() {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  virtual void Next() {
    assert(Valid());
    index_++;
  }
  virtual void Prev() {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();  // Marks as invalid
    } else {
      index_--;
    }
  }
  // 迭代器的key是当前文件的最大key
  Slice key() const {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }
  // 迭代器的value是当前文件的序号和文件大小的序列化值
  Slice value() const {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_+8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }
  virtual Status status() const { return Status::OK(); }
 private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData*>* const flist_;
  uint32_t index_;

  // Backing store for value().  Holds the file number and size.
  mutable char value_buf_[16];
};

static Iterator* GetFileIterator(void* arg,
                                 const ReadOptions& options,
                                 const Slice& file_value) {
  TableCache* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16) {
    return NewErrorIterator(
        Status::Corruption("FileReader invoked with unexpected value"));
  } else {
    // 根据前面LevelFileNumIterator迭代器的值decode出来得到cache的iterator
    return cache->NewIterator(options,
                              DecodeFixed64(file_value.data()),
                              DecodeFixed64(file_value.data() + 8));
  }
}

Iterator* Version::NewConcatenatingIterator(const ReadOptions& options,
                                            int level) const {
  return NewTwoLevelIterator(
      new LevelFileNumIterator(vset_->icmp_, &files_[level]),
      &GetFileIterator, vset_->table_cache_, options);
}

void Version::AddIterators(const ReadOptions& options,
                           std::vector<Iterator*>* iters) {
  // 对于0级文件，全部添加进来，因为其中可能有重叠
  // Merge all level zero files together since they may overlap
  for (size_t i = 0; i < files_[0].size(); i++) {
    iters->push_back(
        vset_->table_cache_->NewIterator(
            options, files_[0][i]->number, files_[0][i]->file_size));
  }

  // 对于>0级文件，使用NewConcatenatingIterator类型的iterator
  // For levels > 0, we can use a concatenating iterator that sequentially
  // walks through the non-overlapping files in the level, opening them
  // lazily.
  for (int level = 1; level < config::kNumLevels; level++) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

// Callback from TableCache::Get()
namespace {
enum SaverState {
  kNotFound,
  kFound,
  kDeleted,
  kCorrupt,
};
struct Saver {
  SaverState state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;
};
}
static void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  Saver* s = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else {
    if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
      s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
      if (s->state == kFound) {
        s->value->assign(v.data(), v.size());
      }
    }
  }
}

// 文件序号越大的越新
static bool NewestFirst(FileMetaData* a, FileMetaData* b) {
  return a->number > b->number;
}

// 查询key
Status Version::Get(const ReadOptions& options,
                    const LookupKey& k,
                    std::string* value,
                    GetStats* stats) {
  Slice ikey = k.internal_key();
  Slice user_key = k.user_key();
  const Comparator* ucmp = vset_->icmp_.user_comparator();
  Status s;

  stats->seek_file = NULL;
  stats->seek_file_level = -1;
  FileMetaData* last_file_read = NULL;
  int last_file_read_level = -1;

  // We can search level-by-level since entries never hop across
  // levels.  Therefore we are guaranteed that if we find data
  // in an smaller level, later levels are irrelevant.
  std::vector<FileMetaData*> tmp;
  FileMetaData* tmp2;
  // 以下的循环，从0级开始查询key
  for (int level = 0; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    // 这个级别没有文件，忽略
    if (num_files == 0) continue;

    // Get the list of files to search in this level
    // files保存本级查询到的文件数据链表
    FileMetaData* const* files = &files_[level][0];
    if (level == 0) {
      // Level-0 files may overlap each other.  Find all files that
      // overlap user_key and process them in order from newest to oldest.
      // 0级文件有可能key范围有重叠,所以要遍历这个级别的所有文件
      tmp.reserve(num_files);
      for (uint32_t i = 0; i < num_files; i++) {
        FileMetaData* f = files[i];
        if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
            ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
          tmp.push_back(f);
        }
      }
      if (tmp.empty()) continue;

      // 0级文件key有可能重叠，需要对文件进行排序，最新的文件优先
      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      // files指向排序后的tmp数组，num_files保存文件数量
      files = &tmp[0];
      num_files = tmp.size();
    } else {
      // 如果不是第一层就二分查找某个文件,这个文件满足条件:其最大的key >= ikey
      // Binary search to find earliest index whose largest key >= ikey.
      uint32_t index = FindFile(vset_->icmp_, files_[level], ikey);
      if (index >= num_files) {
    	// 如果找到的文件索引大于文件数量,说明没有找到
        files = NULL;
        num_files = 0;
      } else {
        tmp2 = files[index];
        // 对比这个文件中最小的key与user_key
        if (ucmp->Compare(user_key, tmp2->smallest.user_key()) < 0) {
          // All of "tmp2" is past any data for user_key
          // 如果user_key比最小的key还小,说明没有找到
          files = NULL;
          num_files = 0;
        } else {
          // 指向文件，数量为1
          files = &tmp2;
          num_files = 1;
        }
      }
    }

    // 出了上面的循环之后,files是排序好的数组
    for (uint32_t i = 0; i < num_files; ++i) {
      if (last_file_read != NULL && stats->seek_file == NULL) {
        // We have had more than one seek for this read.  Charge the 1st file.
    	  // 记录下最后一次seek的文件和级别
    	  // 这里记录下来是为了在Version::UpdateStats中f->allowed_seeks--;然后进行compact用的
    	  // 但是难道不应该是这个过程中seek过的文件都要减allowed_seeks么?
        stats->seek_file = last_file_read;
        stats->seek_file_level = last_file_read_level;
      }

      FileMetaData* f = files[i];
      last_file_read = f;
      last_file_read_level = level;

      Saver saver;
      saver.state = kNotFound;
      saver.ucmp = ucmp;
      saver.user_key = user_key;
      saver.value = value;
      // 这里会读取LRU cache中存储的Table指针,再调用Table指针的InternalGet函数去查找数据(但是这里是磁盘I/O)
      s = vset_->table_cache_->Get(options, f->number, f->file_size,
                                   ikey, &saver, SaveValue);
      if (!s.ok()) {
        return s;
      }
      // 下面的情况,只有为kNotFound时才继续下一个查找,其余的情况都退出函数
      switch (saver.state) {
        case kNotFound:
          break;      // Keep searching in other files
        case kFound:
          return s;
        case kDeleted:
          s = Status::NotFound(Slice());  // Use empty error message for speed
          return s;
        case kCorrupt:
          s = Status::Corruption("corrupted key for ", user_key);
          return s;
      }
    }
  }

  return Status::NotFound(Slice());  // Use an empty error message for speed
}

bool Version::UpdateStats(const GetStats& stats) {
  // stats.seek_file中存放的是最后一个seek的文件
  // 这里不应该只对最后一个seek的文件减allowed_seeks,应该对这个过程中所有seek过的文件都减吧?
  FileMetaData* f = stats.seek_file;
  if (f != NULL) {
    f->allowed_seeks--;
    // 当这个文件的allowed_seeks小于等于0，同时当前没有在compact的文件时，
    // 记录下待compact的文件，以及compact的级别
    // allowed_seeks在Apply函数中进行初始化
    if (f->allowed_seeks <= 0 && file_to_compact_ == NULL) {
      file_to_compact_ = f;
      file_to_compact_level_ = stats.seek_file_level;
      return true;
    }
  }
  return false;
}

void Version::Ref() {
  ++refs_;
}

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

// 返回在这一级别是否有重叠的范围
bool Version::OverlapInLevel(int level,
                             const Slice* smallest_user_key,
                             const Slice* largest_user_key) {
  // level 0有重叠，所以第二个参数判断level是否>0
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

// 返回含有[smallest_user_key,largest_user_key]返回的level来返回
int Version::PickLevelForMemTableOutput(
    const Slice& smallest_user_key,
    const Slice& largest_user_key) {
  int level = 0;
  // 先判断0级
  if (!OverlapInLevel(0, &smallest_user_key, &largest_user_key)) {
    // 0级没有查询到，继续往上查找
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    InternalKey start(smallest_user_key, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(largest_user_key, 0, static_cast<ValueType>(0));
    std::vector<FileMetaData*> overlaps;
    // 逐层查找
    while (level < config::kMaxMemCompactLevel) {
      // 先判断level+1级别是否满足要求，满足就返回
      if (OverlapInLevel(level + 1, &smallest_user_key, &largest_user_key)) {
        break;
      }
      // 然后拿到level+2级别在范围内的文件集合
      GetOverlappingInputs(level + 2, &start, &limit, &overlaps);
      const int64_t sum = TotalFileSize(overlaps);
      // 但是如果超过了返回就不再继续查找了
      if (sum > kMaxGrandParentOverlapBytes) {
        break;
      }
      level++;
    }
  }
  // 到了这里就是0级查询到了，返回level（=0）
  return level;
}

// Store in "*inputs" all files in "level" that overlap [begin,end]
// 将所有level中满足key在[begin,end]范围内的文件都加入inputs中
void Version::GetOverlappingInputs(
    int level,
    const InternalKey* begin,
    const InternalKey* end,
    std::vector<FileMetaData*>* inputs) {
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != NULL) {
    user_begin = begin->user_key();
  }
  if (end != NULL) {
    user_end = end->user_key();
  }
  const Comparator* user_cmp = vset_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size(); ) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != NULL && user_cmp->Compare(file_limit, user_begin) < 0) {
      // 如果当前文件的最大值比key的起始范围还小,忽略这个文件
      // "f" is completely before specified range; skip it
    } else if (end != NULL && user_cmp->Compare(file_start, user_end) > 0) {
      // 如果当前文件的最小值比key的最后范围还大,忽略
      // "f" is completely after specified range; skip it
    } else {
      // 满足条件，将这个文件加进来
      inputs->push_back(f);
      if (level == 0) {
        // Level-0 files may overlap each other.  So check if the newly
        // added file has expanded the range.  If so, restart search.
    	  // level 0需要做一些特殊处理,因为0级的不同文件中key有重复
        if (begin != NULL && user_cmp->Compare(file_start, user_begin) < 0) {
          // 如果当前文件的最小值比key的起始范围还小,调整key起始范围为该文件的最小值,重新开始扫描
          user_begin = file_start;
          // 清空当前保存的结果
          inputs->clear();
          // 重新开始遍历
          i = 0;
        } else if (end != NULL && user_cmp->Compare(file_limit, user_end) > 0) {
          // 如果当前文件的最大值比key的结束范围还大,调整key结束范围为该文件的最大值,重新开始扫描
          user_end = file_limit;
          // 清空当前保存的结果
          inputs->clear();
          // 重新开始遍历
          i = 0;
        }
      }
    }
  }
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < config::kNumLevels; level++) {
    // E.g.,
    //   --- level 1 ---
    //   17:123['a' .. 'd']
    //   20:43['e' .. 'g']
    r.append("--- level ");
    AppendNumberTo(&r, level);
    r.append(" ---\n");
    const std::vector<FileMetaData*>& files = files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      r.push_back(' ');
      AppendNumberTo(&r, files[i]->number);
      r.push_back(':');
      AppendNumberTo(&r, files[i]->file_size);
      r.append("[");
      r.append(files[i]->smallest.DebugString());
      r.append(" .. ");
      r.append(files[i]->largest.DebugString());
      r.append("]\n");
    }
  }
  return r;
}

// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
// 辅助类,用于根据version edit的数据来更新一个version里的数据
class VersionSet::Builder {
 private:
  // Helper to sort by v->files_[file_number].smallest
  // operator,用于比较文件元信息中的排序
  struct BySmallestKey {
    const InternalKeyComparator* internal_comparator;

    bool operator()(FileMetaData* f1, FileMetaData* f2) const {
      // 先比较最小key
      int r = internal_comparator->Compare(f1->smallest, f2->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        // 再比较文件数量
        // Break ties by file number
        return (f1->number < f2->number);
      }
    }
  };

  // 排好序的FileMetaData集合，其中的compare就是前面的BySmallestKey
  typedef std::set<FileMetaData*, BySmallestKey> FileSet;
  // 用于保存每个level待更新的文件
  struct LevelState {
    // 已删除文件
    std::set<uint64_t> deleted_files;
    // 待添加的文件
    FileSet* added_files;
  };

  // 待更新的VersionSet
  VersionSet* vset_;
  // 基准的Version
  Version* base_;
  // 每个级别上都有什么需要更新的
  LevelState levels_[config::kNumLevels];

 public:
  // Initialize a builder with the files from *base and other info from *vset
  Builder(VersionSet* vset, Version* base)
      : vset_(vset),
        base_(base) {
    base_->Ref();
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      levels_[level].added_files = new FileSet(cmp);
    }
  }

  ~Builder() {
    // 遍历所有层次的添加文件，将计数器减一，为0的时候删除之
    for (int level = 0; level < config::kNumLevels; level++) {
      const FileSet* added = levels_[level].added_files;
      std::vector<FileMetaData*> to_unref;
      to_unref.reserve(added->size());
      for (FileSet::const_iterator it = added->begin();
          it != added->end(); ++it) {
        to_unref.push_back(*it);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData* f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }
    base_->Unref();
  }

  // 根据VersionEdit将前面做的改动应用到当前状态中
  // Apply all of the edits in *edit to the current state.
  void Apply(VersionEdit* edit) {
    // Update compaction pointers
	  // 更新version set中每个级别的compact_pointers_
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] =
          edit->compact_pointers_[i].second.Encode().ToString();
    }

    // Delete files
    // 保存每个级别的待删除文件number
    const VersionEdit::DeletedFileSet& del = edit->deleted_files_;
    for (VersionEdit::DeletedFileSet::const_iterator iter = del.begin();
         iter != del.end();
         ++iter) {
      const int level = iter->first;
      const uint64_t number = iter->second;
      levels_[level].deleted_files.insert(number);
    }

    // Add new files
    // 添加新文件
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      FileMetaData* f = new FileMetaData(edit->new_files_[i].second);
      f->refs = 1;

      // We arrange to automatically compact this file after
      // a certain number of seeks.  Let's assume:
      //   (1) One seek costs 10ms
      //   (2) Writing or reading 1MB costs 10ms (100MB/s)
      //   (3) A compaction of 1MB does 25MB of IO:
      //         1MB read from this level
      //         10-12MB read from next level (boundaries may be misaligned)
      //         10-12MB written to next level
      // This implies that 25 seeks cost the same as the compaction
      // of 1MB of data.  I.e., one seek costs approximately the
      // same as the compaction of 40KB of data.  We are a little
      // conservative and allow approximately one seek for every 16KB
      // of data before triggering a compaction.

      // 对上面这段注释的翻译：
      // 这里将在特定数量的seek之后自动进行compact操作，假设：
      // (1) 一次seek需要10ms
      // (2) 读、写1MB文件消耗10ms(100MB/s)
      // (3) 对1MB文件的compact操作时合计一共做了25MB的IO操作，包括：
      //    从这个级别读1MB
      //    从下一个级别读10-12MB
      //    向下一个级别写10-12MB
      //  这意味着25次seek的消耗与1MB数据的compact相当。也就是，
      //  一次seek的消耗与40KB数据的compact消耗近似。这里做一个
      //  保守的估计，在一次compact之前每16KB的数据大约进行1次seek。
      // allowed_seeks数目和文件数量有关
      f->allowed_seeks = (f->file_size / 16384);
      // 不足100就补齐到100
      if (f->allowed_seeks < 100) f->allowed_seeks = 100;

      // 在待删除文件中把添加文件根据number号删除
      levels_[level].deleted_files.erase(f->number);
      // 增加到新添加文件中
      levels_[level].added_files->insert(f);
    }
  }

  // Save the current state in *v.
  // 将改动保存到version中
  void SaveTo(Version* v) {
    BySmallestKey cmp;
    cmp.internal_comparator = &vset_->icmp_;
    // 遍历所有level
    for (int level = 0; level < config::kNumLevels; level++) {
      // Merge the set of added files with the set of pre-existing files.
      // Drop any deleted files.  Store the result in *v.
      // base_是version set当前current的version指针
      const std::vector<FileMetaData*>& base_files = base_->files_[level];
      std::vector<FileMetaData*>::const_iterator base_iter = base_files.begin();
      std::vector<FileMetaData*>::const_iterator base_end = base_files.end();
      // added表示这次需要添加进去的文件
      const FileSet* added = levels_[level].added_files;
      // 预分配空间 = base + added文件之和,也就是当前version的文件以及edit待添加文件的数量之和
      v->files_[level].reserve(base_files.size() + added->size());
      // 遍历所有added文件
      for (FileSet::const_iterator added_iter = added->begin();
           added_iter != added->end();
           ++added_iter) {
        // Add all smaller files listed in base_
    	// 首先将所有在[base_iter, base_end]中小于added_iter的数据都添加进来
        for (std::vector<FileMetaData*>::const_iterator bpos
                 = std::upper_bound(base_iter, base_end, *added_iter, cmp);
             base_iter != bpos;
             ++base_iter) {
          MaybeAddFile(v, level, *base_iter);
        }
        // 然后再添加added_iter的数据
        MaybeAddFile(v, level, *added_iter);
      }

      // Add remaining base files
      // 出了循环之后再添加剩余的文件
      for (; base_iter != base_end; ++base_iter) {
        MaybeAddFile(v, level, *base_iter);
      }

#ifndef NDEBUG
      // Make sure there is no overlap in levels > 0
      if (level > 0) {
        for (uint32_t i = 1; i < v->files_[level].size(); i++) {
          const InternalKey& prev_end = v->files_[level][i-1]->largest;
          const InternalKey& this_begin = v->files_[level][i]->smallest;
          if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
            fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                    prev_end.DebugString().c_str(),
                    this_begin.DebugString().c_str());
            abort();
          }
        }
      }
#endif
    }
  }

  // 判断某个文件是否需要添加进来
  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {
      // 要删除的文件就不要添加了
      // File is deleted: do nothing
    } else {
      std::vector<FileMetaData*>* files = &v->files_[level];
      if (level > 0 && !files->empty()) {
        // Must not overlap
    	// 0级以上不能出现overlap的情况
        assert(vset_->icmp_.Compare((*files)[files->size()-1]->largest,
                                    f->smallest) < 0);
      }
      // 其他文件增加计数器,push到文件数组中
      f->refs++;
      files->push_back(f);
    }
  }
};

VersionSet::VersionSet(const std::string& dbname,
                       const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(NULL),
      descriptor_log_(NULL),
      dummy_versions_(this),
      current_(NULL) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);  // List must be empty
  delete descriptor_log_;
  delete descriptor_file_;
}

void VersionSet::AppendVersion(Version* v) {
  // Make "v" current
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != NULL) {
    current_->Unref();
  }
  // 将v mark 成 current
  current_ = v;
  v->Ref();

  // 添加到链表中
  // Append to linked list
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::LogAndApply(VersionEdit* edit, port::Mutex* mu) {
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  // 新建一个version,其结果为当前version + version edit
  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  // 计算这个新version的compact score和compact level
  Finalize(v);

  // Initialize new descriptor log file if necessary by creating
  // a temporary file that contains a snapshot of the current version.
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == NULL) {
    // 这里没有必要进行unlock操作，因为只有在第一次调用，也就是打开数据库的时候
    // 才会走到这个路径里面来
    // No reason to unlock *mu here since we only hit this path in the
    // first call to LogAndApply (when opening the database).
    assert(descriptor_file_ == NULL);
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    edit->SetNextFile(next_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = new log::Writer(descriptor_file_);
      // 写当前的快照
      s = WriteSnapshot(descriptor_log_);
    }
  }

  // Unlock during expensive MANIFEST log write
  {
    mu->Unlock();

    // Write new record to MANIFEST log
    if (s.ok()) {
      // 保存这次的改动到manifest文件中
      std::string record;
      edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
      if (s.ok()) {
        s = descriptor_file_->Sync();
      }
      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
        if (ManifestContains(record)) { 
          // 在出错的情况下，判断一下是否已经有对应的记录了
          // 如果已经有对应的记录，就将s改为OK
          Log(options_->info_log,
              "MANIFEST contains log record despite error; advancing to new "
              "version to prevent mismatch between in-memory and logged state");
          s = Status::OK();
        }
      }
    }

    // If we just created a new descriptor file, install it by writing a
    // new CURRENT file that points to it.
    if (s.ok() && !new_manifest_file.empty()) {
      s = SetCurrentFile(env_, dbname_, manifest_file_number_);
      // No need to double-check MANIFEST in case of error since it
      // will be discarded below.
    }

    mu->Lock();
  }

  // Install the new version
  if (s.ok()) {
	// 将这个version添加到version set的链表中,并且作为current的version指针
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      delete descriptor_log_;
      delete descriptor_file_;
      descriptor_log_ = NULL;
      descriptor_file_ = NULL;
      env_->DeleteFile(new_manifest_file);
    }
  }

  return s;
}

Status VersionSet::Recover() {
  // 原来还能在函数里面定义一个类的
  struct LogReporter : public log::Reader::Reporter {
    Status* status;
    virtual void Corruption(size_t bytes, const Status& s) {
      if (this->status->ok()) *this->status = s;
    }
  };

  // Read "CURRENT" file, which contains a pointer to the current manifest file
  // 首先读取CURRENT文件
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current[current.size()-1] != '\n') {
    return Status::Corruption("CURRENT file does not end with newline");
  }
  current.resize(current.size() - 1);

  // 构建出CURRENT文件名
  std::string dscname = dbname_ + "/" + current;
  SequentialFile* file;
  s = env_->NewSequentialFile(dscname, &file);
  if (!s.ok()) {
    return s;
  }

  bool have_log_number = false;
  bool have_prev_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  Builder builder(this, current_);

  {
    LogReporter reporter;
    reporter.status = &s;
    log::Reader reader(file, &reporter, true/*checksum*/, 0/*initial_offset*/);
    Slice record;
    std::string scratch;
    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
     // 从这里可知从manifest文件中decode出来的是VersionEdit格式
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (s.ok()) {
        if (edit.has_comparator_ &&
            edit.comparator_ != icmp_.user_comparator()->Name()) {
          s = Status::InvalidArgument(
              edit.comparator_ + " does not match existing comparator ",
              icmp_.user_comparator()->Name());
        }
      }

      if (s.ok()) {
    	// 如果decode成功就写入builder中
        builder.Apply(&edit);
      }

      if (edit.has_log_number_) {
        log_number = edit.log_number_;
        have_log_number = true;
      }

      if (edit.has_prev_log_number_) {
        prev_log_number = edit.prev_log_number_;
        have_prev_log_number = true;
      }

      if (edit.has_next_file_number_) {
        next_file = edit.next_file_number_;
        have_next_file = true;
      }

      if (edit.has_last_sequence_) {
        last_sequence = edit.last_sequence_;
        have_last_sequence = true;
      }
    }
  }
  delete file;
  file = NULL;

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no meta-nextfile entry in descriptor");
    } else if (!have_log_number) {
      s = Status::Corruption("no meta-lognumber entry in descriptor");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence-number entry in descriptor");
    }

    if (!have_prev_log_number) {
      prev_log_number = 0;
    }

    MarkFileNumberUsed(prev_log_number);
    MarkFileNumberUsed(log_number);
  }

  if (s.ok()) {
    Version* v = new Version(this);
    // 将改动保存到Version中
    builder.SaveTo(v);
    // Install recovered version
    Finalize(v);
    AppendVersion(v);
    manifest_file_number_ = next_file;
    next_file_number_ = next_file + 1;
    last_sequence_ = last_sequence;
    log_number_ = log_number;
    prev_log_number_ = prev_log_number;
  }

  return s;
}

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

// 预计算下一次compact的最佳层次
void VersionSet::Finalize(Version* v) {
  // Precomputed best level for next compaction
  int best_level = -1;
  double best_score = -1;

  // 遍历所有的层次计算最佳score和level
  for (int level = 0; level < config::kNumLevels-1; level++) {
    double score;
    if (level == 0) {
      // We treat level-0 specially by bounding the number of files
      // instead of number of bytes for two reasons:
      //
      // (1) With larger write-buffer sizes, it is nice not to do too
      // many level-0 compactions.
      //
      // (2) The files in level-0 are merged on every read and
      // therefore we wish to avoid too many files when the individual
      // file size is small (perhaps because of a small write-buffer
      // setting, or very high compression ratios, or lots of
      // overwrites/deletions).
      // 上面一大段注释的翻译：
      // 对0级进行特殊处理，使用了文件数量而不是文件大小，是出于两个考虑：
      // (1)在有更大的写缓存大小的情况下，不会做很多0级的compact操作。
      // (2)每次读操作都可能触发0级文件的合并操作，所以我们更希望能避免在0级出现
      // 很多文件，而且每个文件的大小还很小的情况（可能是因为小的写缓存设置，或者
      // 高压缩比例，也或者是很多覆盖写、删除操作，等等原因造成的0级别的小文件）
      // level 0是根据这个级别的文件数量来计算分数
      score = v->files_[level].size() /
          static_cast<double>(config::kL0_CompactionTrigger);
    } else {
      // Compute the ratio of current size to size limit.
      // 其他级别是按照该级别所有文件的尺寸之和与来计算分数
      const uint64_t level_bytes = TotalFileSize(v->files_[level]);
      score = static_cast<double>(level_bytes) / MaxBytesForLevel(level);
    }

    // 记录下分数更高的级别和分数
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }

  // 记录下最高的分数和所在的级别
  v->compaction_level_ = best_level;
  v->compaction_score_ = best_score;
}

// 写当前的快照
Status VersionSet::WriteSnapshot(log::Writer* log) {
  // TODO: Break up into multiple records to reduce memory usage on recovery?

  // Save metadata
  VersionEdit edit;
  // 首先写compactor name
  edit.SetComparatorName(icmp_.user_comparator()->Name());

  // Save compaction pointers
  // 其次保存每个level的compact point
  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  // Save files
  // 最后保存每一级别的每个文件的文件大小,最大/最小 key
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

// 返回某个级别文件数量
int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return current_->files_[level].size();
}

const char* VersionSet::LevelSummary(LevelSummaryStorage* scratch) const {
  // Update code if kNumLevels changes
  assert(config::kNumLevels == 7);
  snprintf(scratch->buffer, sizeof(scratch->buffer),
           "files[ %d %d %d %d %d %d %d ]",
           int(current_->files_[0].size()),
           int(current_->files_[1].size()),
           int(current_->files_[2].size()),
           int(current_->files_[3].size()),
           int(current_->files_[4].size()),
           int(current_->files_[5].size()),
           int(current_->files_[6].size()));
  return scratch->buffer;
}

// Return true iff the manifest contains the specified record.
bool VersionSet::ManifestContains(const std::string& record) const {
  std::string fname = DescriptorFileName(dbname_, manifest_file_number_);
  Log(options_->info_log, "ManifestContains: checking %s\n", fname.c_str());
  SequentialFile* file = NULL;
  Status s = env_->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    Log(options_->info_log, "ManifestContains: %s\n", s.ToString().c_str());
    return false;
  }
  log::Reader reader(file, NULL, true/*checksum*/, 0);
  Slice r;
  std::string scratch;
  bool result = false;
  while (reader.ReadRecord(&r, &scratch)) {
    if (r == Slice(record)) {
      result = true;
      break;
    }
  }
  delete file;
  Log(options_->info_log, "ManifestContains: result = %d\n", result ? 1 : 0);
  return result;
}

// 估算某个key的偏移量
uint64_t VersionSet::ApproximateOffsetOf(Version* v, const InternalKey& ikey) {
  uint64_t result = 0;
  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = v->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      if (icmp_.Compare(files[i]->largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        // 比这个文件最大的key都大，要把这个文件的尺寸加上
        result += files[i]->file_size;
      } else if (icmp_.Compare(files[i]->smallest, ikey) > 0) {
        // 比这个文件最小的key都大，忽略这个文件
        // Entire file is after "ikey", so ignore
        if (level > 0) {
          // 非0级文件不会出现重复，是排序好的，所以遇到这种情况可以终止循环不再往下看了
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for
          // "ikey".
          break;
        }
      } else {
        // 这种情况下，key落在这个文件范围中，此时就需要精确计算这个文件的offset了
        // "ikey" falls in the range for this table.  Add the
        // approximate offset of "ikey" within the table.
        Table* tableptr;
        Iterator* iter = table_cache_->NewIterator(
            ReadOptions(), files[i]->number, files[i]->file_size, &tableptr);
        if (tableptr != NULL) {
          result += tableptr->ApproximateOffsetOf(ikey.Encode());
        }
        delete iter;
      }
    }
  }
  return result;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  for (Version* v = dummy_versions_.next_;
       v != &dummy_versions_;
       v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

// 返回某个级别文件的bytes
int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return TotalFileSize(current_->files_[level]);
}

// 返回下一个级别中有重叠的最大数量
int64_t VersionSet::MaxNextLevelOverlappingBytes() {
  int64_t result = 0;
  std::vector<FileMetaData*> overlaps;
  for (int level = 1; level < config::kNumLevels - 1; level++) {
    // 从1级开始遍历
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      // 遍历current_中每个级别的所有文件
      const FileMetaData* f = current_->files_[level][i];
      // 拿到下一个级别针对该该文件有覆盖的文件集合返回到overlaps参数中
      current_->GetOverlappingInputs(level+1, &f->smallest, &f->largest,
                                     &overlaps);
      // 根据overlaps集合得到这个重叠文件的大小集合
      const int64_t sum = TotalFileSize(overlaps);
      if (sum > result) {
        // 如果比当前结果大就更新
        result = sum;
      }
    }
  }
  return result;
}

// Stores the minimal range that covers all entries in inputs in
// *smallest, *largest.
// REQUIRES: inputs is not empty
// 找到inputs里面的最大值和最小值存储在smallest和largest中返回
void VersionSet::GetRange(const std::vector<FileMetaData*>& inputs,
                          InternalKey* smallest,
                          InternalKey* largest) {
  assert(!inputs.empty());
  smallest->Clear();
  largest->Clear();
  for (size_t i = 0; i < inputs.size(); i++) {
    FileMetaData* f = inputs[i];
    if (i == 0) {
      *smallest = f->smallest;
      *largest = f->largest;
    } else {
      if (icmp_.Compare(f->smallest, *smallest) < 0) {
        *smallest = f->smallest;
      }
      if (icmp_.Compare(f->largest, *largest) > 0) {
        *largest = f->largest;
      }
    }
  }
}

// 返回两个文件集合的最大、最小值返回
// Stores the minimal range that covers all entries in inputs1 and inputs2
// in *smallest, *largest.
// REQUIRES: inputs is not empty
void VersionSet::GetRange2(const std::vector<FileMetaData*>& inputs1,
                           const std::vector<FileMetaData*>& inputs2,
                           InternalKey* smallest,
                           InternalKey* largest) {
  std::vector<FileMetaData*> all = inputs1;
  all.insert(all.end(), inputs2.begin(), inputs2.end());
  GetRange(all, smallest, largest);
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_->paranoid_checks;
  options.fill_cache = false;

  // Level-0 files have to be merged together.  For other levels,
  // we will make a concatenating iterator per level.
  // TODO(opt): use concatenating iterator for level-0 if there is no overlap
  // 如果是0级文件,需要把这里面的所有0级文件存起来,所以取inputs_[0].size() + 1
  // 对于其他级别,level和level+1各一个文件就好了
  const int space = (c->level() == 0 ? c->inputs_[0].size() + 1 : 2);
  Iterator** list = new Iterator*[space];
  int num = 0;
  for (int which = 0; which < 2; which++) {
    if (!c->inputs_[which].empty()) {
      if (c->level() + which == 0) {
    	// 处理0级文件
        const std::vector<FileMetaData*>& files = c->inputs_[which];
        for (size_t i = 0; i < files.size(); i++) {
          // 对于0级文件而言,所有的迭代器都是遍历该sstable的迭代器
          list[num++] = table_cache_->NewIterator(
              options, files[i]->number, files[i]->file_size);
        }
      } else {
        // Create concatenating iterator for the files from this level
    	// 对于非0级,创建了一个concatenating iterator来遍历这个级别的所有文件
        list[num++] = NewTwoLevelIterator(
        	// 这里的index iter是LevelFileNumIterator,这是在遍历排序好的FileMetaData数组的迭代器
            new Version::LevelFileNumIterator(icmp_, &c->inputs_[which]),
            // GetFileIterator返回的是遍历一个sstable的迭代器
            &GetFileIterator, table_cache_, options);
        // 综合以上,这里得到的迭代器,首先会在一组排序好的FileMetaData数组中选择一个FileMetaData,然后再在这个FileMetaData表示的sstable中遍历的迭代器
        // 换言之, 非0级的迭代器是将该等级的文件集合在一起进行遍历
      }
    }
  }
  assert(num <= space);
  // 将以上得到的迭代器数组再进行merge
  Iterator* result = NewMergingIterator(&icmp_, list, num);
  delete[] list;
  return result;
}

// 选择compact文件
Compaction* VersionSet::PickCompaction() {
  Compaction* c;
  int level;

  // We prefer compactions triggered by too much data in a level over
  // the compactions triggered by seeks.
  // compaction_score_在VersionSet::Finalize中计算,
  // 这种情况是某个级别的文件尺寸大小超过了阈值需要compact
  const bool size_compaction = (current_->compaction_score_ >= 1);
  // file_to_compact_在Version::UpdateStats函数中计算
  // 这种情况是某个文件的seek次数太多，需要compact
  const bool seek_compaction = (current_->file_to_compact_ != NULL);
  if (size_compaction) {
	  // 如果有compaction_score_ >= 1的情况,优先考虑这种情况
    level = current_->compaction_level_;
    assert(level >= 0);
    assert(level+1 < config::kNumLevels);
    c = new Compaction(level);

    // Pick the first file that comes after compact_pointer_[level]
    // 查找第一个包含比上次已经compact的最大key大的key的文件
    for (size_t i = 0; i < current_->files_[level].size(); i++) {
      FileMetaData* f = current_->files_[level][i];
      if (compact_pointer_[level].empty() || //如果当前层的compact_pointer_为空
          icmp_.Compare(f->largest.Encode(), compact_pointer_[level]) > 0) { // 或者找到了第一个大于compact_pointer_的文件
        c->inputs_[0].push_back(f);
        break;
      }
    }
    // 如果前面的循环找不到,说明已经找了该级别的所有文件了,就把该级别的第一个文件push进去
    // 也就是回绕回去(wrap-aound)
    if (c->inputs_[0].empty()) {
      // Wrap-around to the beginning of the key space
      c->inputs_[0].push_back(current_->files_[level][0]);
    }
  } else if (seek_compaction) {
	  // 然后才考虑file_to_compact_不为空的情况
	  // file_to_compact_是allow_seeks为0的等级
    level = current_->file_to_compact_level_;
    c = new Compaction(level);
    c->inputs_[0].push_back(current_->file_to_compact_);
  } else {
    return NULL;
  }

  c->input_version_ = current_;
  c->input_version_->Ref();

  // Files in level 0 may overlap each other, so pick up all overlapping ones
  if (level == 0) {
	  // 由于0级可能有多个文件的key重复现象,所以如果是0级那么需要把满足范围的文件一起拿到
    InternalKey smallest, largest;
    GetRange(c->inputs_[0], &smallest, &largest);
    // Note that the next call will discard the file we placed in
    // c->inputs_[0] earlier and replace it with an overlapping set
    // which will include the picked file.
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  }

  // 以上计算好了inputs[0],现在开始计算Inputs[1]
  SetupOtherInputs(c);

  return c;
}

// 作用是为了在不影响性能的情况下尽可能多的compaction当前level的文件
void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest, largest;
  // 首先拿到level级所有文件中得最大值和最小值
  GetRange(c->inputs_[0], &smallest, &largest);

  // 遍历level + 1级,得到level + 1级中也在该范围内的文件将它们放到inputs[1]中
  current_->GetOverlappingInputs(level+1, &smallest, &largest, &c->inputs_[1]);

  // Get entire range covered by compaction
  // 所有inputs的开始结束范围
  InternalKey all_start, all_limit;
  GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  // 看是否能在不改变前面已经取出的level+1文件数量的条件下
  // 增加level级别文件的数量
  // 看能否将level中与取出的level+1中的range重叠的也加到inputs中，
  // 而新加的文件的range都在已经加入的level+1的文件的范围中
  if (!c->inputs_[1].empty()) {
    std::vector<FileMetaData*> expanded0;
    // 在level中寻找范围在[all_start, all_limit]中的文件,
    // 新增加的level级别的文件在expended0中返回
    current_->GetOverlappingInputs(level, &all_start, &all_limit, &expanded0);
    // 几种集合的文件大小之和
    
    // level级别的文件大小
    const int64_t inputs0_size = TotalFileSize(c->inputs_[0]);
    // level+1级别的文件大小
    const int64_t inputs1_size = TotalFileSize(c->inputs_[1]);
    // level级别新增的文件大小
    const int64_t expanded0_size = TotalFileSize(expanded0);
    if (expanded0.size() > c->inputs_[0].size() && // 这个条件表示在level中取[all_start, all_limit]的话比原来的inputs[0]范围大
        inputs1_size + expanded0_size < kExpandedCompactionByteSizeLimit) {
      // 满足 inputs1_size + expanded0_size < kExpandedCompactionByteSizeLimit表示现在可以不compact level + 1的文件
      InternalKey new_start, new_limit;
      // 拿到expanded0的范围[new_start, new_limit]
      GetRange(expanded0, &new_start, &new_limit);
      std::vector<FileMetaData*> expanded1;
      // 拿到level + 1中overlap范围[new_start, new_limit]的文件
      current_->GetOverlappingInputs(level+1, &new_start, &new_limit,
                                     &expanded1);
      if (expanded1.size() == c->inputs_[1].size()) { // expanded1和inputs[1]大小相同,而expanded0>inputs[0]
  	  	  	  	  	  	  	  	  	  	  	  	  	  // 那么说明可以扩展level的文件数量而不改变level + 1的文件数量
        Log(options_->info_log,
            "Expanding@%d %d+%d (%ld+%ld bytes) to %d+%d (%ld+%ld bytes)\n",
            level,
            int(c->inputs_[0].size()),
            int(c->inputs_[1].size()),
            long(inputs0_size), long(inputs1_size),
            int(expanded0.size()),
            int(expanded1.size()),
            long(expanded0_size), long(inputs1_size));
        smallest = new_start;
        largest = new_limit;
        c->inputs_[0] = expanded0;
        c->inputs_[1] = expanded1;
        GetRange2(c->inputs_[0], c->inputs_[1], &all_start, &all_limit);
      }
    }
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  // 如果level+2还没到最大层次，那么取得level+2 中重叠的文件放入grandparents_
  if (level + 2 < config::kNumLevels) {
	  // 计算level + 2中也在[all_start,all_limit]范围的文件
    // 用于后面计算level+2级别的overlap时使用
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit,
                                   &c->grandparents_);
  }

  if (false) {
    Log(options_->info_log, "Compacting %d '%s' .. '%s'",
        level,
        smallest.DebugString().c_str(),
        largest.DebugString().c_str());
  }

  // Update the place where we will do the next compaction for this level.
  // We update this immediately instead of waiting for the VersionEdit
  // to be applied so that if the compaction fails, we will try a different
  // key range next time.
  // 这里记录下来下一次要compact时该level的文件从哪里开始
  compact_pointer_[level] = largest.Encode().ToString();
  c->edit_.SetCompactPointer(level, largest);
}

// 返回一个Compaction指针，用于在level级别针对[begin,end]返回的数据进行compact操作
Compaction* VersionSet::CompactRange(
    int level,
    const InternalKey* begin,
    const InternalKey* end) {
  // 首先得到满足要求的文件集合到inputs变量中
  std::vector<FileMetaData*> inputs;
  current_->GetOverlappingInputs(level, begin, end, &inputs);
  if (inputs.empty()) {
    return NULL;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // 避免出现compact太多文件的情况
  // 首先得到该level的文件大小上限
  const uint64_t limit = MaxFileSizeForLevel(level);
  uint64_t total = 0;
  for (size_t i = 0; i < inputs.size(); i++) {
    uint64_t s = inputs[i]->file_size;
    total += s;
    // 这里没有看懂。。。
    if (total >= limit) {
      inputs.resize(i + 1);
      break;
    }
  }

  Compaction* c = new Compaction(level);
  c->input_version_ = current_;
  c->input_version_->Ref();
  c->inputs_[0] = inputs;
  SetupOtherInputs(c);
  return c;
}

Compaction::Compaction(int level)
    : level_(level),
      max_output_file_size_(MaxFileSizeForLevel(level)),
      input_version_(NULL),
      grandparent_index_(0),
      seen_key_(false),
      overlapped_bytes_(0) {
  for (int i = 0; i < config::kNumLevels; i++) {
    level_ptrs_[i] = 0;
  }
}

Compaction::~Compaction() {
  if (input_version_ != NULL) {
    input_version_->Unref();
  }
}

// 返回true表示不需要合并，只要移动文件到上一层就好了
bool Compaction::IsTrivialMove() const {
  // Avoid a move if there is lots of overlapping grandparent data.
  // Otherwise, the move could create a parent file that will require
  // a very expensive merge later on.
  return (num_input_files(0) == 1 &&  // level 只有一个文件
          num_input_files(1) == 0 &&  // level + 1没有文件
          // 有重叠的爷爷辈文件大小之和小于阈值
          TotalFileSize(grandparents_) <= kMaxGrandParentOverlapBytes);
}

// 将这个compact中的所有文件输入到edit中作为待删除的文件
void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < inputs_[which].size(); i++) {
      edit->DeleteFile(level_ + which, inputs_[which][i]->number);
    }
  }
}

// 检查在level + 2及更高的等级上,有没有找到user_key
// 如果都没有找到，说明这个级别就是针对这个key的base level
bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Maybe use binary search to find right entry instead of linear search?
  // 以下使用的线性查找的办法，可以使用二分查找来替换这个算法？
  const Comparator* user_cmp = input_version_->vset_->icmp_.user_comparator();
  for (int lvl = level_ + 2; lvl < config::kNumLevels; lvl++) {
    const std::vector<FileMetaData*>& files = input_version_->files_[lvl];
    for (; level_ptrs_[lvl] < files.size(); ) {
      FileMetaData* f = files[level_ptrs_[lvl]];
      if (user_cmp->Compare(user_key, f->largest.user_key()) <= 0) {
        // We've advanced far enough
        if (user_cmp->Compare(user_key, f->smallest.user_key()) >= 0) {
          // Key falls in this file's range, so definitely not base level
          // 在这个范围内,返回false
          return false;
        }
        break;
      }
      // 每次检查完毕，都将这个计数器递增，这样下一次查找就从上一次停止查找的位置开始继续查找
      level_ptrs_[lvl]++;
    }
  }
  return true;
}

// 判断这个key的加入会不会使得当前output的sstable和grantparents有太多的overlap
bool Compaction::ShouldStopBefore(const Slice& internal_key) {
  // Scan to find earliest grandparent file that contains key.
  const InternalKeyComparator* icmp = &input_version_->vset_->icmp_;
  // 寻找爷爷辈级别的文件中含有这个key的最小文件
  while (grandparent_index_ < grandparents_.size() &&
      icmp->Compare(internal_key,	// 当传入的internal_key一直大于该文件的最大key时这个循环一直下去
                    grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      // 如果之前已经看到这个key了,那要累加overlap范围的大小
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    grandparent_index_++;
  }
  // 第一次进来该函数就会置为true，第二次以后进来都为true了
  seen_key_ = true;

  if (overlapped_bytes_ > kMaxGrandParentOverlapBytes) {
    // Too much overlap for current output; start new output
	  // 如果overlap大小超过了一定范围,返回true
    overlapped_bytes_ = 0;
    return true;
  } else {
    return false;
  }
}

void Compaction::ReleaseInputs() {
  if (input_version_ != NULL) {
    input_version_->Unref();
    input_version_ = NULL;
  }
}

}  // namespace leveldb
