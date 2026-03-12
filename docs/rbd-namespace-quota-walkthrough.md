# RBD Namespace Quota: Code Walkthrough

This document walks through each commit in the `feat/rbd-namespace-quota` branch, explaining what was changed and why, with all technical jargon defined inline.

---

## Commit 1: `cls/rbd: add namespace quota CLS methods and types`

This commit adds the server-side foundation. Everything here runs inside the OSD process (the daemon that manages a physical disk) as part of `cls_rbd`, a plugin loaded by the OSD that handles RBD metadata operations atomically.

### New data type: `NamespaceInfo`

Defined in `cls_rbd_types.h`, a struct with four `uint64_t` fields:

- `max_bytes`: the configured byte limit for this namespace (0 means unlimited)
- `max_objects`: the configured object count limit (0 means unlimited)
- `used_bytes`: how many bytes are currently accounted for
- `used_objects`: how many RADOS objects are currently accounted for

The struct is serialized using DENC, Ceph's encoding framework. `ENCODE_START(1, 1, bl)` means this is version 1 of the encoding, and the minimum compatible reader version is also 1. If a future version adds fields, it would use `ENCODE_START(2, 1, bl)`, meaning "I am version 2, but code that only understands version 1 can still read the parts it knows about." The data is written into a `bufferlist`, Ceph's fundamental byte buffer type (a chain of memory segments used for all I/O and serialization).

