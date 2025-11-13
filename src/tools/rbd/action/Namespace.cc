
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "common/errno.h"
#include "include/stringify.h"
#include "include/types.h"
#include "common/Formatter.h"
#include "common/TextTable.h"
#include "include/rbd/librbd.hpp"
#include <algorithm>
#include <iostream>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>

namespace rbd {
namespace action {
namespace ns {

namespace at = argument_types;
namespace po = boost::program_options;

void get_create_arguments(po::options_description *positional,
                          po::options_description *options) {
  at::add_pool_options(positional, options, true);
}

int execute_create(const po::variables_map &vm,
                   const std::vector<std::string> &ceph_global_init_args) {
  std::string pool_name;
  std::string namespace_name;
  size_t arg_index = 0;
  int r = utils::get_pool_and_namespace_names(vm, true, &pool_name,
                                              &namespace_name, &arg_index);
  if (r < 0) {
    return r;
  }

  if (namespace_name.empty()) {
    std::cerr << "rbd: namespace name was not specified" << std::endl;
    return -EINVAL;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, "", &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  r = rbd.namespace_create(io_ctx, namespace_name.c_str());
  if (r < 0) {
    std::cerr << "rbd: failed to created namespace: " << cpp_strerror(r)
              << std::endl;
    return r;
  }

  return 0;
}

void get_remove_arguments(po::options_description *positional,
                          po::options_description *options) {
  at::add_pool_options(positional, options, true);
}

int execute_remove(const po::variables_map &vm,
                   const std::vector<std::string> &ceph_global_init_args) {
  std::string pool_name;
  std::string namespace_name;
  size_t arg_index = 0;
  int r = utils::get_pool_and_namespace_names(vm, true, &pool_name,
                                              &namespace_name, &arg_index);
  if (r < 0) {
    return r;
  }

  if (namespace_name.empty()) {
    std::cerr << "rbd: namespace name was not specified" << std::endl;
    return -EINVAL;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, "", &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  r = rbd.namespace_remove(io_ctx, namespace_name.c_str());
  if (r == -EBUSY) {
    std::cerr << "rbd: namespace contains images which must be deleted first."
              << std::endl;
    return r;
  } else if (r == -ENOENT) {
    std::cerr << "rbd: namespace does not exist." << std::endl;
    return r;
  } else if (r < 0) {
    std::cerr << "rbd: failed to remove namespace: " << cpp_strerror(r)
              << std::endl;
    return r;
  }

  return 0;
}

void get_list_arguments(po::options_description *positional,
                        po::options_description *options) {
  at::add_pool_options(positional, options, false);
  at::add_format_options(options);
}

int execute_list(const po::variables_map &vm,
                 const std::vector<std::string> &ceph_global_init_args) {
  std::string pool_name;
  size_t arg_index = 0;
  int r = utils::get_pool_and_namespace_names(vm, true, &pool_name,
                                              nullptr, &arg_index);
  if (r < 0) {
    return r;
  }

  at::Format::Formatter formatter;
  r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, "", &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  std::vector<std::string> names;
  r = rbd.namespace_list(io_ctx, &names);
  if (r < 0 && r != -ENOENT) {
    std::cerr << "rbd: failed to list namespaces: " << cpp_strerror(r)
              << std::endl;
    return r;
  }

  std::sort(names.begin(), names.end());

  TextTable tbl;
  if (formatter) {
    formatter->open_array_section("namespaces");
  } else {
    tbl.define_column("NAME", TextTable::LEFT, TextTable::LEFT);
  }

  for (auto& name : names) {
    if (formatter) {
      formatter->open_object_section("namespace");
      formatter->dump_string("name", name);
      formatter->close_section();
    } else {
      tbl << name << TextTable::endrow;
    }
  }

  if (formatter) {
    formatter->close_section();
    formatter->flush(std::cout);
  } else if (!names.empty()) {
    std::cout << tbl;
  }

  return 0;
}

void get_quota_set_arguments(po::options_description *positional,
                             po::options_description *options) {
  at::add_pool_options(positional, options, true);
  options->add_options()
    ("max-bytes", po::value<std::string>(),
     "maximum total bytes allowed within the namespace")
    ("no-max-bytes", "remove the maximum bytes quota limit")
    ("max-objects", po::value<uint64_t>(),
     "maximum object count allowed within the namespace")
    ("no-max-objects", "remove the maximum object quota limit");
}

int execute_quota_set(const po::variables_map &vm,
                      const std::vector<std::string> &ceph_global_init_args) {
  std::string pool_name;
  std::string namespace_name;
  size_t arg_index = 0;
  int r = utils::get_pool_and_namespace_names(vm, true, &pool_name,
                                              &namespace_name, &arg_index);
  if (r < 0) {
    return r;
  }

  if (namespace_name.empty()) {
    std::cerr << "rbd: namespace name was not specified" << std::endl;
    return -EINVAL;
  }

  bool set_max_bytes = false;
  uint64_t max_bytes = 0;
  if (vm.count("max-bytes")) {
    try {
      max_bytes = boost::lexical_cast<uint64_t>(
        vm["max-bytes"].as<std::string>());
    } catch (const boost::bad_lexical_cast&) {
      std::cerr << "rbd: invalid value for --max-bytes" << std::endl;
      return -EINVAL;
    }
    set_max_bytes = true;
  } else if (vm.count("no-max-bytes")) {
    set_max_bytes = true;
    max_bytes = 0;
  }

  bool set_max_objects = false;
  uint64_t max_objects = 0;
  if (vm.count("max-objects")) {
    max_objects = vm["max-objects"].as<uint64_t>();
    set_max_objects = true;
  } else if (vm.count("no-max-objects")) {
    set_max_objects = true;
    max_objects = 0;
  }

  if (!set_max_bytes && !set_max_objects) {
    std::cerr << "rbd: no quota fields specified" << std::endl;
    return -EINVAL;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, "", &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  r = rbd.namespace_set_quota(io_ctx, namespace_name.c_str(),
                              set_max_bytes, max_bytes,
                              set_max_objects, max_objects);
  if (r < 0) {
    std::cerr << "rbd: failed to set namespace quota: "
              << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

void get_quota_show_arguments(po::options_description *positional,
                              po::options_description *options) {
  at::add_pool_options(positional, options, true);
  at::add_format_options(options);
}

int execute_quota_show(const po::variables_map &vm,
                       const std::vector<std::string> &ceph_global_init_args) {
  std::string pool_name;
  std::string namespace_name;
  size_t arg_index = 0;
  int r = utils::get_pool_and_namespace_names(vm, true, &pool_name,
                                              &namespace_name, &arg_index);
  if (r < 0) {
    return r;
  }

  if (namespace_name.empty()) {
    std::cerr << "rbd: namespace name was not specified" << std::endl;
    return -EINVAL;
  }

  argument_types::Format::Formatter formatter;
  r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, "", &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  uint64_t max_bytes = 0, max_objects = 0, used_bytes = 0, used_objects = 0;
  librbd::RBD rbd;
  r = rbd.namespace_get_quota(io_ctx, namespace_name.c_str(),
                              &max_bytes, &max_objects,
                              &used_bytes, &used_objects);
  if (r < 0) {
    std::cerr << "rbd: failed to retrieve namespace quota: "
              << cpp_strerror(r) << std::endl;
    return r;
  }

  if (formatter) {
    formatter->open_object_section("quota");
    formatter->dump_unsigned("max_bytes", max_bytes);
    formatter->dump_unsigned("max_objects", max_objects);
    formatter->dump_unsigned("used_bytes", used_bytes);
    formatter->dump_unsigned("used_objects", used_objects);
    formatter->close_section();
    formatter->flush(std::cout);
  } else {
    TextTable tbl;
    tbl.define_column("FIELD", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("VALUE", TextTable::LEFT, TextTable::LEFT);
    if (max_bytes == 0) {
      tbl << "max_bytes" << "unlimited" << TextTable::endrow;
    } else {
      tbl << "max_bytes" << byte_u_t(max_bytes) << TextTable::endrow;
    }
    if (max_objects == 0) {
      tbl << "max_objects" << "unlimited" << TextTable::endrow;
    } else {
      tbl << "max_objects" << max_objects << TextTable::endrow;
    }
    if (used_bytes == 0) {
      tbl << "used_bytes" << "0 B" << TextTable::endrow;
    } else {
      tbl << "used_bytes" << byte_u_t(used_bytes) << TextTable::endrow;
    }
    tbl << "used_objects" << used_objects << TextTable::endrow;
    std::cout << tbl;
  }

  return 0;
}

Shell::Action action_create(
  {"namespace", "create"}, {},
   "Create an RBD image namespace.", "",
  &get_create_arguments, &execute_create);

Shell::Action action_remove(
  {"namespace", "remove"}, {"namespace", "rm"},
   "Remove an RBD image namespace.", "",
  &get_remove_arguments, &execute_remove);

Shell::Action action_list(
  {"namespace", "list"}, {"namespace", "ls"}, "List RBD image namespaces.", "",
  &get_list_arguments, &execute_list);

Shell::Action action_quota_set(
  {"namespace", "quota", "set"}, {},
   "Set namespace quota limits.", "",
  &get_quota_set_arguments, &execute_quota_set);

Shell::Action action_quota_show(
  {"namespace", "quota", "show"}, {"namespace", "quota", "ls"},
   "Show namespace quota usage.", "",
  &get_quota_show_arguments, &execute_quota_show);

} // namespace ns
} // namespace action
} // namespace rbd
