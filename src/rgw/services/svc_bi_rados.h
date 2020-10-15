// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */


#pragma once

#include "rgw/rgw_datalog.h"
#include "rgw/rgw_service.h"
#include "rgw/rgw_tools.h"

#include "svc_bi.h"
#include "svc_rados.h"
#include "svc_tier_rados.h"

struct rgw_bucket_dir_header;

class RGWSI_BILog_RADOS;

#define RGW_NO_SHARD -1

#define RGW_SHARDS_PRIME_0 7877
#define RGW_SHARDS_PRIME_1 65521

/*
 * Defined Bucket Index Namespaces
 */
#define RGW_OBJ_NS_MULTIPART "multipart"
#define RGW_OBJ_NS_SHADOW    "shadow"

class RGWSI_BucketIndex_RADOS : public RGWSI_BucketIndex
{
  friend class RGWSI_BILog_RADOS_InIndex;

  int open_pool(const DoutPrefixProvider *dpp,
                const rgw_pool& pool,
                RGWSI_RADOS::Pool *index_pool,
                bool mostly_omap);

  int open_bucket_index_pool(const DoutPrefixProvider *dpp,
                            const RGWBucketInfo& bucket_info,
                            RGWSI_RADOS::Pool *index_pool);
  int open_bucket_index_base(const DoutPrefixProvider *dpp,
                             const RGWBucketInfo& bucket_info,
                             RGWSI_RADOS::Pool *index_pool,
                             std::string *bucket_oid_base);

  void get_bucket_index_object(const std::string& bucket_oid_base,
                               uint32_t num_shards,
                               int shard_id,
                               uint64_t gen_id,
                               std::string *bucket_obj);
  int get_bucket_index_object(const std::string& bucket_oid_base,
			      const std::string& obj_key,
                              uint32_t num_shards, rgw::BucketHashType hash_type,
                              uint64_t gen_id, std::string *bucket_obj, int *shard_id);


public:

  struct Svc {
    RGWSI_Zone *zone{nullptr};
    RGWSI_RADOS *rados{nullptr};
    RGWSI_BILog_RADOS *bilog{nullptr};
    RGWDataChangesLog *datalog_rados{nullptr};
  } svc;

  RGWSI_BucketIndex_RADOS(CephContext *cct);

  void init(RGWSI_Zone *zone_svc,
            RGWSI_RADOS *rados_svc,
            RGWSI_BILog_RADOS *bilog_svc,
            RGWDataChangesLog *datalog_rados_svc);

  static int shards_max() {
    return RGW_SHARDS_PRIME_1;
  }

  static int shard_id(const std::string& key, int max_shards) {
    return rgw_shard_id(key, max_shards);
  }

  static uint32_t bucket_shard_index(const std::string& key,
                                     int num_shards) {
    uint32_t sid = ceph_str_hash_linux(key.c_str(), key.size());
    uint32_t sid2 = sid ^ ((sid & 0xFF) << 24);
    return rgw_shards_mod(sid2, num_shards);
  }

  static uint32_t bucket_shard_index(const rgw_obj_key& obj_key,
				     int num_shards)
  {
    std::string sharding_key;
    if (obj_key.ns == RGW_OBJ_NS_MULTIPART) {
      RGWMPObj mp;
      mp.from_meta(obj_key.name);
      sharding_key = mp.get_key();
    } else {
      sharding_key = obj_key.name;
    }

    return bucket_shard_index(sharding_key, num_shards);
  }

  int init_index(const DoutPrefixProvider *dpp, RGWBucketInfo& bucket_info,const rgw::bucket_index_layout_generation& idx_layout) override;
  int clean_index(const DoutPrefixProvider *dpp, RGWBucketInfo& bucket_info, const rgw::bucket_index_layout_generation& idx_layout) override;

  /* RADOS specific */

  int read_stats(const DoutPrefixProvider *dpp,
                 const RGWBucketInfo& bucket_info,
                 RGWBucketEnt *stats,
                 optional_yield y) override;

  int get_reshard_status(const DoutPrefixProvider *dpp, const RGWBucketInfo& bucket_info,
                         std::list<cls_rgw_bucket_instance_entry> *status);

  int handle_overwrite(const DoutPrefixProvider *dpp, const RGWBucketInfo& info,
                       const RGWBucketInfo& orig_info) override;

  int open_bucket_index_shard(const DoutPrefixProvider *dpp,
                              const RGWBucketInfo& bucket_info,
                              const std::string& obj_key,
                              RGWSI_RADOS::Obj *bucket_obj,
                              int *shard_id);

  int open_bucket_index_shard(const DoutPrefixProvider *dpp,
                              const RGWBucketInfo& bucket_info,
                              int shard_id,
                              uint32_t num_shards,
                              uint64_t gen,
                              RGWSI_RADOS::Obj *bucket_obj);

  int open_bucket_index(const DoutPrefixProvider *dpp,
                        const RGWBucketInfo& bucket_info,
                        RGWSI_RADOS::Pool *index_pool,
                        std::string *bucket_oid);

  int open_bucket_index(const DoutPrefixProvider *dpp,
                        const RGWBucketInfo& bucket_info,
                        std::optional<int> shard_id,
                        const rgw::bucket_index_layout_generation& idx_layout,
                        RGWSI_RADOS::Pool *index_pool,
                        std::map<int, std::string> *bucket_objs,
                        std::map<int, std::string> *bucket_instance_ids);

  int cls_bucket_head(const RGWBucketInfo& bucket_info,
                      int shard_id,
                      vector<rgw_bucket_dir_header> *headers,
                      map<int, string> *bucket_instance_ids,
                      optional_yield y) override;
};


