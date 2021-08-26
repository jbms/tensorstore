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

#include "tensorstore/driver/driver.h"

#include <vector>

#include "tensorstore/context.h"
#include "tensorstore/data_type.h"
#include "tensorstore/driver/kvs_backed_chunk_driver.h"
#include "tensorstore/driver/registry.h"
#include "tensorstore/driver/zarr/driver_impl.h"
#include "tensorstore/driver/zarr/metadata.h"
#include "tensorstore/driver/zarr/spec.h"
#include "tensorstore/index.h"
#include "tensorstore/index_space/index_domain_builder.h"
#include "tensorstore/index_space/transform_broadcastable_array.h"
#include "tensorstore/internal/cache/cache_key.h"
#include "tensorstore/internal/cache/chunk_cache.h"
#include "tensorstore/internal/json.h"
#include "tensorstore/internal/path.h"
#include "tensorstore/internal/type_traits.h"
#include "tensorstore/kvstore/kvstore.h"
#include "tensorstore/tensorstore.h"
#include "tensorstore/util/future.h"

namespace tensorstore {
namespace internal_zarr {

namespace {
constexpr const char kZarrMetadataKey[] = ".zarray";

inline char GetDimensionSeparatorChar(DimensionSeparator dimension_separator) {
  return dimension_separator == DimensionSeparator::kDotSeparated ? '.' : '/';
}

DimensionSeparator GetDimensionSeparator(
    const ZarrPartialMetadata& partial_metadata, const ZarrMetadata& metadata) {
  if (metadata.dimension_separator) {
    return *metadata.dimension_separator;
  } else if (partial_metadata.dimension_separator) {
    return *partial_metadata.dimension_separator;
  }
  return DimensionSeparator::kDotSeparated;
}

Result<ZarrMetadataPtr> ParseEncodedMetadata(std::string_view encoded_value) {
  nlohmann::json raw_data = nlohmann::json::parse(encoded_value, nullptr,
                                                  /*allow_exceptions=*/false);
  if (raw_data.is_discarded()) {
    return absl::FailedPreconditionError("Invalid JSON");
  }
  auto metadata = std::make_shared<ZarrMetadata>();
  TENSORSTORE_ASSIGN_OR_RETURN(*metadata,
                               ZarrMetadata::FromJson(std::move(raw_data)));
  return metadata;
}

class MetadataCache : public internal_kvs_backed_chunk_driver::MetadataCache {
  using Base = internal_kvs_backed_chunk_driver::MetadataCache;

 public:
  using Base::Base;
  std::string GetMetadataStorageKey(std::string_view entry_key) override {
    return tensorstore::StrCat(entry_key, kZarrMetadataKey);
  }

  Result<MetadataPtr> DecodeMetadata(std::string_view entry_key,
                                     absl::Cord encoded_metadata) override {
    return ParseEncodedMetadata(encoded_metadata.Flatten());
  }

