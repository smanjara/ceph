// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_MOSDPGINFO_H
#define CEPH_MOSDPGINFO_H

#include "msg/Message.h"
#include "osd/osd_types.h"

class MOSDPGInfo : public Message {
private:
  static constexpr int HEAD_VERSION = 5;
  static constexpr int COMPAT_VERSION = 5;

  epoch_t epoch = 0;

public:
  using pg_list_t = std::vector<std::pair<pg_notify_t,PastIntervals>>;
  pg_list_t pg_list;

  epoch_t get_epoch() const { return epoch; }

  MOSDPGInfo()
    : MOSDPGInfo{0, {}}
  {}
  MOSDPGInfo(epoch_t mv)
    : MOSDPGInfo(mv, {})
  {}
  MOSDPGInfo(epoch_t mv, pg_list_t&& l)
    : Message{MSG_OSD_PG_INFO, HEAD_VERSION, COMPAT_VERSION},
      epoch{mv},
      pg_list{std::move(l)}
  {
    set_priority(CEPH_MSG_PRIO_HIGH);
  }
private:
  ~MOSDPGInfo() override {}

public:
  std::string_view get_type_name() const override { return "pg_info"; }
  void print(ostream& out) const override {
    out << "pg_info(";
    for (auto i = pg_list.begin();
         i != pg_list.end();
         ++i) {
      if (i != pg_list.begin())
	out << " ";
      out << i->first << "=" << i->second;
    }
    out << " epoch " << epoch
	<< ")";
  }

  void encode_payload(uint64_t features) override {
    using ceph::encode;
    encode(epoch, payload);
    encode(pg_list, payload);
  }
  void decode_payload() override {
    auto p = payload.cbegin();
    decode(epoch, p);
    decode(pg_list, p);
  }
private:
  template<class T, typename... Args>
  friend boost::intrusive_ptr<T> ceph::make_message(Args&&... args);
};

#endif
