// Copyright 2020 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TENSORSTORE_KVSTORE_REGISTRY_H_
#define TENSORSTORE_KVSTORE_REGISTRY_H_

/// \file Interface for defining and registering a KeyValueStore driver that
///     supports a JSON representation.
///
/// To define a KeyValueStore driver, create a `Derived` class that inherits
/// from the CRTP base `RegisteredKeyValueStore<Derived>`, and define a global
/// constant of type `KeyValueStoreDriverRegistration<Derived>` to register it.
///
/// Refer to `memory/memory_key_value_store.cc` for an example.

#include "tensorstore/internal/context_binding.h"
#include "tensorstore/internal/json_registry.h"
#include "tensorstore/json_serialization_options.h"
#include "tensorstore/kvstore/driver.h"

namespace tensorstore {
namespace internal_kvstore {

using kvstore::Driver;
using kvstore::DriverPtr;
using kvstore::DriverSpec;
using kvstore::DriverSpecPtr;

template <typename Derived>
class DriverOpenState;

struct DriverFromJsonOptions : public JsonSerializationOptions {
  const std::string& path;
};

using DriverRegistry =
    internal::JsonRegistry<DriverSpec, DriverFromJsonOptions,
                           JsonSerializationOptions,
                           internal::IntrusivePtr<const DriverSpec>>;

/// Returns the global KeyValueStore driver registry.
///
/// This should not be called directly by code outside this module.
DriverRegistry& GetDriverRegistry();

/// CRTP base class for KeyValueStore implementations that support a JSON
/// representation.
///
/// The `Derived` class must override all of the virtual methods of
/// `KeyValueStore`, except for the members noted as being defined automatically
/// by `RegisteredKeyValueStore`, and in addition must define the following
/// members:
///
/// - The `id` member specifies the string driver identifier:
///
///     static constexpr char id[] = "my_driver_id";
///
/// - The `SpecData` class includes as members the parameters and resources
///   necessary to create/open the driver.
///
///   It must define an `ApplyMembers` method for compatibility with
///   `ContextBindingTraits` (refer to `tensorstore/internal/context_binding.h`
///   for details).
///
///   Members of `SpecData` should be referenced in the `json_binder` and
///   `EncodeCacheKey` implementation, as noted below.
///
///     struct SpecData {
///        // Example members:
///       int mem1;
///       Context::Resource<SomeResource> mem2;
///
///       // For compatibility with `ContextBindingTraits`.
///       constexpr static auto ApplyMembers = [](auto& x, auto f) {
///         return f(f.mem2);
///       };
///     };
///
/// - The `json_binder` member must be a JSON object binder for `SpecData`.
///   This should handle converting each member of `SpecData` to/from the JSON
///   representation.
///
///     constexpr static auto json_binder = jb::Object(
///         jb::Member("mem1", jb::Projection(&SpecData::mem1)),
///         jb::Member("mem2", jb::Projection(&SpecData::mem2)));
///
/// - The static `EncodeCacheKey` method encodes the `SpecData` representation
///   as a cache key.  It will only be called after binding context resources.
///   Typically this just calls `tensorstore::internal::EncodeCacheKey` with the
///   members that are relevant to caching.  Members that only affect creation
///   but not opening should normally be skipped.
///
///     static void EncodeCacheKey(std::string *out,
///                                const SpecData& data) {
///       tensorstore::internal::EncodeCacheKey(out, data.mem1, data.mem2);
///     }
///
/// - The static `Open` method is called to initiate opening the driver.  This
///   is called by `kvstore::Open`.  Note that `KeyValueStoreOpenState` is
///   a CRTP class template parameterized by the `Derived` driver type.
///
///     static void Open(internal_kvstore::DriverOpenState<Derived> state) {
///       // Access the context-bound `SpecData` representation as
///       `state.spec()`.
///       // Access the newly allocated `Derived` object as `state.driver()`.
///       // Report errors via `state.SetError`.
///     }
///
/// - The `GetBoundSpecData` method must set `spec` to the context-bound
///   representation of the JSON specification of the driver.
///
///     absl::Status GetBoundSpecData(SpecData& spec) const;
///
/// - The `Derived` class must be default constructible.  Any required
///   initialization should be performed in the `Open` method.
///
/// Refer to `memory/memory_key_value_store.cc` for an example driver
/// implementation.
template <typename Derived, typename Parent = Driver>
class RegisteredDriver : public Parent {
 private:
  /// Encodes the cache key from the context-bound `SpecData` representation.
  ///
  /// This is used by `RegisteredKeyValueStore::EncodeCacheKey` below and by
  /// `RegisteredKeyValueStoreBoundSpec::EncodeCacheKey`.
  ///
  /// The `SpecData` template parameter is always equal to
  /// `typename Derived::SpecData`, but is specified as a template parameter
  /// because `Derived` is incomplete when this class template is instantiated.
  template <typename SpecData>
  static void EncodeCacheKeyImpl(std::string* out, const SpecData& data) {
    internal::EncodeCacheKey(out, typeid(Derived));
    Derived::EncodeCacheKey(out, data);
  }

