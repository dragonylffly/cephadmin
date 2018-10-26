// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Bitmap based in memory allocator.
 * Author: Ramesh Chander, Ramesh.Chander@sandisk.com
 */

#ifndef  CEPH_OS_BLUESTORE_BITALLOCATOR_H
#define CEPH_OS_BLUESTORE_BITALLOCATOR_H


#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <vector>
#include "intarith.h"
//#include "os/bluestore/bluestore_types.h"

using namespace std;

#define alloc_assert assert

#ifdef BIT_ALLOCATOR_DEBUG
#define alloc_dbg_assert(x) assert(x)
#else
#define alloc_dbg_assert(x) (static_cast<void> (0))
#endif

#define ceph_abort() abort()

typedef struct {
    uint64_t span_size;
    uint64_t blocks_per_zone;
} Config;

class CephContext;

class AllocExtent;
typedef vector<AllocExtent> AllocExtentVector;


class Allocator {
public:
  virtual ~Allocator() {}

  virtual int reserve(uint64_t need) = 0;
  virtual void unreserve(uint64_t unused) = 0;

  /*
   * Allocate required number of blocks in n number of extents.
   * Min and Max number of extents are limited by:
   * a. alloc unit
   * b. max_alloc_size.
   * as no extent can be lesser than alloc_unit and greater than max_alloc size.
   * Apart from that extents can vary between these lower and higher limits according
   * to free block search algorithm and availability of contiguous space.
   */
  virtual int64_t allocate(uint64_t want_size, uint64_t alloc_unit,
			   uint64_t max_alloc_size, int64_t hint,
			   AllocExtentVector *extents) = 0;

  int64_t allocate(uint64_t want_size, uint64_t alloc_unit,
		   int64_t hint, AllocExtentVector *extents) {
    return allocate(want_size, alloc_unit, want_size, hint, extents);
  }

  virtual void release(
    uint64_t offset, uint64_t length) = 0;

  virtual void dump() = 0;

  virtual void init_add_free(uint64_t offset, uint64_t length) = 0;
  virtual void init_rm_free(uint64_t offset, uint64_t length) = 0;

  virtual uint64_t get_free() = 0;

  virtual void shutdown() = 0;
  static Allocator *create(Config* conf, string type, int64_t size,
			   int64_t block_size);
};



class AllocExtent {
public:
  uint64_t offset;
  uint32_t length;

  AllocExtent() { 
    offset = 0;
    length = 0;
  }

  AllocExtent(int64_t off, int32_t len) : offset(off), length(len) { }
  uint64_t end() const {
    return offset + length;
  }
  bool operator==(const AllocExtent& other) const {
    return offset == other.offset && length == other.length;
  }
};


class ExtentList {
  AllocExtentVector *m_extents;
  int64_t m_block_size;
  int64_t m_max_blocks;

public:
  void init(AllocExtentVector *extents, int64_t block_size,
	    uint64_t max_alloc_size) {
    m_extents = extents;
    m_block_size = block_size;
    m_max_blocks = max_alloc_size / block_size;
    assert(m_extents->empty());
  }

  ExtentList(AllocExtentVector *extents, int64_t block_size) {
    init(extents, block_size, 0);
  }

  ExtentList(AllocExtentVector *extents, int64_t block_size,
	     uint64_t max_alloc_size) {
    init(extents, block_size, max_alloc_size);
  }

  void reset() {
    m_extents->clear();
  }

  void add_extents(int64_t start, int64_t count);

  AllocExtentVector *get_extents() {
    return m_extents;
  }

  std::pair<int64_t, int64_t> get_nth_extent(int index) {
      return std::make_pair
            ((*m_extents)[index].offset / m_block_size,
             (*m_extents)[index].length / m_block_size);
  }

  int64_t get_extent_count() {
    return m_extents->size();
  }
};


class BitAllocatorStats {
public:
  std::atomic<int64_t> m_total_alloc_calls;
  std::atomic<int64_t> m_total_free_calls;
  std::atomic<int64_t> m_total_allocated;
  std::atomic<int64_t> m_total_freed;
  std::atomic<int64_t> m_total_serial_scans;
  std::atomic<int64_t> m_total_concurrent_scans;
  std::atomic<int64_t> m_total_node_scanned;