  Result<absl::Cord> EncodeMetadata(std::string_view entry_key,
                                    const void* metadata) override {
    return absl::Cord(
        ::nlohmann::json(*static_cast<const ZarrMetadata*>(metadata)).dump());
  }
};

namespace jb = tensorstore::internal_json_binding;

class ZarrDriver
    : public internal_kvs_backed_chunk_driver::RegisteredKvsDriver<ZarrDriver> {
  using Base =
      internal_kvs_backed_chunk_driver::RegisteredKvsDriver<ZarrDriver>;

 public:
  using Base::Base;

  class OpenState;

  constexpr static char id[] = "zarr";

  struct SpecData : public internal_kvs_backed_chunk_driver::SpecData {
    ZarrPartialMetadata partial_metadata;
    SelectedField selected_field;

    constexpr static auto ApplyMembers = [](auto& x, auto f) {
      return f(
          internal::BaseCast<internal_kvs_backed_chunk_driver::SpecData>(x),
          x.partial_metadata, x.selected_field);
    };
  };

  static Status ApplyOptions(SpecData& spec, SpecOptions&& options) {
    if (options.minimal_spec) {
      spec.partial_metadata = ZarrPartialMetadata{};
    }
    return Base::ApplyOptions(spec, std::move(options));
  }

  static Result<SpecRankAndFieldInfo> GetSpecInfo(const SpecData& spec) {
    return GetSpecRankAndFieldInfo(spec.partial_metadata, spec.selected_field,
                                   spec.schema);
  }

  static inline const auto json_binder = jb::Sequence(
      internal_kvs_backed_chunk_driver::SpecJsonBinder,
      jb::Member("metadata", jb::Projection(&SpecData::partial_metadata,
                                            jb::DefaultInitializedValue())),
      // Deprecated `key_encoding` property.
      jb::LoadSave(jb::OptionalMember(
          "key_encoding",
          jb::Compose<DimensionSeparator>(
              [](auto is_loading, const auto& options, auto* obj,
                 DimensionSeparator* value) {
                auto& sep = obj->partial_metadata.dimension_separator;
                if (sep && *sep != *value) {
                  return absl::InvalidArgumentError(tensorstore::StrCat(
                      "value (", ::nlohmann::json(*value).dump(),
                      ") does not match value in metadata (",
                      ::nlohmann::json(*sep).dump(), ")"));
                }
                sep = *value;
                return absl::OkStatus();
              },
              DimensionSeparatorJsonBinder))),
      jb::Member("field",
                 jb::Projection(&SpecData::selected_field,
                                jb::DefaultValue<jb::kNeverIncludeDefaults>(
                                    [](auto* obj) { *obj = std::string{}; }))),
      jb::Initialize([](auto* obj) {
        TENSORSTORE_ASSIGN_OR_RETURN(auto info, GetSpecInfo(*obj));
        if (info.full_rank != dynamic_rank) {
          TENSORSTORE_RETURN_IF_ERROR(
              obj->schema.Set(RankConstraint(info.full_rank)));
        }
        if (info.field) {
          TENSORSTORE_RETURN_IF_ERROR(obj->schema.Set(info.field->dtype));
        }
        return absl::OkStatus();
      }));

  static Result<IndexDomain<>> SpecGetDomain(const SpecData& spec) {
    TENSORSTORE_ASSIGN_OR_RETURN(auto info, GetSpecInfo(spec));
    return GetDomainFromMetadata(info, spec.partial_metadata.shape,
                                 spec.schema);
  }

  static Result<CodecSpec::Ptr> SpecGetCodec(const SpecData& spec) {
    auto codec_spec = CodecSpec::Make<ZarrCodecSpec>();
    codec_spec->compressor = spec.partial_metadata.compressor;
    TENSORSTORE_RETURN_IF_ERROR(codec_spec->MergeFrom(spec.schema.codec()));
    return codec_spec;
  }

  static Result<ChunkLayout> SpecGetChunkLayout(const SpecData& spec) {
    auto chunk_layout = spec.schema.chunk_layout();
    TENSORSTORE_ASSIGN_OR_RETURN(auto info, GetSpecInfo(spec));
    TENSORSTORE_RETURN_IF_ERROR(
        SetChunkLayoutFromMetadata(info, spec.partial_metadata.chunks,
                                   spec.partial_metadata.order, chunk_layout));
    return chunk_layout;
  }

  static Result<SharedArray<const void>> SpecGetFillValue(
      const SpecData& spec, IndexTransformView<> transform) {
    SharedArrayView<const void> fill_value = spec.schema.fill_value();

    const auto& metadata = spec.partial_metadata;
    if (metadata.dtype && metadata.fill_value) {
      TENSORSTORE_ASSIGN_OR_RETURN(
          size_t field_index,
          GetFieldIndex(*metadata.dtype, spec.selected_field));
      fill_value = (*metadata.fill_value)[field_index];
    }

    if (!fill_value.valid() || !transform.valid()) {
      return SharedArray<const void>(fill_value);
    }

    const DimensionIndex output_rank = transform.output_rank();
    if (output_rank < fill_value.rank()) {
      return absl::InvalidArgumentError(
          tensorstore::StrCat("Transform with output rank ", output_rank,
                              " is not compatible with metadata"));
    }
    Index pseudo_shape[kMaxRank];
    std::fill_n(pseudo_shape, output_rank - fill_value.rank(), kInfIndex + 1);
    for (DimensionIndex i = 0; i < fill_value.rank(); ++i) {
      Index size = fill_value.shape()[i];
      if (size == 1) size = kInfIndex + 1;
      pseudo_shape[output_rank - fill_value.rank() + i] = size;
    }
    return TransformOutputBroadcastableArray(
        transform, std::move(fill_value),
        IndexDomain(span(pseudo_shape, output_rank)));
  }

  Result<SharedArray<const void>> GetFillValue(
      IndexTransformView<> transform) override {
    const auto& metadata = *static_cast<const ZarrMetadata*>(
        this->cache()->initial_metadata_.get());
    const auto& fill_value = metadata.fill_value[this->component_index()];
    if (!fill_value.valid()) return {std::in_place};
    const auto& field = metadata.dtype.fields[this->component_index()];
    IndexDomainBuilder builder(field.field_shape.size() + metadata.rank);
    span<Index> shape = builder.shape();
    std::fill_n(shape.begin(), metadata.rank, kInfIndex + 1);
    std::copy(field.field_shape.begin(), field.field_shape.end(),
              shape.end() - field.field_shape.size());
    TENSORSTORE_ASSIGN_OR_RETURN(auto output_domain, builder.Finalize());
    return TransformOutputBroadcastableArray(transform, fill_value,
                                             output_domain);
  }
};

class DataCache : public internal_kvs_backed_chunk_driver::DataCache {
  using Base = internal_kvs_backed_chunk_driver::DataCache;

