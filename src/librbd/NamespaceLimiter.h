// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_NAMESPACE_LIMITER_H
#define CEPH_LIBRBD_NAMESPACE_LIMITER_H

#include "include/rados/librados.hpp"

class CephContext;

namespace librbd {

class NamespaceLimiter {
public:
  NamespaceLimiter(CephContext *cct, librados::IoCtx& io_ctx);

  bool enabled() const {
    return m_enabled;
  }

  int reserve(uint64_t bytes, uint64_t objects);
  int release(uint64_t bytes, uint64_t objects);

private:
  CephContext *m_cct = nullptr;
  librados::IoCtx m_default_io_ctx;
  std::string m_namespace;
  bool m_enabled = false;
  bool m_supported = true;

  int update(int64_t bytes, int64_t objects, bool enforce);
};

} // namespace librbd

#endif // CEPH_LIBRBD_NAMESPACE_LIMITER_H