  BitAllocatorStats() {
    m_total_alloc_calls = 0;
    m_total_free_calls = 0;
    m_total_allocated = 0;
    m_total_freed = 0;
    m_total_serial_scans = 0;
    m_total_concurrent_scans = 0;
    m_total_node_scanned = 0;
  }

  void add_alloc_calls(int64_t val) {
    std::atomic_fetch_add(&m_total_alloc_calls, val);
  }
  void add_free_calls(int64_t val) {
    std::atomic_fetch_add(&m_total_free_calls, val);
  }
  void add_allocated(int64_t val) {
    std::atomic_fetch_add(&m_total_allocated, val);
  }
  void add_freed(int64_t val) {
    std::atomic_fetch_add(&m_total_freed, val);
  }
  void add_serial_scans(int64_t val) {
    std::atomic_fetch_add(&m_total_serial_scans, val);
  }
  void add_concurrent_scans(int64_t val) {
    std::atomic_fetch_add(&m_total_concurrent_scans, val);
  }
  void add_node_scanned(int64_t val) {
    std::atomic_fetch_add(&m_total_node_scanned, val);
  }
};

template <class BitMapEntity>
class BitMapEntityIter {
  typedef vector<BitMapEntity> BitMapEntityVector;
  BitMapEntityVector *m_list;
  int64_t m_start_idx;
  int64_t m_cur_idx;
  bool m_wrap;
  bool m_wrapped;
  bool m_end;
public:

  void init(BitMapEntityVector *list, bool wrap, int64_t start_idx) {
    m_list = list;
    m_wrap = wrap;
    m_start_idx = start_idx;
    m_cur_idx = m_start_idx;
    m_wrapped = false;
    m_end = false;
  }

  BitMapEntityIter(BitMapEntityVector *list, int64_t start_idx) {
    init(list, false, start_idx);
  }
  BitMapEntityIter(BitMapEntityVector *list, int64_t start_idx, bool wrap) {
    init(list, wrap, start_idx);
  }

  BitMapEntity *next() {
    int64_t cur_idx = m_cur_idx;

    if (m_wrapped &&
      cur_idx == m_start_idx) {
      /*
       * End of wrap cycle + 1
       */
      if (!m_end) {
        m_end = true;
        return &(*m_list)[cur_idx];
      }
      return NULL;
    }
    m_cur_idx++;

    if (m_cur_idx == (int64_t)m_list->size() &&
        m_wrap) {
      m_cur_idx = 0;
      m_wrapped = true;
    }

    if (cur_idx == (int64_t)m_list->size()) {
      /*
       * End of list
       */
      return NULL;
    }

    alloc_assert(cur_idx < (int64_t)m_list->size());
    return &(*m_list)[cur_idx];
  }

  int64_t index() {
    return m_cur_idx;
  }
};

typedef unsigned long bmap_t;
typedef vector<bmap_t> bmap_mask_vec_t;

class BmapEntry {
private:
  bmap_t m_bits;

public:
  static bmap_t full_bmask() {
    return (bmap_t) -1;
  }
  static int64_t size() {
    return (sizeof(bmap_t) * 8);
  }
  static bmap_t empty_bmask() {
    return (bmap_t) 0;
  }
  static bmap_t align_mask(int x) {
    return ((x) >= BmapEntry::size()? (bmap_t) -1 : (~(((bmap_t) -1) >> (x))));
  }
  static bmap_t bit_mask(int bit_num) {
    return (bmap_t) 0x1 << ((BmapEntry::size() - 1) - bit_num);
  }
  bmap_t atomic_fetch() {
    return m_bits;
  }
  BmapEntry(Config*, bool val);
  BmapEntry(Config*) {
    m_bits = 0;
  }
  BmapEntry(const BmapEntry& bmap) {
    m_bits = bmap.m_bits;
  }

  void clear_bit(int bit);
  void clear_bits(int offset, int num_bits);
  void set_bits(int offset, int num_bits);
  bool check_n_set_bit(int bit);
  bool check_bit(int bit);
  bool is_allocated(int64_t start_bit, int64_t num_bits);

  int find_n_cont_bits(int start_offset, int64_t num_bits);
  int find_n_free_bits(int start_idx, int64_t max_bits,
           int *free_bit, int *end_idx);
  int find_first_set_bits(int64_t required_blocks, int bit_offset,
          int *start_offset, int64_t *scanned);

  void dump_state(Config* conf, const int& count);
  ~BmapEntry();

};

