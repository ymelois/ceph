// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_LIBRBD_API_NAMESPACE_H
#define CEPH_LIBRBD_API_NAMESPACE_H

#include "include/rados/librados_fwd.hpp"
#include "include/rbd/librbd.hpp"
#include <string>
#include <vector>

namespace cls { namespace rbd { struct NamespaceInfo; } }

namespace librbd {

struct ImageCtx;

namespace api {

template <typename ImageCtxT = librbd::ImageCtx>
struct Namespace {

  static int create(librados::IoCtx& io_ctx, const std::string& name);
  static int remove(librados::IoCtx& io_ctx, const std::string& name);
  static int list(librados::IoCtx& io_ctx, std::vector<std::string>* names);
  static int exists(librados::IoCtx& io_ctx, const std::string& name, bool *exists);
  static int set_quota(librados::IoCtx& io_ctx, const std::string& name,
                       bool set_max_bytes, uint64_t max_bytes,
                       bool set_max_objects, uint64_t max_objects);
  static int get_quota(librados::IoCtx& io_ctx, const std::string& name,
                       cls::rbd::NamespaceInfo *info);

};

} // namespace api
} // namespace librbd

extern template class librbd::api::Namespace<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_API_NAMESPACE_H