 public:
  explicit DataCache(Initializer initializer, std::string key_prefix,
                     DimensionSeparator dimension_separator)
      : Base(initializer,
             GetChunkGridSpecification(*static_cast<const ZarrMetadata*>(
                 initializer.metadata.get()))),
        key_prefix_(std::move(key_prefix)),
        dimension_separator_(dimension_separator) {}

  Status ValidateMetadataCompatibility(const void* existing_metadata_ptr,
                                       const void* new_metadata_ptr) override {
    assert(existing_metadata_ptr);
    assert(new_metadata_ptr);
    const auto& existing_metadata =
        *static_cast<const ZarrMetadata*>(existing_metadata_ptr);
    const auto& new_metadata =
        *static_cast<const ZarrMetadata*>(new_metadata_ptr);
    if (IsMetadataCompatible(existing_metadata, new_metadata)) {
      return absl::OkStatus();
    }
    return absl::FailedPreconditionError(
        StrCat("Updated zarr metadata ", ::nlohmann::json(new_metadata).dump(),
               " is incompatible with existing metadata ",
               ::nlohmann::json(existing_metadata).dump()));
  }

  void GetChunkGridBounds(
      const void* metadata_ptr, MutableBoxView<> bounds,
      BitSpan<std::uint64_t> implicit_lower_bounds,
      BitSpan<std::uint64_t> implicit_upper_bounds) override {
    const auto& metadata = *static_cast<const ZarrMetadata*>(metadata_ptr);
    assert(bounds.rank() == static_cast<DimensionIndex>(metadata.shape.size()));
    assert(bounds.rank() == implicit_lower_bounds.size());
    assert(bounds.rank() == implicit_upper_bounds.size());
    std::fill(bounds.origin().begin(), bounds.origin().end(), Index(0));
    std::copy(metadata.shape.begin(), metadata.shape.end(),
              bounds.shape().begin());
    implicit_lower_bounds.fill(false);
    implicit_upper_bounds.fill(true);
  }

  Result<std::shared_ptr<const void>> GetResizedMetadata(
      const void* existing_metadata, span<const Index> new_inclusive_min,
      span<const Index> new_exclusive_max) override {
    auto new_metadata = std::make_shared<ZarrMetadata>(
        *static_cast<const ZarrMetadata*>(existing_metadata));
    const DimensionIndex rank = new_metadata->shape.size();
    assert(rank == new_inclusive_min.size());
    assert(rank == new_exclusive_max.size());
    for (DimensionIndex i = 0; i < rank; ++i) {
      assert(ExplicitIndexOr(new_inclusive_min[i], 0) == 0);
      const Index new_size = new_exclusive_max[i];
      if (new_size == kImplicit) continue;
      new_metadata->shape[i] = new_size;
    }
    return new_metadata;
  }

