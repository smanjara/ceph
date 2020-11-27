// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <random>

#include "test/crimson/gtest_seastar.h"

#include "crimson/os/seastore/segment_cleaner.h"
#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/transaction_manager.h"
#include "crimson/os/seastore/segment_manager.h"

#include "test/crimson/seastore/test_block.h"

using namespace crimson;
using namespace crimson::os;
using namespace crimson::os::seastore;

namespace {
  [[maybe_unused]] seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_test);
  }
}

struct test_extent_record_t {
  test_extent_desc_t desc;
  unsigned refcount = 0;
  test_extent_record_t() = default;
  test_extent_record_t(
    const test_extent_desc_t &desc,
    unsigned refcount) : desc(desc), refcount(refcount) {}

  void update(const test_extent_desc_t &to) {
    desc = to;
  }

  bool operator==(const test_extent_desc_t &rhs) const {
    return desc == rhs;
  }
  bool operator!=(const test_extent_desc_t &rhs) const {
    return desc != rhs;
  }
};

std::ostream &operator<<(std::ostream &lhs, const test_extent_record_t &rhs) {
  return lhs << "test_extent_record_t(" << rhs.desc
	     << ", refcount=" << rhs.refcount << ")";
}

struct transaction_manager_test_t : public seastar_test_suite_t {
  std::unique_ptr<SegmentManager> segment_manager;
  std::unique_ptr<SegmentCleaner> segment_cleaner;
  std::unique_ptr<Journal> journal;
  std::unique_ptr<Cache> cache;
  LBAManagerRef lba_manager;
  std::unique_ptr<TransactionManager> tm;

  std::random_device rd;
  std::mt19937 gen;

  transaction_manager_test_t()
    : segment_manager(create_ephemeral(segment_manager::DEFAULT_TEST_EPHEMERAL)),
      gen(rd()) {
    init();
  }

  void init() {
    segment_cleaner = std::make_unique<SegmentCleaner>(
      SegmentCleaner::config_t::default_from_segment_manager(
	*segment_manager));
    journal = std::make_unique<Journal>(*segment_manager);
    cache = std::make_unique<Cache>(*segment_manager);
    lba_manager = lba_manager::create_lba_manager(*segment_manager, *cache);
    tm = std::make_unique<TransactionManager>(
      *segment_manager, *segment_cleaner, *journal, *cache, *lba_manager);

    journal->set_segment_provider(&*segment_cleaner);
    segment_cleaner->set_extent_callback(&*tm);
  }

  void destroy() {
    tm.reset();
    lba_manager.reset();
    cache.reset();
    journal.reset();
    segment_cleaner.reset();
  }

  laddr_t get_random_laddr(size_t block_size, laddr_t limit) {
    return block_size *
      std::uniform_int_distribution<>(0, (limit / block_size) - 1)(gen);
  }

  char get_random_contents() {
    return static_cast<char>(std::uniform_int_distribution<>(0, 255)(gen));
  }

  seastar::future<> set_up_fut() final {
    return segment_manager->init().safe_then([this] {
      return tm->mkfs();
    }).safe_then([this] {
      return tm->mount();
    }).handle_error(
      crimson::ct_error::all_same_way([] {
	ASSERT_FALSE("Unable to mount");
      })
    );
  }

  seastar::future<> tear_down_fut() final {
    return tm->close(
    ).handle_error(
      crimson::ct_error::all_same_way([] {
	ASSERT_FALSE("Unable to close");
      })
    );
  }

  struct test_extents_t : std::map<laddr_t, test_extent_record_t> {
  private:
    void check_available(laddr_t addr, extent_len_t len) {
      auto iter = upper_bound(addr);
      if (iter != begin()) {
	auto liter = iter;
	liter--;
	EXPECT_FALSE(liter->first + liter->second.desc.len > addr);
      }
      if (iter != end()) {
	EXPECT_FALSE(iter->first < addr + len);
      }
    }
    void check_hint(laddr_t hint, laddr_t addr, extent_len_t len) {
      auto iter = lower_bound(hint);
      laddr_t last = hint;
      while (true) {
	if (iter == end() || iter->first > addr) {
	  EXPECT_EQ(addr, last);
	  break;
	}
	EXPECT_FALSE(iter->first - last > len);
	last = iter->first + iter->second.desc.len;
	++iter;
      }
    }
  public:
    void insert(TestBlock &extent) {
      check_available(extent.get_laddr(), extent.get_length());
      emplace(
	std::make_pair(
	  extent.get_laddr(),
	  test_extent_record_t{extent.get_desc(), 1}
	));
    }
    void alloced(laddr_t hint, TestBlock &extent) {
      check_hint(hint, extent.get_laddr(), extent.get_length());
      insert(extent);
    }
  } test_mappings;