class BitMapArea {
protected:
  int16_t m_area_index;

public:
  static int64_t get_zone_size(Config *conf);
  static int64_t get_span_size(Config *conf);
  static int get_level(Config *conf, int64_t total_blocks);
  static int64_t get_level_factor(Config *conf, int level);
  virtual bool is_allocated(int64_t start_block, int64_t num_blocks) = 0;
  virtual bool is_exhausted() = 0;
  virtual bool child_check_n_lock(BitMapArea *child, int64_t required) {
      ceph_abort();
      return true;
  }
  virtual bool child_check_n_lock(BitMapArea *child, int64_t required, bool lock) {
      ceph_abort();
      return true;
  }
  virtual void child_unlock(BitMapArea *child) {
    ceph_abort();
  }

  virtual void lock_excl() = 0;
  virtual bool lock_excl_try() {
    ceph_abort();
    return false;
  }
  virtual void lock_shared() {
    ceph_abort();
    return;
  }
  virtual void unlock() = 0;

  virtual int64_t sub_used_blocks(int64_t num_blocks) = 0;
  virtual int64_t add_used_blocks(int64_t num_blocks) = 0;
  virtual bool reserve_blocks(int64_t num_blocks) = 0;
  virtual void unreserve(int64_t num_blocks, int64_t allocated) = 0;
  virtual int64_t get_reserved_blocks() = 0;
  virtual int64_t get_used_blocks() = 0;

  virtual void shutdown() = 0;

  virtual int64_t alloc_blocks_dis(int64_t num_blocks, int64_t min_alloc,
             int64_t hint, int64_t blk_off, ExtentList *block_list) {
    ceph_abort();
    return 0;
  }

  virtual void set_blocks_used(int64_t start_block, int64_t num_blocks) = 0;
  virtual void free_blocks(int64_t start_block, int64_t num_blocks) = 0;
  virtual int64_t size() = 0;

  int64_t child_count();
  int64_t get_index();
  int64_t get_level();
  virtual void dump_state(Config *conf, int& count) = 0;
  BitMapArea(Config *conf) { }
  virtual ~BitMapArea() { }
};

class BitMapAreaList {

private:
  std::vector<BitMapArea*> m_items;

public:
  /* Must be DefaultConstructible as BitMapAreaIN and derivates employ
   * a deferred init, sorry. */
  BitMapAreaList() = default;

  BitMapAreaList(std::vector<BitMapArea*>&& m_items)
    : m_items(std::move(m_items)) {
  }

  BitMapArea *get_nth_item(const int64_t idx) {
    return m_items[idx];
  }

  /* FIXME: we really should use size_t. */
  int64_t size() const {
    return m_items.size();
  }
};

/* Intensionally inlined for the sake of BitMapAreaLeaf::alloc_blocks_dis_int. */
class BmapEntityListIter {
  BitMapAreaList* m_list;
  int64_t m_start_idx;
  int64_t m_cur_idx;
  bool m_wrap;
  bool m_wrapped;
  bool m_end;

public:
  BmapEntityListIter(BitMapAreaList* const list,
                     const int64_t start_idx,
                     const bool wrap = false)
    : m_list(list),
      m_start_idx(start_idx),
      m_cur_idx(start_idx),
      m_wrap(wrap),
      m_wrapped(false),
      m_end(false) {
  }

  BitMapArea* next() {
    int64_t cur_idx = m_cur_idx;

    if (m_wrapped &&
      cur_idx == m_start_idx) {
      /*
       * End of wrap cycle + 1
       */
      if (!m_end) {
        m_end = true;
        return m_list->get_nth_item(cur_idx);
      }
      return NULL;
    }
    m_cur_idx++;

    if (m_cur_idx == m_list->size() &&
        m_wrap) {
      m_cur_idx = 0;
      m_wrapped = true;
    }
    if (cur_idx == m_list->size()) {
      /*
       * End of list
       */
      return NULL;
    }

    /* This method should be *really* fast as it's being executed over
     * and over during traversal of allocators indexes. */
    alloc_dbg_assert(cur_idx < m_list->size());
    return m_list->get_nth_item(cur_idx);
  }

  int64_t index();
};

typedef vector<BmapEntry> BmapEntryVector;