 public:
  void EncodeCacheKey(std::string* out) const override {
    using SpecData = typename Derived::SpecData;
    // Generates a cache key by obtaining the `SpecData` representation,
    // then computing the cache key from that.
    SpecData bound_spec_data;
    if (auto status = static_cast<const Derived*>(this)->GetBoundSpecData(
            bound_spec_data);
        !status.ok()) {
      // Could not obtain bound spec data.  Just use the default implementation
      // that encodes the exact object identity.
      return Driver::EncodeCacheKey(out);
    }
    EncodeCacheKeyImpl(out, bound_spec_data);
  }

  Result<DriverSpecPtr> GetBoundSpec() const override {
    using SpecImpl = RegisteredDriverSpec<Derived>;
    internal::IntrusivePtr<SpecImpl> spec(new SpecImpl);
    spec->context_binding_state_ = ContextBindingState::bound;
    TENSORSTORE_RETURN_IF_ERROR(
        static_cast<const Derived*>(this)->GetBoundSpecData(spec->data_));
    return spec;
  }

 private:
  template <typename>
  friend class DriverRegistration;
  template <typename>
  friend class RegisteredDriverSpec;
};

/// Parameter type for the static `Open` method that driver types inherited from
/// `RegisteredKeyValueStore` must implement.  For asynchronous open
/// implementations, this type may be copied and the copy retained until the
/// operation completes.
template <typename Derived>
class DriverOpenState {
  template <typename, typename>
  friend class RegisteredDriver;
  template <typename>
  friend class RegisteredDriverSpec;

 public:
  using SpecData = typename Derived::SpecData;

  /// Returns the promise that must be marked ready to indicate the open has
  /// completed.  The result is initialized in a success state with a copy of
  /// `driver()`, such that when the last reference to the `promise` is
  /// released, the promise is marked ready and the open is considered to have
  /// completed successfully.  The result should only be changed to indicate an
  /// error.
  const Promise<DriverPtr>& promise() const { return promise_; }

  /// Sets an error on the promise, indicating that the open failed.
  void SetError(Status status) { promise_.SetResult(std::move(status)); }

  /// Returns a reference to the `Driver` being opened.
  Derived& driver() const { return *driver_; }

  /// Returns a reference to the bound spec.
  const SpecData& spec() const { return spec_->data_; }

 private:
  Driver::PtrT<Derived> driver_;
  Promise<DriverPtr> promise_;
  internal::IntrusivePtr<const RegisteredDriverSpec<Derived>> spec_;
};

template <typename Derived>
class RegisteredDriverSpec : public DriverSpec {
  using SpecData = typename Derived::SpecData;

 public:
  absl::Status BindContext(const Context& context) override {
    return internal::ContextBindingTraits<SpecData>::Bind(data_, context);
  }

  void UnbindContext(
      const internal::ContextSpecBuilder& context_builder) override {
    internal::ContextBindingTraits<SpecData>::Unbind(data_, context_builder);
  }

  void StripContext() override {
    internal::ContextBindingTraits<SpecData>::Strip(data_);
  }

  void EncodeCacheKey(std::string* out) const override {
    Derived::RegisteredDriver::EncodeCacheKeyImpl(out, data_);
  }

  DriverSpecPtr Clone() const final {
    return DriverSpecPtr(new RegisteredDriverSpec(*this));
  }

  Future<DriverPtr> DoOpen() const override {
    DriverOpenState<Derived> open_state;
    open_state.spec_.reset(this);
    open_state.driver_.reset(new Derived);
    auto [promise, future] =
        PromiseFuturePair<DriverPtr>::Make(open_state.driver_);
    open_state.promise_ = std::move(promise);
    Derived::Open(std::move(open_state));
    return future;
  }

  SpecData data_;
};

/// Registers a KeyValueStore driver implementation.
///
/// Example usage:
///
///     class MyDriver : public internal_kvstore::RegisteredDriver<MyDriver> {
///       // ...
///     };
///
///     const internal_kvstore::DriverRegistration<MyDriver> registration;
///
template <typename Derived>
class DriverRegistration {
 public:
  DriverRegistration() {
    GetDriverRegistry().Register<RegisteredDriverSpec<Derived>>(
        Derived::id,
        internal_json_binding::Projection(&RegisteredDriverSpec<Derived>::data_,
                                          Derived::json_binder));
  }
};

}  // namespace internal_kvstore
}  // namespace tensorstore

#endif  // TENSORSTORE_KVSTORE_REGISTRY_H_