  struct test_transaction_t {
    TransactionRef t;
    test_extents_t mappings;
  };

  test_transaction_t create_transaction() {
    return { tm->create_transaction(), test_mappings };
  }

  TestBlockRef alloc_extent(
    test_transaction_t &t,
    laddr_t hint,
    extent_len_t len,
    char contents) {
    auto extent = tm->alloc_extent<TestBlock>(
      *(t.t),
      hint,
      len).unsafe_get0();
    extent->set_contents(contents);
    EXPECT_FALSE(t.mappings.count(extent->get_laddr()));
    EXPECT_EQ(len, extent->get_length());
    t.mappings.alloced(hint, *extent);
    return extent;
  }

  TestBlockRef alloc_extent(
    test_transaction_t &t,
    laddr_t hint,
    extent_len_t len) {
    return alloc_extent(
      t,
      hint,
      len,
      get_random_contents());
  }

  void replay() {
    tm->close().unsafe_get();
    auto next = segment_cleaner->get_next();
    destroy();
    init();
    segment_cleaner->set_next(next);
    tm->mount().unsafe_get();
  }

  void check_mappings() {
    auto t = create_transaction();
    check_mappings(t);
  }

  TestBlockRef get_extent(
    test_transaction_t &t,
    laddr_t addr,
    extent_len_t len) {
    ceph_assert(t.mappings.count(addr));
    ceph_assert(t.mappings[addr].desc.len == len);

    auto ret_list = tm->read_extents<TestBlock>(
      *t.t, addr, len
    ).unsafe_get0();
    EXPECT_EQ(ret_list.size(), 1);
    auto &ext = ret_list.begin()->second;
    auto &laddr = ret_list.begin()->first;
    EXPECT_EQ(addr, laddr);
    EXPECT_EQ(addr, ext->get_laddr());
    return ext;
  }

  test_block_mutator_t mutator;
  TestBlockRef mutate_extent(
    test_transaction_t &t,
    TestBlockRef ref) {
    ceph_assert(t.mappings.count(ref->get_laddr()));
    ceph_assert(t.mappings[ref->get_laddr()].desc.len == ref->get_length());
    auto ext = tm->get_mutable_extent(*t.t, ref)->cast<TestBlock>();
    EXPECT_EQ(ext->get_laddr(), ref->get_laddr());
    EXPECT_EQ(ext->get_desc(), ref->get_desc());
    mutator.mutate(*ext, gen);
    t.mappings[ext->get_laddr()].update(ext->get_desc());
    return ext;
  }

  void inc_ref(test_transaction_t &t, laddr_t offset) {
    ceph_assert(t.mappings.count(offset));
    ceph_assert(t.mappings[offset].refcount > 0);
    auto refcnt = tm->inc_ref(*t.t, offset).unsafe_get0();
    t.mappings[offset].refcount++;
    EXPECT_EQ(refcnt, t.mappings[offset].refcount);
  }

  void dec_ref(test_transaction_t &t, laddr_t offset) {
    ceph_assert(t.mappings.count(offset));
    ceph_assert(t.mappings[offset].refcount > 0);
    auto refcnt = tm->dec_ref(*t.t, offset).unsafe_get0();
    t.mappings[offset].refcount--;
    EXPECT_EQ(refcnt, t.mappings[offset].refcount);
    if (t.mappings[offset].refcount == 0) {
      t.mappings.erase(offset);
    }
  }

  void check_mappings(test_transaction_t &t) {
    for (auto &i: t.mappings) {
      logger().debug("check_mappings: {}->{}", i.first, i.second);
      auto ext = get_extent(t, i.first, i.second.desc.len);
      EXPECT_EQ(i.second, ext->get_desc());
    }
  }