  /// Returns the ChunkCache grid to use for the given metadata.
  static internal::ChunkGridSpecification GetChunkGridSpecification(
      const ZarrMetadata& metadata) {
    internal::ChunkGridSpecification::Components components;
    components.reserve(metadata.dtype.fields.size());
    std::vector<DimensionIndex> chunked_to_cell_dimensions(
        metadata.chunks.size());
    std::iota(chunked_to_cell_dimensions.begin(),
              chunked_to_cell_dimensions.end(), static_cast<DimensionIndex>(0));
    for (std::size_t field_i = 0; field_i < metadata.dtype.fields.size();
         ++field_i) {
      const auto& field = metadata.dtype.fields[field_i];
      const auto& field_layout = metadata.chunk_layout.fields[field_i];
      auto fill_value = metadata.fill_value[field_i];
      if (!fill_value.valid()) {
        // Use value-initialized rank-0 fill value.
        fill_value = AllocateArray(span<const Index, 0>{}, c_order, value_init,
                                   field.dtype);
      }
      assert(fill_value.rank() <=
             static_cast<DimensionIndex>(field.field_shape.size()));
      const DimensionIndex cell_rank = field_layout.full_chunk_shape().size();
      SharedArray<const void> chunk_fill_value;
      chunk_fill_value.layout().set_rank(cell_rank);
      chunk_fill_value.element_pointer() = fill_value.element_pointer();
      const DimensionIndex fill_value_start_dim = cell_rank - fill_value.rank();
      for (DimensionIndex cell_dim = 0; cell_dim < fill_value_start_dim;
           ++cell_dim) {
        chunk_fill_value.shape()[cell_dim] =
            field_layout.full_chunk_shape()[cell_dim];
        chunk_fill_value.byte_strides()[cell_dim] = 0;
      }
      for (DimensionIndex cell_dim = fill_value_start_dim; cell_dim < cell_rank;
           ++cell_dim) {
        const Index size = field_layout.full_chunk_shape()[cell_dim];
        assert(fill_value.shape()[cell_dim - fill_value_start_dim] == size);
        chunk_fill_value.shape()[cell_dim] = size;
        chunk_fill_value.byte_strides()[cell_dim] =
            fill_value.byte_strides()[cell_dim - fill_value_start_dim];
      }
      components.emplace_back(std::move(chunk_fill_value),
                              // Since all chunked dimensions are resizable in
                              // zarr, just specify unbounded
                              // `component_bounds`.
                              Box<>(cell_rank), chunked_to_cell_dimensions);
    }
    return internal::ChunkGridSpecification{std::move(components)};
  }

  Result<absl::InlinedVector<SharedArrayView<const void>, 1>> DecodeChunk(
      const void* metadata, span<const Index> chunk_indices,
      absl::Cord data) override {
    return internal_zarr::DecodeChunk(
        *static_cast<const ZarrMetadata*>(metadata), std::move(data));
  }

  Result<absl::Cord> EncodeChunk(
      const void* metadata, span<const Index> chunk_indices,
      span<const ArrayView<const void>> component_arrays) override {
    return internal_zarr::EncodeChunk(
        *static_cast<const ZarrMetadata*>(metadata), component_arrays);
  }

  std::string GetChunkStorageKey(const void* metadata,
                                 span<const Index> cell_indices) override {
    return tensorstore::StrCat(
        key_prefix_, EncodeChunkIndices(cell_indices, dimension_separator_));
  }

  absl::Status GetBoundSpecData(
      internal_kvs_backed_chunk_driver::SpecData& spec_base,
      const void* metadata_ptr, std::size_t component_index) override {
    auto& spec = static_cast<ZarrDriver::SpecData&>(spec_base);
    const auto& metadata = *static_cast<const ZarrMetadata*>(metadata_ptr);
    spec.selected_field = EncodeSelectedField(component_index, metadata.dtype);
    auto& pm = spec.partial_metadata;
    pm.rank = metadata.rank;
    pm.zarr_format = metadata.zarr_format;
    pm.shape = metadata.shape;
    pm.chunks = metadata.chunks;
    pm.compressor = metadata.compressor;
    pm.filters = metadata.filters;
    pm.order = metadata.order;
    pm.dtype = metadata.dtype;
    pm.fill_value = metadata.fill_value;
    pm.dimension_separator = dimension_separator_;
    return absl::OkStatus();
  }