The struct also includes `operator==` (for tests), `dump()` (for structured output), and `generate_test_instances()` (required by Ceph's test infrastructure for any DENC-encoded type).

### Where the data lives on disk

Every RBD pool has a RADOS object called `rbd_namespace` in the pool's default namespace (the empty-string namespace). A RADOS object is a blob of data stored on an OSD, identified by a name within a pool+namespace combination. Each RADOS object can have binary content, extended attributes, and an **omap**: a key-value store attached to the object, where keys and values are arbitrary byte strings.

The `rbd_namespace` object's omap already has entries with the key pattern `name_<namespace_name>` for each RBD namespace that exists. Before this feature, those omap values were **empty bufferlists**, serving only as existence markers. Now, the values contain DENC-encoded `NamespaceInfo` structs.

The backward-compatibility bridge is the new helper function `namespace_read_info()`: it reads the omap value for a given key, and if the bufferlist length is 0 (a namespace created before quota support), it returns a default `NamespaceInfo` with all four fields set to zero. This means pre-quota namespaces are treated as having no limits and no tracked usage.

The existing `namespace_add` cls method (called when `rbd namespace create` runs) is also changed: instead of writing an empty bufferlist as the omap value, it now writes a default-constructed `NamespaceInfo` (all zeros). The result is functionally equivalent (no limits, no usage), but the value now has structure from the start.

### Three new OSD class methods

A cls method is a function that runs on the OSD, inside the OSD process. The client sends an "exec" operation specifying the class name (`rbd`), method name, and serialized input. The OSD calls the method, which can read/write the object's data and omap, and returns serialized output. Because the method runs on the OSD that owns the object, it can perform atomic read-modify-write operations without race conditions.

Each method is registered with permission flags: `CLS_METHOD_RD` (reads data) and `CLS_METHOD_WR` (writes data). These flags affect how the OSD handles locking and replication.

#### `namespace_quota_get` (RD only)

Decodes a namespace name from the input bufferlist. Looks up `name_<ns>` in the omap using `cls_cxx_map_get_val` (a helper available inside cls methods that reads a single omap key-value pair). Decodes the `NamespaceInfo` from the value and returns it in the output bufferlist.

Returns `-ENOENT` (no such entry) if the namespace does not exist, `-EINVAL` (invalid argument) if the name is empty.

#### `namespace_quota_set` (RD | WR)

Sets the `max_bytes` and/or `max_objects` limits on a namespace. The input contains:
- the namespace name
- a boolean `set_max_bytes` and, if true, the new `max_bytes` value
- a boolean `set_max_objects` and, if true, the new `max_objects` value

The method performs a read-modify-write: it reads the current `NamespaceInfo` from the omap, updates only the flagged fields, then writes it back using `cls_cxx_map_set_val`. This preserves the usage counters and any unmodified limit. For example, you can change `max_bytes` without touching `max_objects`.

Returns `-EINVAL` if the name is empty or neither field is flagged.

#### `namespace_quota_update` (RD | WR)

Atomically adjusts `used_bytes` and `used_objects` by signed deltas. The input contains:
- the namespace name
- `delta_bytes` (int64_t, can be positive or negative)
- `delta_objects` (int64_t, can be positive or negative)
- `enforce` (boolean)

The `apply_delta` lambda handles three cases:
- **Zero delta**: no change, returns success
- **Negative delta** (releasing resources): checks that the magnitude does not exceed the current value. If it would, returns `-ERANGE` (result out of range) to prevent underflow below zero
- **Positive delta**: checks for `uint64_t` overflow (`-EOVERFLOW`). Then, only if `enforce` is true and the configured limit is nonzero, checks whether the new value would exceed the limit, returning `-EDQUOT` (disk quota exceeded) if so. When `enforce` is false, the increment is applied without checking limits, allowing usage to exceed the configured maximum. The release path always uses negative deltas with `enforce=false`; positive deltas with `enforce=false` are a separate case for bypassing limit checks on increments

After computing both new values, the method writes the updated `NamespaceInfo` back to the omap.

### Unit tests

The `TestClsRbd::namespace_quota` test (in `test_cls_rbd.cc`) uses Google Test (`TEST_F` defines a test case belonging to the `TestClsRbd` fixture, which sets up a RADOS connection and pool before each test). It covers:

- Getting quota on a freshly created namespace (all zeros)
- Setting one field only, verifying the other is unchanged
- Setting both fields
- Removing a limit (setting it to 0)
- Invalid inputs: empty name (`-EINVAL`), no fields flagged (`-EINVAL`)
- Nonexistent namespace (`-ENOENT`)
- Reserving within limits, exceeding bytes limit (`-EDQUOT`), exceeding objects limit (`-EDQUOT`)
- Releasing (negative delta with `enforce=false`)
- Skipping enforcement (`enforce=false` allows exceeding limits)
- Underflow protection (releasing more than used, `-ERANGE`)

---

## Commit 2: `librbd: enforce namespace quotas on create, resize, and remove`

This commit integrates quota enforcement into librbd, the C/C++ client library that implements RBD. librbd talks to RADOS to manage images: creating, removing, resizing, reading, writing. The operations are implemented as state machines, where each step issues an async RADOS operation and registers a callback for the next step, avoiding blocking threads while waiting for network I/O.

### `NamespaceLimiter` (new helper class)

A convenience wrapper around `namespace_quota_update` calls. Constructed with a `CephContext` (Ceph's global context object, holding configuration and logging) and an `IoCtx` (a handle representing a connection to a specific pool and namespace).

On construction:
- If the IoCtx's namespace is the empty string (the default namespace), the limiter disables itself. Quotas only apply to named namespaces, not to the pool-wide default namespace.
- Otherwise, it stores the namespace name, duplicates the IoCtx, and sets the duplicate to the default namespace (where the `rbd_namespace` object lives).

Two public methods:

**`reserve(bytes, objects)`**: calls `namespace_quota_update` with positive deltas and `enforce=true`. If the values exceed `INT64_MAX` (the maximum positive value for the signed `int64_t` that the cls method expects), returns `-ERANGE` rather than silently truncating.

**`release(bytes, objects)`**: calls `namespace_quota_update` with negative deltas and `enforce=false`. Unlike reserve, releases clamp values to `INT64_MAX` rather than returning an error, because a release must never fail and leak quota. If the RADOS call fails for some other reason, it logs the error but still returns the error code.

The private `update()` method handles graceful degradation:
- `-EOPNOTSUPP` (operation not supported): the OSD does not have the new cls methods. This happens during rolling upgrades or when running custom code against a stock OSD. The limiter disables itself permanently by setting `m_supported = false` and returns success.
- `-ENOENT`: the namespace has no omap entry. The limiter disables itself by setting `m_enabled = false` and returns success.
- `-EDQUOT`: quota exceeded. Returned as-is to the caller (not logged as an error, since it is an expected condition).

### `CreateRequest` changes

`CreateRequest` is the state machine that creates a new RBD image. It has a sequence of steps: validate pool, validate data pool, add image to directory, create ID object, negotiate features, create the image header, etc. Each step is a separate async method.

A new step is inserted between "validate data pool" and "add image to directory":

**`send_reserve_namespace_quota()`**: checks whether the IoCtx has a named namespace. If not (empty string), skips directly to adding the image to the directory. Otherwise, computes:
- `reserved_bytes = m_size` (the image's virtual size)
- `reserved_objects = Striper::get_num_objects(m_layout, m_size)`: the Striper is the component that maps byte offsets within an image to specific RADOS objects. This call computes how many RADOS objects are needed to store an image of the given size with the given stripe layout (determined by the object order, which is the log2 of the object size, typically 22, meaning 4 MiB objects).

Then it builds an `ObjectWriteOperation` (a client-side batch of operations submitted atomically), adds a `namespace_quota_update` exec call to it, and submits it asynchronously via `aio_operate`. An `AioCompletion` (async completion callback) is created using `create_rados_callback`, which wraps a typed member function pointer so it gets called when the RADOS operation finishes.

**`handle_reserve_namespace_quota(int r)`**: the callback. If `r` is `-EOPNOTSUPP` or `-ENOENT`, quotas are not available, so it proceeds. If `r` is negative (including `-EDQUOT`), it calls `complete(r)` to abort the entire create. Otherwise, it sets `m_namespace_quota_reserved = true` and proceeds to the next step.

**Rollback in `complete()`**: if the create fails at any later step (for example, the image name already exists, giving `-EEXIST`) and quota was already reserved, the completion handler synchronously releases the reserved bytes and objects by calling `namespace_quota_update` with negative deltas and `enforce=false`. This prevents quota from being permanently leaked by a failed create. The release is best-effort: if it fails (other than `-EOPNOTSUPP` / `-ENOENT`), it logs a warning but does not change the overall error code.

### `RemoveRequest` changes

`RemoveRequest` is the state machine that removes an RBD image. It opens the image, checks for snapshots and watchers, trims data, removes the header object, and removes the directory entry.

A `NamespaceLimiter` is constructed in the `RemoveRequest` constructor, receiving the `CephContext` and `IoCtx`.

**`record_namespace_usage()`**: called before `pre_remove_image` (which checks whether the image can be removed). Under the image lock (a shared reader lock, `std::shared_lock`, since it only needs to read the image's size), it snapshots:
- `m_namespace_usage_bytes = m_image_ctx->size` (the image's current virtual size)
- `m_namespace_usage_objects = Striper::get_num_objects(layout, size)` (how many RADOS objects the image uses)

The usage is recorded early because later steps close the image context, making the size unavailable.

**`finish(int r)`**: on success (`r == 0`), calls `m_namespace_limiter.release()` with the recorded bytes and objects. On failure, nothing is released because the image still exists and its resources are still in use.

### `ResizeRequest` changes

`ResizeRequest` is the state machine that changes an image's virtual size. It can grow (allocate more space) or shrink (trim data and free space). The grow path blocks writes, optionally grows the object map, updates the header, then unblocks writes. The shrink path blocks writes, trims data, optionally shrinks the object map, updates the header, then unblocks writes.

A new method `send_dispatch_resize()` is introduced. It is called after the journal append (if journaling is enabled) and decides whether the resize is a grow or shrink. For grows, it calls the new `send_reserve_ns_quota()`. For shrinks, it goes directly to the trim path (quota is released after the shrink completes).

**`send_reserve_ns_quota()`**: computes the deltas:
- `m_ns_delta_bytes = m_new_size - m_original_size`
- `m_ns_delta_objects`: the difference in object count between the new size and original size, computed via `Striper::get_num_objects` for each

If the namespace is empty (default namespace) or both deltas are zero, it skips to `send_grow_object_map()`. Otherwise, it dups the IoCtx to the default namespace and issues an async `namespace_quota_update` with `enforce=true`.

**`handle_reserve_ns_quota(int *result)`**: uses the pattern where a `Context*` return value of `nullptr` means "I started an async operation, wait for the next callback", while returning a Context means "execute this synchronously to finish." On `-EOPNOTSUPP` / `-ENOENT`, proceeds without quota. On error (including `-EDQUOT`), unblocks writes and returns a finisher context with the error code. On success, sets `m_ns_quota_reserved = true` and calls `send_grow_object_map()`.

**Rollback on failure**: two later steps, `handle_grow_object_map` and `handle_update_header`, check `m_ns_quota_reserved`. If either fails after quota was reserved, they call `send_release_ns_quota()` to release the delta before returning the error.

**`send_release_ns_quota(delta_bytes, delta_objects)`**: a synchronous best-effort release. It dups the IoCtx, sets it to the default namespace, and calls `cls_client::namespace_quota_update` with negative deltas and `enforce=false`. The comment notes that `IoCtx::operate()` is synchronous but dispatches to a separate worker thread internally, so it does not re-enter the completion thread. Failures other than `-EOPNOTSUPP` / `-ENOENT` are logged but not propagated.

**Shrink path release in `update_size_and_overlap()`**: after a successful shrink, this method computes `delta_bytes = original_size - new_size` and the delta in object count, then calls `send_release_ns_quota()` to give back the freed space. No enforcement check is needed because shrinking always reduces usage.

---

## Commit 3: `librbd: add namespace quota get/set API and Python bindings`

This commit exposes the feature to external consumers through the public librbd API.

### C++ API (`librbd.hpp`)

Two new methods on the `RBD` class (the main entry point for RBD operations in C++):

- `namespace_set_quota(io_ctx, name, set_max_bytes, max_bytes, set_max_objects, max_objects)`: sets one or both limits
- `namespace_get_quota(io_ctx, name, &max_bytes, &max_objects, &used_bytes, &used_objects)`: retrieves all four values through output pointers

### C API (`librbd.h`)

A bitmask defines which fields to update:
- `RBD_NAMESPACE_QUOTA_FIELD_MAX_BYTES` (bit 0)
- `RBD_NAMESPACE_QUOTA_FIELD_MAX_OBJECTS` (bit 1)

A struct `rbd_namespace_quota_info_t` holds the four `uint64_t` fields.

Functions:
- `rbd_namespace_set_quota(io, name, fields, max_bytes, max_objects)`: the `fields` bitmask specifies which limits to change
- `rbd_namespace_get_quota(io, name, &info)`: fills in the struct

The C API uses a bitmask instead of separate booleans because C does not have default arguments, and a bitmask is a common C idiom for "which fields are valid."

### Internal `Namespace` API layer (`librbd/api/Namespace.cc`)

`set_quota()` and `get_quota()` are static methods on `librbd::api::Namespace<>`. They duplicate the IoCtx (creating a copy of the pool+namespace handle), set the duplicate to the default namespace (because the `rbd_namespace` object lives there), and call the cls_client functions (`cls_client::namespace_quota_set` / `cls_client::namespace_quota_get`).

These are the internal entry points; both the C and C++ public APIs delegate here.

### `librbd.cc` glue

The C++ methods (`RBD::namespace_set_quota`, `RBD::namespace_get_quota`) directly call the `api::Namespace<>` methods.

The C functions (`rbd_namespace_set_quota`, `rbd_namespace_get_quota`) convert between C and C++ types:
- `from_rados_ioctx_t` converts a C `rados_ioctx_t` handle to a C++ `IoCtx`
- The bitmask is translated to booleans for `set_quota`
- The internal `NamespaceInfo` struct is copied field-by-field into the C `rbd_namespace_quota_info_t` for `get_quota`

### Python bindings

Ceph's Python RBD bindings use **Cython**, a language that lets you write C extensions for Python using Python-like syntax.

**`c_rbd.pxd`** (Cython declaration file, like a C header): declares the `rbd_namespace_quota_info_t` struct and the two C functions so that Cython knows their signatures.

**`rbd.pyx`** (Cython implementation file): adds two methods to the `RBD` Python class:

- `namespace_set_quota(ioctx, name, fields, max_bytes, max_objects)`: converts Python arguments to C types using `cdef` (Cython's keyword for declaring raw C-typed variables, not Python objects). The actual C call happens inside a `with nogil:` block, which releases the Python Global Interpreter Lock so other Python threads can run while the C function blocks on network I/O. If the return code is nonzero, raises a Python exception via `make_ex`.

- `namespace_get_quota(ioctx, name)`: calls the C function, then returns a Python dict with keys `max_bytes`, `max_objects`, `used_bytes`, `used_objects`.

Module-level constants `RBD_NAMESPACE_QUOTA_FIELD_MAX_BYTES` and `RBD_NAMESPACE_QUOTA_FIELD_MAX_OBJECTS` are also exported for use by Python callers.

---

## Commit 4: `rbd: add namespace quota set/show CLI commands`

This adds user-facing commands to the `rbd` command-line tool.

### `rbd namespace quota set`

Registered as a `Shell::Action` (the rbd CLI's command registration mechanism) with the command path `{"namespace", "quota", "set"}`, giving you the command `rbd namespace quota set <pool>/<namespace> [options]`.

Arguments are defined using Boost.Program_options (`po::options_description` defines what flags are accepted; `po::variables_map` holds parsed values):

- `--max-bytes <N>`: sets the byte limit. Parsed with `boost::lexical_cast<uint64_t>` (a Boost function that converts a string to a typed value, throwing `bad_lexical_cast` on failure).
- `--no-max-bytes`: removes the byte limit (sets it to 0, meaning unlimited)
- `--max-objects <N>`: sets the object count limit
- `--no-max-objects`: removes the object count limit

At least one of these must be specified, otherwise the command returns `-EINVAL`. The `--no-max-*` flags and `--max-*` flags are mutually exclusive for each field (handled by `else if`).

The execution function (`execute_quota_set`) parses the pool and namespace names using `utils::get_pool_and_namespace_names`, initializes a RADOS connection via `utils::init`, creates an `RBD` object, and calls `namespace_set_quota`.

### `rbd namespace quota show`

Registered with command path `{"namespace", "quota", "show"}` and alias `{"namespace", "quota", "ls"}`.

Supports `--format json` via the structured output system. A `Formatter` is Ceph's interface for producing structured output (JSON, XML, etc.). When `--format json` is specified:
- `open_object_section("quota")` starts a JSON object
- `dump_unsigned("max_bytes", max_bytes)` writes a key-value pair
- `close_section()` / `flush()` finishes and outputs the JSON

When no format is specified (plain text mode), the output uses `TextTable`, a Ceph utility for printing aligned columns. Byte values are formatted with `byte_u_t`, which converts raw byte counts to human-readable strings (e.g. 4194304 becomes "4 MiB"). Zero limits display as "unlimited".

---

## Commit 5: `test: add namespace quota integration tests`

### Shell integration test (`qa/workunits/cephtool/test.sh`)

`test_rbd_namespace_quota()` is a bash function added to the OSD test suite. It runs against a real Ceph cluster (not a mock). The function:

1. Creates a namespace with a UUID-based name (via `uuidgen`, converted to lowercase)
2. Sets quotas with `rbd namespace quota set`
3. Verifies the values with `rbd namespace quota show --format json`, piping through `jq` (a command-line JSON processor)
4. Creates a 3 MiB image (succeeds, within the 4 MiB quota)
5. Tries to create another 3 MiB image (`expect_false`, meaning the command should fail with `-EDQUOT`)
6. Removes the first image, freeing the quota
7. Tests resize enforcement: creates an image, grows it within quota, tries to grow beyond quota (fails), shrinks it (succeeds)
8. Tests partial quota updates: changes only `max_bytes`, verifies `max_objects` is unchanged
9. Tests quota removal: `--no-max-bytes`, verifies `max_bytes` is 0 while `max_objects` is preserved
10. Validates that setting quota on an empty namespace name fails
11. Cleans up by removing the namespace

### C API unit tests (`test_librbd.cc`)

Eight test cases using Google Test. Each uses `REQUIRE_FORMAT_V2()`, a macro that skips the test if format 1 is configured (namespace quotas only work with format 2, the modern RBD on-disk format).

Tests use `BOOST_SCOPE_EXIT` (a Boost macro that registers cleanup code to run when the current scope exits, regardless of how: return, assertion failure, etc.) to ensure namespaces and IoCtx handles are cleaned up.

Several tests contain a block like:
```cpp
librados::IoCtx tmp;
librados::IoCtx::from_rados_ioctx_t(ioctx, tmp);
tmp.set_namespace(ns_name);
```
This is needed because the test framework uses `rados_test_stub`, a mock implementation of the RADOS client. The stub's C function `rados_ioctx_set_namespace` does not correctly handle type casting (the C handle points to a `TestIoCtxImpl`, not the real `IoCtxImpl`), so the C++ `IoCtx::set_namespace()` is used instead, which goes through the stub-aware overrides.

**NamespaceQuotaCreate**: sets a 12 MiB quota, creates an 8 MiB image, verifies `used_bytes >= 8 MiB` and `used_objects > 0`. Tries to create another 8 MiB image, expects `-EDQUOT`. Removes the first image, creates a third (succeeds because quota was freed), removes it, verifies usage is back to zero.

**NamespaceQuotaGetInfo**: verifies the internal `api::Namespace<>::get_quota` returns the correct `max_bytes` and `max_objects`. Creates and removes an image, verifies usage returns to zero.

**NamespaceQuotaIsolation**: creates two namespaces, one with a 6 MiB byte quota (`full_ns`) and one without any quota (`open_ns`). Creates a 4 MiB image in `full_ns` (succeeds), tries another 4 MiB (fails with `-EDQUOT`). Creates a 4 MiB image in `open_ns` (succeeds, proving the quota on one namespace does not affect the other).

**NamespaceQuotaDefaultNamespace**: verifies that calling `rbd_namespace_set_quota` with an empty namespace name returns `-EINVAL`, and that the internal API also returns `-EINVAL`. Quotas are only meaningful for named namespaces.

**NamespaceQuotaPP** ("PP" means C++ API, following Ceph's naming convention): exercises the C++ `RBD::namespace_set_quota` and `RBD::namespace_get_quota`. Sets a 16 MiB / 64 objects quota, reads it back, creates a 4 MiB image, verifies usage increased, removes the image, verifies usage is zero.

**NamespaceQuotaResize**: sets a 12 MiB quota, creates a 4 MiB image, opens it, resizes to 8 MiB (succeeds), verifies `used_bytes >= 8 MiB`. Tries to resize to 20 MiB (fails with `-EDQUOT`), verifies usage unchanged. Shrinks to 4 MiB (succeeds), verifies usage decreased. Re-grows to 8 MiB (succeeds, proving the shrink released quota). Closes and removes the image.

**NamespaceQuotaObjectCount**: sets an object-count-only quota (`max_objects=1`, no byte limit). Creates a 4 MiB image (uses 1 object with the default 4 MiB object order). Tries a second image (fails with `-EDQUOT`). Removes the first, verifies `used_objects == 0`.

**NamespaceQuotaCreateFailureRollback**: sets a 12 MiB quota, creates a 4 MiB image named `rollback_img`. Records the used_bytes after the first create. Tries to create another image with the same name `rollback_img`: this fails with `-EEXIST` (already exists) at the "add to directory" step, which happens after quota has already been reserved. The test verifies that `used_bytes` is unchanged after the failed create, proving that the `complete()` rollback path correctly released the reservation. Finally, removes the image and verifies usage is zero.