  void submit_transaction(test_transaction_t t) {
    tm->submit_transaction(std::move(t.t)).unsafe_get();
    test_mappings = t.mappings;
  }
};

TEST_F(transaction_manager_test_t, basic)
{
  constexpr laddr_t SIZE = 4096;
  run_async([this] {
    constexpr laddr_t ADDR = 0xFF * SIZE;
    {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	ADDR,
	SIZE,
	'a');
      ASSERT_EQ(ADDR, extent->get_laddr());
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
  });
}

TEST_F(transaction_manager_test_t, mutate)
{
  constexpr laddr_t SIZE = 4096;
  run_async([this] {
    constexpr laddr_t ADDR = 0xFF * SIZE;
    {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	ADDR,
	SIZE,
	'a');
      ASSERT_EQ(ADDR, extent->get_laddr());
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
    replay();
    {
      auto t = create_transaction();
      auto ext = get_extent(
	t,
	ADDR,
	SIZE);
      auto mut = mutate_extent(t, ext);
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
    replay();
    check_mappings();
  });
}

TEST_F(transaction_manager_test_t, create_remove_same_transaction)
{
  constexpr laddr_t SIZE = 4096;
  run_async([this] {
    constexpr laddr_t ADDR = 0xFF * SIZE;
    {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	ADDR,
	SIZE,
	'a');
      ASSERT_EQ(ADDR, extent->get_laddr());
      check_mappings(t);
      dec_ref(t, ADDR);
      check_mappings(t);

      extent = alloc_extent(
	t,
	ADDR,
	SIZE,
	'a');

      submit_transaction(std::move(t));
      check_mappings();
    }
    replay();
    check_mappings();
  });
}


TEST_F(transaction_manager_test_t, inc_dec_ref)
{
  constexpr laddr_t SIZE = 4096;
  run_async([this] {
    constexpr laddr_t ADDR = 0xFF * SIZE;
    {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	ADDR,
	SIZE,
	'a');
      ASSERT_EQ(ADDR, extent->get_laddr());
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
    replay();
    {
      auto t = create_transaction();
      inc_ref(t, ADDR);
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
    {
      auto t = create_transaction();
      dec_ref(t, ADDR);
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
    replay();
    {
      auto t = create_transaction();
      dec_ref(t, ADDR);
      check_mappings(t);
      check_mappings();
      submit_transaction(std::move(t));
      check_mappings();
    }
  });
}

TEST_F(transaction_manager_test_t, cause_lba_split)
{
  constexpr laddr_t SIZE = 4096;
  run_async([this] {
    for (unsigned i = 0; i < 200; ++i) {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	i * SIZE,
	SIZE,
	(char)(i & 0xFF));
      ASSERT_EQ(i * SIZE, extent->get_laddr());
      submit_transaction(std::move(t));
    }
    check_mappings();
  });
}

TEST_F(transaction_manager_test_t, random_writes)
{
  constexpr size_t TOTAL = 4<<20;
  constexpr size_t BSIZE = 4<<10;
  constexpr size_t PADDING_SIZE = 256<<10;
  constexpr size_t BLOCKS = TOTAL / BSIZE;
  run_async([this] {
    for (unsigned i = 0; i < BLOCKS; ++i) {
      auto t = create_transaction();
      auto extent = alloc_extent(
	t,
	i * BSIZE,
	BSIZE);
      ASSERT_EQ(i * BSIZE, extent->get_laddr());
      submit_transaction(std::move(t));
    }

    for (unsigned i = 0; i < 5; ++i) {
      for (unsigned j = 0; j < 50; ++j) {
	auto t = create_transaction();
	for (unsigned k = 0; k < 2; ++k) {
	  auto ext = get_extent(
	    t,
	    get_random_laddr(BSIZE, TOTAL),
	    BSIZE);
	  auto mut = mutate_extent(t, ext);
	  // pad out transaction
	  auto padding = alloc_extent(
	    t,
	    TOTAL + (k * PADDING_SIZE),
	    PADDING_SIZE);
	  dec_ref(t, padding->get_laddr());
	}
	submit_transaction(std::move(t));
      }
      replay();
      logger().debug("random_writes: checking");
      check_mappings();
      logger().debug("random_writes: done replaying/checking");
    }
  });
}