class BitMapZone: public BitMapArea {

private:
  std::atomic<int32_t> m_used_blocks;
  BmapEntryVector m_bmap_vec;
  std::mutex m_lock;

public:
  static int64_t count;
  static int64_t total_blocks;
  static void incr_count() { count++;}
  static int64_t get_total_blocks() {return total_blocks;}
  bool is_allocated(int64_t start_block, int64_t num_blocks) override;
  bool is_exhausted() override final;
  void reset_marker();

  int64_t sub_used_blocks(int64_t num_blocks) override;
  int64_t add_used_blocks(int64_t num_blocks) override;
  bool reserve_blocks(int64_t num_blocks) override;
  void unreserve(int64_t num_blocks, int64_t allocated) override;
  int64_t get_reserved_blocks() override;
  int64_t get_used_blocks() override final;
  int64_t size() override final {
    return get_total_blocks();
  }

  void lock_excl() override;
  bool lock_excl_try() override;
  void unlock() override;
  bool check_locked();

  void free_blocks_int(int64_t start_block, int64_t num_blocks);
  void init(Config* conf, int64_t zone_num, int64_t total_blocks, bool def);

  BitMapZone(Config* conf, int64_t total_blocks, int64_t zone_num);
  BitMapZone(Config* conf, int64_t total_blocks, int64_t zone_num, bool def);

  ~BitMapZone() override;
  void shutdown() override;
  int64_t alloc_blocks_dis(int64_t num_blocks, int64_t min_alloc, int64_t hint,
        int64_t blk_off, ExtentList *block_list) override;  
  void set_blocks_used(int64_t start_block, int64_t num_blocks) override;

  void free_blocks(int64_t start_block, int64_t num_blocks) override;
  void dump_state(Config* conf, int& count) override;
};

class BitMapAreaIN: public BitMapArea{

protected:
  int64_t m_child_size_blocks;
  int64_t m_total_blocks;
  int16_t m_level;

  int64_t m_used_blocks;
  int64_t m_reserved_blocks;
  std::mutex m_blocks_lock;
  BitMapAreaList m_child_list;

  bool is_allocated(int64_t start_block, int64_t num_blocks) override;
  bool is_exhausted() override;

  bool child_check_n_lock(BitMapArea *child, int64_t required, bool lock) override {
    ceph_abort();
    return false;
  }

  bool child_check_n_lock(BitMapArea *child, int64_t required) override;
  void child_unlock(BitMapArea *child) override;

  void lock_excl() override {
    return;
  }
  void lock_shared() override {
    return;
  }
  void unlock() override {
    return;
  }

  void init(Config* conf, int64_t total_blocks, int64_t zone_size_block, bool def);
  void init_common(Config* conf,
                   int64_t total_blocks,
                   int64_t zone_size_block,
                   bool def);
  int64_t alloc_blocks_dis_int_work(bool wrap, int64_t num_blocks, int64_t min_alloc, int64_t hint,
        int64_t blk_off, ExtentList *block_list);  

  int64_t alloc_blocks_int_work(bool wait, bool wrap,
                         int64_t num_blocks, int64_t hint, int64_t *start_block);

public:
  BitMapAreaIN(Config *conf);
  BitMapAreaIN(Config* conf, int64_t zone_num, int64_t total_blocks);
  BitMapAreaIN(Config* conf, int64_t zone_num, int64_t total_blocks,
	       bool def);

  ~BitMapAreaIN() override;
  void shutdown() override;
  int64_t sub_used_blocks(int64_t num_blocks) override;
  int64_t add_used_blocks(int64_t num_blocks) override;
  bool reserve_blocks(int64_t num_blocks) override;
  void unreserve(int64_t num_blocks, int64_t allocated) override;
  int64_t get_reserved_blocks() override;
  int64_t get_used_blocks() override;
  virtual int64_t get_used_blocks_adj();
  int64_t size() override {
    return m_total_blocks;
  }
  using BitMapArea::alloc_blocks_dis; //non-wait version

  virtual int64_t alloc_blocks_dis_int(int64_t num_blocks, int64_t min_alloc, int64_t hint,
                                       int64_t blk_off, ExtentList *block_list);  
  int64_t alloc_blocks_dis(int64_t num_blocks, int64_t min_alloc, int64_t hint,
                           int64_t blk_off, ExtentList *block_list) override;  
  virtual void set_blocks_used_int(int64_t start_block, int64_t num_blocks);
  void set_blocks_used(int64_t start_block, int64_t num_blocks) override;

