// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/NamespaceLimiter.h"
#include "cls/rbd/cls_rbd_client.h"
#include "common/dout.h"
#include "common/errno.h"
#include <limits>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::NamespaceLimiter: " << __func__ << ": "

namespace librbd {

NamespaceLimiter::NamespaceLimiter(CephContext *cct, librados::IoCtx& io_ctx)
  : m_cct(cct), m_namespace(io_ctx.get_namespace()) {
  if (m_namespace.empty()) {
    return;
  }

  m_enabled = true;
  m_default_io_ctx.dup(io_ctx);
  m_default_io_ctx.set_namespace("");
}

int NamespaceLimiter::reserve(uint64_t bytes, uint64_t objects) {
  if (!m_enabled || (!bytes && !objects)) {
    return 0;
  }
  if (bytes > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      objects > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return -ERANGE;
  }
  return update(static_cast<int64_t>(bytes),
                static_cast<int64_t>(objects), true);
}

// Releases must always succeed to avoid leaking quota reservations, so
// values exceeding int64_t::max() are clamped rather than rejected (unlike
// reserve(), which returns -ERANGE in that case).
int NamespaceLimiter::release(uint64_t bytes, uint64_t objects) {
  if (!m_enabled || (!bytes && !objects)) {
    return 0;
  }

  const uint64_t max_val =
    static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  bytes = std::min(bytes, max_val);
  objects = std::min(objects, max_val);

  int64_t release_bytes = -static_cast<int64_t>(bytes);
  int64_t release_objects = -static_cast<int64_t>(objects);
  int r = update(release_bytes, release_objects, false);
  if (r < 0) {
    lderr(m_cct) << "namespace quota release failed: "
                 << cpp_strerror(r) << dendl;
  }
  return r;
}

int NamespaceLimiter::update(int64_t bytes, int64_t objects, bool enforce) {
  if (!m_enabled || !m_supported || (!bytes && !objects)) {
    return 0;
  }

  int r = cls_client::namespace_quota_update(
    &m_default_io_ctx, m_namespace, bytes, objects, enforce);
  if (r == -EOPNOTSUPP) {
    ldout(m_cct, 5) << "namespace quotas not supported" << dendl;
    m_supported = false;
    return 0;
  } else if (r == -ENOENT) {
    ldout(m_cct, 5) << "namespace " << m_namespace << " missing quota entry"
                    << dendl;
    m_enabled = false;
    return 0;
  } else if (r < 0) {
    if (r != -EDQUOT) {
      lderr(m_cct) << "failed to update namespace quota: "
                   << cpp_strerror(r) << dendl;
    }
    return r;
  }
  return 0;
}

} // namespace librbd
