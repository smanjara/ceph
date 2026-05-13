// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

// Integration test for the data race between aio_dispatch's slot.clear() and
// ReadHandler's shard.signal.emit() in rgwrados::shard_io::async_reads().
//
// To exercise the race path this test:
//   1. issues async_reads() with max_concurrent > 1 against real RADOS objects.
//   2. uses ForcedErrorReader which returns Error for shard 0.
//   3. repeats many times to increase the probability of hitting the window.
//      use TSAN to reliably detects the race.

#include "driver/rados/shard_io.h"

#include <optional>
#include <string>
#include <thread>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <gtest/gtest.h>

#include "common/async/blocked_completion.h"
#include "common/ceph_argparse.h"
#include "common/debug.h"
#include "global/global_init.h"
#include "global/global_context.h"
#include "include/rados/librados.hpp"

#define dout_subsys ceph_subsys_rgw
#define dout_context g_ceph_context

using boost::system::error_code;

static constexpr auto poolname = "ceph_test_rgw_shard_io_rados";

class ShardIoRados : public ::testing::Test {
 protected:
  static librados::Rados rados;
  static librados::IoCtx io;
  // the ioc thread runs ReadHandlers;
  static boost::asio::io_context ioc;
  static std::optional<boost::asio::executor_work_guard<
      boost::asio::io_context::executor_type>> ioc_work;
  static std::thread ioc_thread;

  static void SetUpTestCase() {
    ASSERT_EQ(0, rados.init_with_context(g_ceph_context));
    ASSERT_EQ(0, rados.connect());

    ioc_work.emplace(boost::asio::make_work_guard(ioc));
    ioc_thread = std::thread([] { ioc.run(); });

    int r = rados.ioctx_create(poolname, io);
    if (r == -ENOENT) {
      r = rados.pool_create(poolname);
      if (r == -EEXIST) {
        r = 0;
      } else if (r == 0) {
        r = rados.ioctx_create(poolname, io);
      }
    }
    ASSERT_EQ(0, r);

    bufferlist bl;
    bl.append(std::string(512 * 1024, 'x'));
    for (int i = 0; i < 5; ++i) {
      ASSERT_EQ(0, io.write_full("shard_io_" + std::to_string(i), bl));
    }
  }

  static void TearDownTestCase() {
    for (int i = 0; i < 5; ++i) {
      io.remove("shard_io_" + std::to_string(i));
    }
    rados.shutdown();
    ioc_work.reset();
    ioc_thread.join();
  }
};

librados::Rados ShardIoRados::rados;
librados::IoCtx ShardIoRados::io;
boost::asio::io_context ShardIoRados::ioc;
std::optional<boost::asio::executor_work_guard<
    boost::asio::io_context::executor_type>> ShardIoRados::ioc_work;
std::thread ShardIoRados::ioc_thread;

// the reader that forces shard 0 to return Error even when its AIO succeeds.
// this triggers shard.signal.emit() for all concurrent shards
struct ForcedErrorReader : rgwrados::shard_io::RadosReader {
  using RadosReader::RadosReader;

  void prepare_read(int, librados::ObjectReadOperation& op) override {
    op.read(0, 0, nullptr, nullptr);
  }

  rgwrados::shard_io::Result on_complete(int shard, error_code ec) override {
    if (ec || shard == 0) {
      return rgwrados::shard_io::Result::Error;
    }
    return rgwrados::shard_io::Result::Success;
  }

  void add_prefix(std::ostream& out) const override {
    out << "ForcedErrorReader: ";
  }
};

TEST_F(ShardIoRados, concurrent_reads_error_trigger_race)
{
  const auto dpp = NoDoutPrefix{dout_context, dout_subsys};
  auto ex = boost::asio::make_strand(ioc.get_executor());


  const std::map<int, std::string> objects = {
    {0, "shard_io_0"},
    {1, "shard_io_1"},
    {2, "shard_io_2"},
    {3, "shard_io_3"},
    {4, "shard_io_4"},
  };

  constexpr int kIterations = 500;

  for (int i = 0; i < kIterations; ++i) {
    auto reader = ForcedErrorReader{dpp, ex, io};
    error_code ec;
    rgwrados::shard_io::async_reads(reader, objects, 5,
                                    ceph::async::use_blocked[ec]);
    // expects an error
    EXPECT_TRUE(ec) << "iteration " << i << ": expected forced error";
  }
}

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  env_to_vec(args);

  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                         CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(cct.get());

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