  virtual void free_blocks_int(int64_t start_block, int64_t num_blocks);
  void free_blocks(int64_t start_block, int64_t num_blocks) override;
  void dump_state(Config* conf, int& count) override;
};

class BitMapAreaLeaf: public BitMapAreaIN{

private:
  void init(Config *conf, int64_t total_blocks, int64_t zone_size_block,
            bool def);

public:
  static int64_t count;
  static void incr_count() { count++;}
  BitMapAreaLeaf(Config *conf) : BitMapAreaIN(conf) { }
  BitMapAreaLeaf(Config* conf, int64_t zone_num, int64_t total_blocks);
  BitMapAreaLeaf(Config* conf, int64_t zone_num, int64_t total_blocks,
		 bool def);

  using BitMapAreaIN::child_check_n_lock;
  bool child_check_n_lock(BitMapArea *child, int64_t required) override {
    ceph_abort();
    return false;
  }

  bool child_check_n_lock(BitMapZone* child, int64_t required, bool lock);

  int64_t alloc_blocks_int(int64_t num_blocks, int64_t hint, int64_t *start_block);
  int64_t alloc_blocks_dis_int(int64_t num_blocks, int64_t min_alloc, int64_t hint,
        int64_t blk_off, ExtentList *block_list) override;  
  void free_blocks_int(int64_t start_block, int64_t num_blocks) override;

  ~BitMapAreaLeaf() override;
};


typedef enum bmap_alloc_mode {
  SERIAL = 1,
  CONCURRENT = 2,
} bmap_alloc_mode_t;

class BitAllocator:public BitMapAreaIN{
private:
  Config* const conf;
  bmap_alloc_mode_t m_alloc_mode;
  std::mutex m_serial_mutex;
  pthread_rwlock_t m_rw_lock;
  BitAllocatorStats *m_stats;
  bool m_is_stats_on;
  int64_t m_extra_blocks;

  bool is_stats_on() {
    return m_is_stats_on;
  }

  using BitMapArea::child_check_n_lock;
  bool child_check_n_lock(BitMapArea *child, int64_t required) override;
  void child_unlock(BitMapArea *child) override;

  void serial_lock();
  bool try_serial_lock();
  void serial_unlock();
  void lock_excl() override;
  void lock_shared() override;
  bool try_lock();
  void unlock() override;

  bool check_input(int64_t num_blocks);
  bool check_input_dis(int64_t num_blocks);
  void init_check(int64_t total_blocks, int64_t zone_size_block,
                 bmap_alloc_mode_t mode, bool def, bool stats_on);
  int64_t alloc_blocks_dis_work(int64_t num_blocks, int64_t min_alloc, int64_t hint, ExtentList *block_list, bool reserved);

  int64_t alloc_blocks_dis_int(int64_t num_blocks, int64_t min_alloc, 
           int64_t hint, int64_t area_blk_off, ExtentList *block_list) override;

public:
  BitAllocator(Config* conf, int64_t total_blocks,
	       int64_t zone_size_block, bmap_alloc_mode_t mode);
  BitAllocator(Config* conf, int64_t total_blocks, int64_t zone_size_block,
	       bmap_alloc_mode_t mode, bool def);
  BitAllocator(Config* conf, int64_t total_blocks, int64_t zone_size_block,
               bmap_alloc_mode_t mode, bool def, bool stats_on);
  ~BitAllocator() override;
  void shutdown() override;
  using BitMapAreaIN::alloc_blocks_dis; //Wait version

  void free_blocks(int64_t start_block, int64_t num_blocks) override;
  void set_blocks_used(int64_t start_block, int64_t num_blocks) override;
  void unreserve_blocks(int64_t blocks);

  int64_t alloc_blocks_dis_res(int64_t num_blocks, int64_t min_alloc, int64_t hint, ExtentList *block_list);

  void free_blocks_dis(int64_t num_blocks, ExtentList *block_list);
  bool is_allocated_dis(ExtentList *blocks, int64_t num_blocks);

  int64_t total_blocks() const {
    return m_total_blocks - m_extra_blocks;
  }
  int64_t get_used_blocks() override {
    return (BitMapAreaIN::get_used_blocks_adj() - m_extra_blocks);
  }

  BitAllocatorStats *get_stats() {
      return m_stats;
  }
  void dump();
};

#endif //End of file