  Result<ChunkLayout> GetChunkLayout(const void* metadata_ptr,
                                     std::size_t component_index) override {
    const auto& metadata = *static_cast<const ZarrMetadata*>(metadata_ptr);
    ChunkLayout chunk_layout;
    TENSORSTORE_RETURN_IF_ERROR(internal_zarr::SetChunkLayoutFromMetadata(
        GetSpecRankAndFieldInfo(metadata, component_index), metadata.chunks,
        metadata.order, chunk_layout));
    TENSORSTORE_RETURN_IF_ERROR(chunk_layout.Finalize());
    return chunk_layout;
  }

  Result<CodecSpec::Ptr> GetCodec(const void* metadata,
                                  std::size_t component_index) override {
    return internal_zarr::GetCodecSpecFromMetadata(
        *static_cast<const ZarrMetadata*>(metadata));
  }

  std::string GetBaseKvstorePath() override { return key_prefix_; }

 private:
  std::string key_prefix_;
  DimensionSeparator dimension_separator_;
};

class ZarrDriver::OpenState : public ZarrDriver::OpenStateBase {
 public:
  using ZarrDriver::OpenStateBase::OpenStateBase;

  std::string GetPrefixForDeleteExisting() override {
    return spec().store.path;
  }

  std::string GetMetadataCacheEntryKey() override { return spec().store.path; }

  std::unique_ptr<internal_kvs_backed_chunk_driver::MetadataCache>
  GetMetadataCache(MetadataCache::Initializer initializer) override {
    return std::make_unique<MetadataCache>(std::move(initializer));
  }

  Result<std::shared_ptr<const void>> Create(
      const void* existing_metadata) override {
    if (existing_metadata) {
      return absl::AlreadyExistsError("");
    }
    TENSORSTORE_ASSIGN_OR_RETURN(
        auto metadata,
        internal_zarr::GetNewMetadata(spec().partial_metadata,
                                      spec().selected_field, spec().schema),
        tensorstore::MaybeAnnotateStatus(
            _, "Cannot create using specified \"metadata\" and schema"));
    return metadata;
  }

  std::string GetDataCacheKey(const void* metadata) override {
    std::string result;
    const auto& spec = this->spec();
    const auto& zarr_metadata = *static_cast<const ZarrMetadata*>(metadata);
    internal::EncodeCacheKey(
        &result, spec.store.path,
        GetDimensionSeparator(spec.partial_metadata, zarr_metadata),
        zarr_metadata);
    return result;
  }

  std::unique_ptr<internal_kvs_backed_chunk_driver::DataCache> GetDataCache(
      DataCache::Initializer initializer) override {
    const auto& metadata =
        *static_cast<const ZarrMetadata*>(initializer.metadata.get());
    return std::make_unique<DataCache>(
        std::move(initializer), spec().store.path,
        GetDimensionSeparator(spec().partial_metadata, metadata));
  }

  Result<std::size_t> GetComponentIndex(const void* metadata_ptr,
                                        OpenMode open_mode) override {
    const auto& metadata = *static_cast<const ZarrMetadata*>(metadata_ptr);
    TENSORSTORE_RETURN_IF_ERROR(
        ValidateMetadata(metadata, spec().partial_metadata));
    TENSORSTORE_ASSIGN_OR_RETURN(
        auto field_index, GetFieldIndex(metadata.dtype, spec().selected_field));
    TENSORSTORE_RETURN_IF_ERROR(
        ValidateMetadataSchema(metadata, field_index, spec().schema));
    return field_index;
  }
};

const internal::DriverRegistration<ZarrDriver> registration;

}  // namespace

std::string EncodeChunkIndices(span<const Index> indices,
                               DimensionSeparator dimension_separator) {
  const char separator = GetDimensionSeparatorChar(dimension_separator);
  std::string key;
  for (DimensionIndex i = 0; i < indices.size(); ++i) {
    if (i != 0) {
      StrAppend(&key, separator, indices[i]);
    } else {
      StrAppend(&key, indices[i]);
    }
  }
  return key;
}

}  // namespace internal_zarr
}  // namespace tensorstore
