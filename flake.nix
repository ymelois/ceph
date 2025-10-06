{
  description = "ceph pacific (16.2.x) development environment";

  inputs = {
    # GCC 13.4, Python 3.13, Boost 1.87 (matches ceph-par0 VMs)
    nixpkgs.url = "github:nixos/nixpkgs?rev=d04d8548aed39902419f14a8537006426dc1e4fa";
    # Python 3.9 for dashboard tests (CherryPy 13.1 is incompatible with 3.12)
    nixpkgs-py39.url = "github:nixos/nixpkgs?rev=7cf5ccf1cdb2ba5f08f0ac29fc3d04b0b59a07e4";
  };

  outputs =
    {
      nixpkgs,
      nixpkgs-py39,
      ...
    }:
    let
      inherit (nixpkgs) lib;

      supportedSystems = [
        "aarch64-linux"
        "x86_64-linux"
      ];

      forAllSystems =
        systems: f:
        lib.genAttrs systems (
          system:
          f {
            pkgs = import nixpkgs { inherit system; };
            pkgs-py39 = import nixpkgs-py39 { inherit system; };
          }
        );
    in
    {
      devShells = forAllSystems supportedSystems (
        { pkgs, pkgs-py39 }:
        let
          python = pkgs.python313;
          stdenv = pkgs.gcc13Stdenv;

          boost = pkgs.boost187.override {
            enablePython = true;
            inherit python stdenv;
          };

          pythonEnv = python.withPackages (
            ps: with ps; [
              # build
              cython
              setuptools
              sphinx
            ]
          );

          pythonEnvTest = pkgs-py39.python39.withPackages (
            ps: with ps; [
              tox
            ]
          );

        in
        {
          default = pkgs.mkShell.override { inherit stdenv; } {
            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
              git
              which
              nasm
              gperf
              pythonEnv
              ccache
              cunit
              doxygen
              graphviz
            ];

            buildInputs = with pkgs; [
              # core required
              boost
              openssl
              curl
              zlib
              snappy
              lz4
              bzip2
              leveldb

              # linux required
              systemd
              libuuid
              util-linux
              udev
              keyutils
              libcap
              libcap_ng
              libnl
              libaio
              liburing

              # storage
              xfsprogs
              cryptsetup

              # encoding, parsing, auth
              expat
              icu
              oath-toolkit
              libedit

              # messaging
              rabbitmq-c
              rdkafka

              # misc
              lua5_3
              fuse
              jemalloc
              libxml2
              sqlite

              # testing
              gtest
              linuxHeaders
              jq
              xmlstarlet
            ];

            BOOST_ROOT = "${boost}";

            shellHook = ''
              echo "Ceph Pacific 16.2.x dev shell (GCC 13, Python 3.13, Boost 1.87)"
              echo ""
              echo "Build:"
              echo "  ./do_cmake.sh \\"
              echo "    -DCMAKE_BUILD_TYPE=Debug \\"
              echo "    -DWITH_SYSTEM_BOOST=ON \\"
              echo "    -DWITH_SYSTEM_GTEST=ON \\"
              echo "    -DWITH_CCACHE=ON \\"
              echo "    -DWITH_PYTHON3=3.13 \\"
              echo "    -DWITH_XFS=ON \\"
              echo "    -DALLOCATOR=jemalloc \\"
              echo "    -DWITH_OPENLDAP=OFF \\"
              echo "    -DWITH_RDMA=OFF \\"
              echo "    -DWITH_LTTNG=OFF \\"
              echo "    -DWITH_BABELTRACE=OFF \\"
              echo "    -DHAVE_BABELTRACE=OFF \\"
              echo "    -DWITH_SELINUX=OFF \\"
              echo "    -DWITH_RADOSGW_FCGI_FRONTEND=OFF \\"
              echo "    -DWITH_MGR_DASHBOARD_FRONTEND=OFF \\"
              echo "    -DWITH_TESTS=OFF"
              echo ""
              echo "Dashboard tests: nix develop .#dashboard-test"
            '';
          };

          dashboard-test = pkgs.mkShell {
            nativeBuildInputs = [ pythonEnvTest ];

            shellHook = ''
              echo "Dashboard test shell (Python 3.9, tox)"
              echo ""
              echo "From src/pybind/mgr/dashboard:"
              echo "  tox -e py3 -- tests/test_rbd_namespace.py"
            '';
          };
        }
      );
    };
}
