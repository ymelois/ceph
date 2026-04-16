#!/usr/bin/env bash

GITLAB="https://gitlab.exherbo.org/exherbo/net/-/raw/master/packages/sys-cluster/ceph/files"

patches=(
    "hostname-fix.patch"
    "ceph-gcc-13-cstdint.patch"
    "rocksdb-gcc-13-cstdint.patch"
    "0001-error-has-no-member-named-to_string-boost-1.81.patch"
    "fix-cython-3.patch"
    "force-bundled-fmt.patch"
    "pacific-revert-rgw-missing-etag-multipart-upload-result.patch"
    "fix-ftbfs-on-gcc-14.patch"
    "rapidjson-fix-gcc-14.patch"
    "pybind-fix-gcc-14.patch"
    "ceph-16.2.15-fix-mgr-python-3.13.patch"
    "ceph-16.2.15-fix-mgr-bcrypt.patch"
    "ceph-16.2.15-fix-mgr-bcrypt-dashboard.patch"
    "ceph-16.2.15-fix-mgr-bcrypt-dashboard-2.patch"
    "ceph-16.2.15-fix-cve-2024-47866.patch"
    "ceph-16.2.15-fix-boost-1.86.patch"
    "ceph-16.2.15-fix-boost-1.87.patch"
)

git submodule deinit --force --all
git submodule update --init --recursive

for patch in "${patches[@]}"; do
    echo "Applying $patch..."
    jj desc --message "patch: $patch"
    curl "$GITLAB/$patch" | git apply
    jj new
done
