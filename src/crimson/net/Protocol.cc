// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "Protocol.h"

#include "auth/Auth.h"

#include "crimson/common/formatter.h"
#include "crimson/common/log.h"
#include "crimson/net/Errors.h"
#include "crimson/net/chained_dispatchers.h"
#include "crimson/net/SocketConnection.h"
#include "msg/Message.h"

using namespace ceph::msgr::v2;
using crimson::common::local_conf;

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_ms);
  }
}

[[noreturn]] void abort_in_fault() {
  throw std::system_error(make_error_code(crimson::net::error::negotiation_failure));
}

[[noreturn]] void abort_protocol() {
  throw std::system_error(make_error_code(crimson::net::error::protocol_aborted));
}

std::size_t get_msg_size(const FrameAssembler &rx_frame_asm)
{
  ceph_assert(rx_frame_asm.get_num_segments() > 0);
  size_t sum = 0;
  // we don't include SegmentIndex::Msg::HEADER.
  for (size_t idx = 1; idx < rx_frame_asm.get_num_segments(); idx++) {
    sum += rx_frame_asm.get_segment_logical_len(idx);
  }
  return sum;
}

} // namespace anonymous

namespace crimson::net {

Protocol::Protocol(ChainedDispatchers& dispatchers,
                   SocketConnection& conn)
  : dispatchers(dispatchers),
    conn(conn),
    frame_assembler(conn)
{}

Protocol::~Protocol()
{
  ceph_assert(gate.is_closed());
  assert(!out_exit_dispatching);
}

ceph::bufferlist Protocol::sweep_out_pending_msgs_to_sent(
      size_t num_msgs,
      bool require_keepalive,
      std::optional<utime_t> maybe_keepalive_ack,
      bool require_ack)
{
  ceph::bufferlist bl = do_sweep_messages(out_pending_msgs,
                                          num_msgs,
                                          require_keepalive,
                                          maybe_keepalive_ack,
                                          require_ack);
  if (!conn.policy.lossy) {
    out_sent_msgs.insert(
        out_sent_msgs.end(),
        std::make_move_iterator(out_pending_msgs.begin()),
        std::make_move_iterator(out_pending_msgs.end()));
  }
  out_pending_msgs.clear();
  return bl;
}

seastar::future<> Protocol::send(MessageURef msg)
{
  if (out_state != out_state_t::drop) {
    out_pending_msgs.push_back(std::move(msg));
    notify_out_dispatch();
  }
  return seastar::now();
}

seastar::future<> Protocol::send_keepalive()
{
  if (!need_keepalive) {
    need_keepalive = true;
    notify_out_dispatch();
  }
  return seastar::now();
}

void Protocol::set_out_state(
    const Protocol::out_state_t &new_state)
{
  ceph_assert_always(!(
    (new_state == out_state_t::none && out_state != out_state_t::none) ||
    (new_state == out_state_t::open && out_state == out_state_t::open) ||
    (new_state != out_state_t::drop && out_state == out_state_t::drop)
  ));

  bool dispatch_in = false;
  if (out_state != out_state_t::open &&
      new_state == out_state_t::open) {
    // to open
    ceph_assert_always(frame_assembler.is_socket_valid());
    dispatch_in = true;
#ifdef UNIT_TESTS_BUILT
    if (conn.interceptor) {
      conn.interceptor->register_conn_ready(conn);
    }
#endif
  } else if (out_state == out_state_t::open &&
             new_state != out_state_t::open) {
    // from open
    ceph_assert_always(frame_assembler.is_socket_valid());
    frame_assembler.shutdown_socket();
    if (out_dispatching) {
      ceph_assert_always(!out_exit_dispatching.has_value());
      out_exit_dispatching = seastar::shared_promise<>();
    }
  }

  if (out_state != new_state) {
    out_state = new_state;
    out_state_changed.set_value();
    out_state_changed = seastar::shared_promise<>();
  }

  // The above needs to be atomic
  if (dispatch_in) {
    do_in_dispatch();
  }
}

seastar::future<> Protocol::wait_io_exit_dispatching()
{
  ceph_assert_always(out_state != out_state_t::open);
  ceph_assert_always(!frame_assembler.is_socket_valid());
  return seastar::when_all(
    [this] {
      if (out_exit_dispatching) {
        return out_exit_dispatching->get_shared_future();
      } else {
        return seastar::now();
      }
    }(),
    [this] {
      if (in_exit_dispatching) {
        return in_exit_dispatching->get_shared_future();
      } else {
        return seastar::now();
      }
    }()
  ).discard_result();
}

void Protocol::requeue_out_sent()
{
  assert(out_state != out_state_t::open);
  if (out_sent_msgs.empty()) {
    return;
  }

  out_seq -= out_sent_msgs.size();
  logger().debug("{} requeue {} items, revert out_seq to {}",
                 conn, out_sent_msgs.size(), out_seq);
  for (MessageURef& msg : out_sent_msgs) {
    msg->clear_payload();
    msg->set_seq(0);
  }
  out_pending_msgs.insert(
      out_pending_msgs.begin(),
      std::make_move_iterator(out_sent_msgs.begin()),
      std::make_move_iterator(out_sent_msgs.end()));
  out_sent_msgs.clear();
  notify_out_dispatch();
}

void Protocol::requeue_out_sent_up_to(seq_num_t seq)
{
  assert(out_state != out_state_t::open);
  if (out_sent_msgs.empty() && out_pending_msgs.empty()) {
    logger().debug("{} nothing to requeue, reset out_seq from {} to seq {}",
                   conn, out_seq, seq);
    out_seq = seq;
    return;
  }
  logger().debug("{} discarding sent msgs by seq {} (sent_len={}, out_seq={})",
                 conn, seq, out_sent_msgs.size(), out_seq);
  while (!out_sent_msgs.empty()) {
    auto cur_seq = out_sent_msgs.front()->get_seq();
    if (cur_seq == 0 || cur_seq > seq) {
      break;
    } else {
      out_sent_msgs.pop_front();
    }
  }
  requeue_out_sent();
}

void Protocol::reset_out()
{
  assert(out_state != out_state_t::open);
  out_seq = 0;
  out_pending_msgs.clear();
  out_sent_msgs.clear();
  need_keepalive = false;
  next_keepalive_ack = std::nullopt;
  ack_left = 0;
}

void Protocol::ack_out_sent(seq_num_t seq)
{
  if (conn.policy.lossy) {  // lossy connections don't keep sent messages
    return;
  }
  while (!out_sent_msgs.empty() &&
         out_sent_msgs.front()->get_seq() <= seq) {
    logger().trace("{} got ack seq {} >= {}, pop {}",
                   conn, seq, out_sent_msgs.front()->get_seq(),
                   *out_sent_msgs.front());
    out_sent_msgs.pop_front();
  }
}

seastar::future<stop_t> Protocol::try_exit_out_dispatch() {
  assert(!is_out_queued());
  return frame_assembler.flush().then([this] {
    if (!is_out_queued()) {
      // still nothing pending to send after flush,
      // the dispatching can ONLY stop now
      ceph_assert(out_dispatching);
      out_dispatching = false;
      if (unlikely(out_exit_dispatching.has_value())) {
        out_exit_dispatching->set_value();
        out_exit_dispatching = std::nullopt;
        logger().info("{} do_out_dispatch: nothing queued at {},"
                      " set out_exit_dispatching",
                      conn, out_state);
      }
      return seastar::make_ready_future<stop_t>(stop_t::yes);
    } else {
      // something is pending to send during flushing
      return seastar::make_ready_future<stop_t>(stop_t::no);
    }
  });
}

seastar::future<> Protocol::do_out_dispatch()
{
  return seastar::repeat([this] {
    switch (out_state) {
     case out_state_t::open: {
      size_t num_msgs = out_pending_msgs.size();
      bool still_queued = is_out_queued();
      if (unlikely(!still_queued)) {
        return try_exit_out_dispatch();
      }
      auto to_ack = ack_left;
      assert(to_ack == 0 || in_seq > 0);
      // sweep all pending out with the concrete Protocol
      return frame_assembler.write(
        sweep_out_pending_msgs_to_sent(
          num_msgs, need_keepalive, next_keepalive_ack, to_ack > 0)
      ).then([this, prv_keepalive_ack=next_keepalive_ack, to_ack] {
        need_keepalive = false;
        if (next_keepalive_ack == prv_keepalive_ack) {
          next_keepalive_ack = std::nullopt;
        }
        assert(ack_left >= to_ack);
        ack_left -= to_ack;
        if (!is_out_queued()) {
          return try_exit_out_dispatch();
        } else {
          // messages were enqueued during socket write
          return seastar::make_ready_future<stop_t>(stop_t::no);
        }
      });
     }
     case out_state_t::delay:
      // delay out dispatching until open
      if (out_exit_dispatching) {
        out_exit_dispatching->set_value();
        out_exit_dispatching = std::nullopt;
        logger().info("{} do_out_dispatch: delay and set out_exit_dispatching ...", conn);
      } else {
        logger().info("{} do_out_dispatch: delay ...", conn);
      }
      return out_state_changed.get_shared_future(
      ).then([] { return stop_t::no; });
     case out_state_t::drop:
      ceph_assert(out_dispatching);
      out_dispatching = false;
      if (out_exit_dispatching) {
        out_exit_dispatching->set_value();
        out_exit_dispatching = std::nullopt;
        logger().info("{} do_out_dispatch: dropped and set out_exit_dispatching", conn);
      } else {
        logger().info("{} do_out_dispatch: dropped", conn);
      }
      return seastar::make_ready_future<stop_t>(stop_t::yes);
     default:
      ceph_assert(false);
    }
  }).handle_exception_type([this] (const std::system_error& e) {
    if (e.code() != std::errc::broken_pipe &&
        e.code() != std::errc::connection_reset &&
        e.code() != error::negotiation_failure) {
      logger().error("{} do_out_dispatch(): unexpected error at {} -- {}",
                     conn, out_state, e);
      ceph_abort();
    }
    ceph_assert_always(frame_assembler.has_socket());
    frame_assembler.shutdown_socket();
    if (out_state == out_state_t::open) {
      logger().info("{} do_out_dispatch(): fault at {}, going to delay -- {}",
                    conn, out_state, e);
      std::exception_ptr eptr;
      try {
        throw e;
      } catch(...) {
        eptr = std::current_exception();
      }
      set_out_state(out_state_t::delay);
      notify_out_fault("do_out_dispatch", eptr);
    } else {
      logger().info("{} do_out_dispatch(): fault at {} -- {}",
                    conn, out_state, e);
    }
    return do_out_dispatch();
  });
}

void Protocol::notify_out_dispatch()
{
  notify_out();
  if (out_dispatching) {
    // already dispatching
    return;
  }
  out_dispatching = true;
  switch (out_state) {
   case out_state_t::open:
     [[fallthrough]];
   case out_state_t::delay:
    assert(!gate.is_closed());
    gate.dispatch_in_background("do_out_dispatch", *this, [this] {
      return do_out_dispatch();
    });
    return;
   case out_state_t::drop:
    out_dispatching = false;
    return;
   default:
    ceph_assert(false);
  }
}

seastar::future<>
Protocol::read_message(utime_t throttle_stamp, std::size_t msg_size)
{
  return frame_assembler.read_frame_payload(
  ).then([this, throttle_stamp, msg_size](auto payload) {
    if (unlikely(out_state != out_state_t::open)) {
      logger().debug("{} triggered {} during read_message()",
                     conn, out_state);
      abort_protocol();
    }

    utime_t recv_stamp{seastar::lowres_system_clock::now()};

    // we need to get the size before std::moving segments data
    auto msg_frame = MessageFrame::Decode(*payload);
    // XXX: paranoid copy just to avoid oops
    ceph_msg_header2 current_header = msg_frame.header();

    logger().trace("{} got {} + {} + {} byte message,"
                   " envelope type={} src={} off={} seq={}",
                   conn, msg_frame.front_len(), msg_frame.middle_len(),
                   msg_frame.data_len(), current_header.type, conn.get_peer_name(),
                   current_header.data_off, current_header.seq);

    ceph_msg_header header{current_header.seq,
                           current_header.tid,
                           current_header.type,
                           current_header.priority,
                           current_header.version,
                           ceph_le32(msg_frame.front_len()),
                           ceph_le32(msg_frame.middle_len()),
                           ceph_le32(msg_frame.data_len()),
                           current_header.data_off,
                           conn.get_peer_name(),
                           current_header.compat_version,
                           current_header.reserved,
                           ceph_le32(0)};
    ceph_msg_footer footer{ceph_le32(0), ceph_le32(0),
                           ceph_le32(0), ceph_le64(0), current_header.flags};

    auto conn_ref = seastar::static_pointer_cast<SocketConnection>(
        conn.shared_from_this());
    Message *message = decode_message(nullptr, 0, header, footer,
        msg_frame.front(), msg_frame.middle(), msg_frame.data(), conn_ref);
    if (!message) {
      logger().warn("{} decode message failed", conn);
      abort_in_fault();
    }

    // store reservation size in message, so we don't get confused
    // by messages entering the dispatch queue through other paths.
    message->set_dispatch_throttle_size(msg_size);

    message->set_throttle_stamp(throttle_stamp);
    message->set_recv_stamp(recv_stamp);
    message->set_recv_complete_stamp(utime_t{seastar::lowres_system_clock::now()});

    // check received seq#.  if it is old, drop the message.
    // note that incoming messages may skip ahead.  this is convenient for the
    // client side queueing because messages can't be renumbered, but the (kernel)
    // client will occasionally pull a message out of the sent queue to send
    // elsewhere.  in that case it doesn't matter if we "got" it or not.
    uint64_t cur_seq = get_in_seq();
    if (message->get_seq() <= cur_seq) {
      logger().error("{} got old message {} <= {} {}, discarding",
                     conn, message->get_seq(), cur_seq, *message);
      if (HAVE_FEATURE(conn.features, RECONNECT_SEQ) &&
          local_conf()->ms_die_on_old_message) {
        ceph_assert(0 == "old msgs despite reconnect_seq feature");
      }
      return seastar::now();
    } else if (message->get_seq() > cur_seq + 1) {
      logger().error("{} missed message? skipped from seq {} to {}",
                     conn, cur_seq, message->get_seq());
      if (local_conf()->ms_die_on_skipped_message) {
        ceph_assert(0 == "skipped incoming seq");
      }
    }

    // note last received message.
    in_seq = message->get_seq();
    if (conn.policy.lossy) {
      logger().debug("{} <== #{} === {} ({})",
                     conn,
                     message->get_seq(),
                     *message,
                     message->get_type());
    } else {
      logger().debug("{} <== #{},{} === {} ({})",
                     conn,
                     message->get_seq(),
                     current_header.ack_seq,
                     *message,
                     message->get_type());
    }

    // notify ack
    if (!conn.policy.lossy) {
      ++ack_left;
      notify_out_dispatch();
    }

    ack_out_sent(current_header.ack_seq);

    // TODO: change MessageRef with seastar::shared_ptr
    auto msg_ref = MessageRef{message, false};
    assert(out_state == out_state_t::open);
    // throttle the reading process by the returned future
    return dispatchers.ms_dispatch(conn_ref, std::move(msg_ref));
  });
}

void Protocol::do_in_dispatch()
{
  ceph_assert_always(!in_exit_dispatching.has_value());
  in_exit_dispatching = seastar::shared_promise<>();
  gate.dispatch_in_background("do_in_dispatch", *this, [this] {
    return seastar::keep_doing([this] {
      return frame_assembler.read_main_preamble(
      ).then([this](auto ret) {
        switch (ret.tag) {
          case Tag::MESSAGE: {
            size_t msg_size = get_msg_size(*ret.rx_frame_asm);
            return seastar::futurize_invoke([this] {
              // throttle_message() logic
              if (!conn.policy.throttler_messages) {
                return seastar::now();
              }
              // TODO: message throttler
              ceph_assert(false);
              return seastar::now();
            }).then([this, msg_size] {
              // throttle_bytes() logic
              if (!conn.policy.throttler_bytes) {
                return seastar::now();
              }
              if (!msg_size) {
                return seastar::now();
              }
              logger().trace("{} wants {} bytes from policy throttler {}/{}",
                             conn, msg_size,
                             conn.policy.throttler_bytes->get_current(),
                             conn.policy.throttler_bytes->get_max());
              return conn.policy.throttler_bytes->get(msg_size);
            }).then([this, msg_size] {
              // TODO: throttle_dispatch_queue() logic
              utime_t throttle_stamp{seastar::lowres_system_clock::now()};
              return read_message(throttle_stamp, msg_size);
            });
          }
          case Tag::ACK:
            return frame_assembler.read_frame_payload(
            ).then([this](auto payload) {
              // handle_message_ack() logic
              auto ack = AckFrame::Decode(payload->back());
              logger().debug("{} GOT AckFrame: seq={}", conn, ack.seq());
              ack_out_sent(ack.seq());
            });
          case Tag::KEEPALIVE2:
            return frame_assembler.read_frame_payload(
            ).then([this](auto payload) {
              // handle_keepalive2() logic
              auto keepalive_frame = KeepAliveFrame::Decode(payload->back());
              logger().debug("{} GOT KeepAliveFrame: timestamp={}",
                             conn, keepalive_frame.timestamp());
              // notify keepalive ack
              next_keepalive_ack = keepalive_frame.timestamp();
              notify_out_dispatch();

              last_keepalive = seastar::lowres_system_clock::now();
            });
          case Tag::KEEPALIVE2_ACK:
            return frame_assembler.read_frame_payload(
            ).then([this](auto payload) {
              // handle_keepalive2_ack() logic
              auto keepalive_ack_frame = KeepAliveFrameAck::Decode(payload->back());
              auto _last_keepalive_ack =
                seastar::lowres_system_clock::time_point{keepalive_ack_frame.timestamp()};
              set_last_keepalive_ack(_last_keepalive_ack);
              logger().debug("{} GOT KeepAliveFrameAck: timestamp={}",
                             conn, _last_keepalive_ack);
            });
          default: {
            logger().warn("{} do_in_dispatch() received unexpected tag: {}",
                          conn, static_cast<uint32_t>(ret.tag));
            abort_in_fault();
          }
        }
      });
    }).handle_exception([this](std::exception_ptr eptr) {
      const char *e_what;
      try {
        std::rethrow_exception(eptr);
      } catch (std::exception &e) {
        e_what = e.what();
      }

      if (out_state == out_state_t::open) {
        logger().info("{} do_in_dispatch(): fault at {}, going to delay -- {}",
                      conn, out_state, e_what);
        set_out_state(out_state_t::delay);
        notify_out_fault("do_in_dispatch", eptr);
      } else {
        logger().info("{} do_in_dispatch(): fault at {} -- {}",
                      conn, out_state, e_what);
      }
    }).finally([this] {
      ceph_assert_always(in_exit_dispatching.has_value());
      in_exit_dispatching->set_value();
      in_exit_dispatching = std::nullopt;
    });
  });
}

} // namespace crimson::net
