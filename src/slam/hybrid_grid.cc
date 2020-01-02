/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <Eigen/Core>
#include <array>
#include <boost/container/set.hpp>
#include <boost/unordered_set.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <pcl/filters/voxel_grid.h>

#include "common/common.h"
#include "glog/logging.h"
#include "slam/hybrid_grid.h"

// Converts an 'index' with each dimension from 0 to 2^'bits' - 1 to a flat
// z-major index.
inline int ToFlatIndex(const Eigen::Array3i& index, const int bits) {
  DCHECK((index >= 0).all() && (index < (1 << bits)).all()) << index;
  return (((index.z() << bits) + index.y()) << bits) + index.x();
}

// Converts a flat z-major 'index' to a 3-dimensional index with each dimension
// from 0 to 2^'bits' - 1.
inline Eigen::Array3i To3DIndex(const int index, const int bits) {
  DCHECK_LT(index, 1 << (3 * bits));
  const int mask = (1 << bits) - 1;
  return {index & mask, (index >> bits) & mask, (index >> bits) >> bits};
}

// A function to compare value to the default value. (Allows specializations).
template <typename TValueType>
bool IsDefaultValue(const TValueType& v) {
  return v == TValueType();
}

// Specialization to compare a std::vector to the default value.
template <typename TElementType>
bool IsDefaultValue(const std::vector<TElementType>& v) {
  return v.empty();
}

// A flat grid of '2^kBits' x '2^kBits' x '2^kBits' voxels storing values of
// type 'ValueType' in contiguous memory. Indices in each dimension are 0-based.
template <typename TValueType, int kBits>
class FlatGrid {
 public:
  using ValueType = TValueType;

  // Creates a new flat grid with all values being default constructed.
  FlatGrid() {
    for (ValueType& value : cells_) {
      value = ValueType();
    }
  }

  FlatGrid(const FlatGrid&) = delete;
  FlatGrid& operator=(const FlatGrid&) = delete;

  // Returns the number of voxels per dimension.
  static int grid_size() { return 1 << kBits; }

  // Returns the value stored at 'index', each dimension of 'index' being
  // between 0 and grid_size() - 1.
  ValueType value(const Eigen::Array3i& index) const {
    return cells_[ToFlatIndex(index, kBits)];
  }

  // Returns a pointer to a value to allow changing it.
  ValueType* mutable_value(const Eigen::Array3i& index) {
    return &cells_[ToFlatIndex(index, kBits)];
  }

  // An iterator for iterating over all values not comparing equal to the
  // default constructed value.
  class Iterator {
   public:
    Iterator() : current_(nullptr), end_(nullptr) {}

    explicit Iterator(const FlatGrid& flat_grid)
        : current_(flat_grid.cells_.data()),
          end_(flat_grid.cells_.data() + flat_grid.cells_.size()) {
      while (!Done() && IsDefaultValue(*current_)) {
        ++current_;
      }
    }

    void Next() {
      DCHECK(!Done());
      do {
        ++current_;
      } while (!Done() && IsDefaultValue(*current_));
    }

    bool Done() const { return current_ == end_; }

    Eigen::Array3i GetCellIndex() const {
      DCHECK(!Done());
      const int index = (1 << (3 * kBits)) - (end_ - current_);
      return To3DIndex(index, kBits);
    }

    const ValueType& GetValue() const {
      DCHECK(!Done());
      return *current_;
    }

   private:
    const ValueType* current_;
    const ValueType* end_;
  };

 private:
  std::array<ValueType, 1 << (3 * kBits)> cells_;
};

// A grid consisting of '2^kBits' x '2^kBits' x '2^kBits' grids of type
// 'WrappedGrid'. Wrapped grids are constructed on first access via
// 'mutable_value()'.
template <typename WrappedGrid, int kBits>
class NestedGrid {
 public:
  using ValueType = typename WrappedGrid::ValueType;

  // Returns the number of voxels per dimension.
  static int grid_size() { return WrappedGrid::grid_size() << kBits; }

  // Returns the value stored at 'index', each dimension of 'index' being
  // between 0 and grid_size() - 1.
  ValueType value(const Eigen::Array3i& index) const {
    const Eigen::Array3i meta_index = GetMetaIndex(index);
    const WrappedGrid* const meta_cell =
        meta_cells_[ToFlatIndex(meta_index, kBits)].get();
    if (meta_cell == nullptr) {
      return ValueType();
    }
    const Eigen::Array3i inner_index =
        index - meta_index * WrappedGrid::grid_size();
    return meta_cell->value(inner_index);
  }

  // Returns a pointer to the value at 'index' to allow changing it. If
  // necessary a new wrapped grid is constructed to contain that value.
  ValueType* mutable_value(const Eigen::Array3i& index) {
    const Eigen::Array3i meta_index = GetMetaIndex(index);
    std::unique_ptr<WrappedGrid>& meta_cell =
        meta_cells_[ToFlatIndex(meta_index, kBits)];
    if (meta_cell == nullptr) {
      meta_cell.reset(new WrappedGrid());
    }
    const Eigen::Array3i inner_index =
        index - meta_index * WrappedGrid::grid_size();
    return meta_cell->mutable_value(inner_index);
  }

  // An iterator for iterating over all values not comparing equal to the
  // default constructed value.
  class Iterator {
   public:
    Iterator() : current_(nullptr), end_(nullptr), nested_iterator_() {}

    explicit Iterator(const NestedGrid& nested_grid)
        : current_(nested_grid.meta_cells_.data()),
          end_(nested_grid.meta_cells_.data() + nested_grid.meta_cells_.size()),
          nested_iterator_() {
      AdvanceToValidNestedIterator();
    }

    void Next() {
      DCHECK(!Done());
      nested_iterator_.Next();
      if (!nested_iterator_.Done()) {
        return;
      }
      ++current_;
      AdvanceToValidNestedIterator();
    }

    bool Done() const { return current_ == end_; }

    Eigen::Array3i GetCellIndex() const {
      DCHECK(!Done());
      const int index = (1 << (3 * kBits)) - (end_ - current_);
      return To3DIndex(index, kBits) * WrappedGrid::grid_size() +
             nested_iterator_.GetCellIndex();
    }

    const ValueType& GetValue() const {
      DCHECK(!Done());
      return nested_iterator_.GetValue();
    }

   private:
    void AdvanceToValidNestedIterator() {
      for (; !Done(); ++current_) {
        if (*current_ != nullptr) {
          nested_iterator_ = typename WrappedGrid::Iterator(**current_);
          if (!nested_iterator_.Done()) {
            break;
          }
        }
      }
    }

    const std::unique_ptr<WrappedGrid>* current_;
    const std::unique_ptr<WrappedGrid>* end_;
    typename WrappedGrid::Iterator nested_iterator_;
  };

 private:
  // Returns the Eigen::Array3i (meta) index of the meta cell containing
  // 'index'.
  Eigen::Array3i GetMetaIndex(const Eigen::Array3i& index) const {
    DCHECK((index >= 0).all()) << index;
    const Eigen::Array3i meta_index = index / WrappedGrid::grid_size();
    DCHECK((meta_index < (1 << kBits)).all()) << index;
    return meta_index;
  }

  std::array<std::unique_ptr<WrappedGrid>, 1 << (3 * kBits)> meta_cells_;
};

// A grid consisting of 2x2x2 grids of type 'WrappedGrid' initially. Wrapped
// grids are constructed on first access via 'mutable_value()'. If necessary,
// the grid grows to twice the size in each dimension. The range of indices is
// (almost) symmetric around the origin, i.e. negative indices are allowed.
template <typename WrappedGrid>
class DynamicGrid {
 public:
  using ValueType = typename WrappedGrid::ValueType;

  DynamicGrid() : bits_(1), meta_cells_(8) {}
  DynamicGrid(DynamicGrid&&) noexcept = default;
  DynamicGrid& operator=(DynamicGrid&&) noexcept = default;

  // Returns the current number of voxels per dimension.
  int grid_size() const { return WrappedGrid::grid_size() << bits_; }

  // Returns the value stored at 'index'.
  ValueType value(const Eigen::Array3i& index) const {
    const Eigen::Array3i shifted_index = index + (grid_size() >> 1);
    // The cast to unsigned is for performance to check with 3 comparisons
    // shifted_index.[xyz] >= 0 and shifted_index.[xyz] < grid_size.
    if ((shifted_index.cast<unsigned int>() >= grid_size()).any()) {
      return ValueType();
    }
    const Eigen::Array3i meta_index = GetMetaIndex(shifted_index);
    const WrappedGrid* const meta_cell =
        meta_cells_[ToFlatIndex(meta_index, bits_)].get();
    if (meta_cell == nullptr) {
      return ValueType();
    }
    const Eigen::Array3i inner_index =
        shifted_index - meta_index * WrappedGrid::grid_size();
    return meta_cell->value(inner_index);
  }

  // Returns a pointer to the value at 'index' to allow changing it, dynamically
  // growing the DynamicGrid and constructing new WrappedGrids as needed.
  ValueType* mutable_value(const Eigen::Array3i& index) {
    const Eigen::Array3i shifted_index = index + (grid_size() >> 1);
    // The cast to unsigned is for performance to check with 3 comparisons
    // shifted_index.[xyz] >= 0 and shifted_index.[xyz] < grid_size.
    if ((shifted_index.cast<unsigned int>() >= grid_size()).any()) {
      Grow();
      return mutable_value(index);
    }
    const Eigen::Array3i meta_index = GetMetaIndex(shifted_index);
    std::unique_ptr<WrappedGrid>& meta_cell =
        meta_cells_[ToFlatIndex(meta_index, bits_)];
    if (meta_cell == nullptr) {
      meta_cell.reset(new WrappedGrid());
    }
    const Eigen::Array3i inner_index =
        shifted_index - meta_index * WrappedGrid::grid_size();
    return meta_cell->mutable_value(inner_index);
  }

  // An iterator for iterating over all values not comparing equal to the
  // default constructed value.
  class Iterator {
   public:
    explicit Iterator(const DynamicGrid& dynamic_grid)
        : bits_(dynamic_grid.bits_),
          current_(dynamic_grid.meta_cells_.data()),
          end_(dynamic_grid.meta_cells_.data() +
               dynamic_grid.meta_cells_.size()),
          nested_iterator_() {
      AdvanceToValidNestedIterator();
    }

    void Next() {
      DCHECK(!Done());
      nested_iterator_.Next();
      if (!nested_iterator_.Done()) {
        return;
      }
      ++current_;
      AdvanceToValidNestedIterator();
    }

    bool Done() const { return current_ == end_; }

    Eigen::Array3i GetCellIndex() const {
      DCHECK(!Done());
      const int outer_index = (1 << (3 * bits_)) - (end_ - current_);
      const Eigen::Array3i shifted_index =
          To3DIndex(outer_index, bits_) * WrappedGrid::grid_size() +
          nested_iterator_.GetCellIndex();
      return shifted_index - ((1 << (bits_ - 1)) * WrappedGrid::grid_size());
    }

    const ValueType& GetValue() const {
      DCHECK(!Done());
      return nested_iterator_.GetValue();
    }

    void AdvanceToEnd() { current_ = end_; }

    std::pair<Eigen::Array3i, ValueType> operator*() const {
      return std::pair<Eigen::Array3i, ValueType>(GetCellIndex(), GetValue());
    }

    Iterator& operator++() {
      Next();
      return *this;
    }

    bool operator!=(const Iterator& it) const {
      return it.current_ != current_;
    }

   private:
    void AdvanceToValidNestedIterator() {
      for (; !Done(); ++current_) {
        if (*current_ != nullptr) {
          nested_iterator_ = typename WrappedGrid::Iterator(**current_);
          if (!nested_iterator_.Done()) {
            break;
          }
        }
      }
    }

    int bits_;
    const std::unique_ptr<WrappedGrid>* current_;
    const std::unique_ptr<WrappedGrid>* const end_;
    typename WrappedGrid::Iterator nested_iterator_;
  };

 private:
  // Returns the Eigen::Array3i (meta) index of the meta cell containing
  // 'index'.
  Eigen::Array3i GetMetaIndex(const Eigen::Array3i& index) const {
    DCHECK((index >= 0).all()) << index;
    const Eigen::Array3i meta_index = index / WrappedGrid::grid_size();
    DCHECK((meta_index < (1 << bits_)).all()) << index;
    return meta_index;
  }

  // Grows this grid by a factor of 2 in each of the 3 dimensions.
  void Grow() {
    const int new_bits = bits_ + 1;
    CHECK_LE(new_bits, 8);
    std::vector<std::unique_ptr<WrappedGrid>> new_meta_cells_(
        8 * meta_cells_.size());
    for (int z = 0; z != (1 << bits_); ++z) {
      for (int y = 0; y != (1 << bits_); ++y) {
        for (int x = 0; x != (1 << bits_); ++x) {
          const Eigen::Array3i original_meta_index(x, y, z);
          const Eigen::Array3i new_meta_index =
              original_meta_index + (1 << (bits_ - 1));
          new_meta_cells_[ToFlatIndex(new_meta_index, new_bits)] =
              std::move(meta_cells_[ToFlatIndex(original_meta_index, bits_)]);
        }
      }
    }
    meta_cells_ = std::move(new_meta_cells_);
    bits_ = new_bits;
  }

  int bits_;
  std::vector<std::unique_ptr<WrappedGrid>> meta_cells_;
};

template <typename ValueType>
using GridBase = DynamicGrid<NestedGrid<FlatGrid<ValueType, 3>, 3>>;

// Represents a 3D grid as a wide, shallow tree.
template <typename ValueType>
class HybridGridBase : public GridBase<ValueType> {
 public:
  using Iterator = typename GridBase<ValueType>::Iterator;

  // Creates a new tree-based probability grid with voxels having edge length
  // 'resolution' around the origin which becomes the center of the cell at
  // index (0, 0, 0).
  explicit HybridGridBase(const float resolution) : resolution_(resolution) {}

  float resolution() const { return resolution_; }

  // Returns the index of the cell containing the 'point'. Indices are integer
  // vectors identifying cells, for this the coordinates are rounded to the next
  // multiple of the resolution.
  Eigen::Array3i GetCellIndex(const Eigen::Vector3f& point) const {
    Eigen::Array3f index = point.array() / resolution_;
    return Eigen::Array3i(RoundToInt(index.x()), RoundToInt(index.y()),
                          RoundToInt(index.z()));
  }

  // Returns one of the octants, (0, 0, 0), (1, 0, 0), ..., (1, 1, 1).
  static Eigen::Array3i GetOctant(const int i) {
    DCHECK_GE(i, 0);
    DCHECK_LT(i, 8);
    return {static_cast<bool>(i & 1), static_cast<bool>(i & 2),
            static_cast<bool>(i & 4)};
  }

  // Returns the center of the cell at 'index'.
  Eigen::Vector3f GetCenterOfCell(const Eigen::Array3i& index) const {
    return index.matrix().cast<float>() * resolution_;
  }

  // Iterator functions for range-based for loops.
  Iterator begin() const { return Iterator(*this); }

  Iterator end() const {
    Iterator it(*this);
    it.AdvanceToEnd();
    return it;
  }

 private:
  static int RoundToInt(double n) { return std::lround(n); }

 private:
  // Edge length of each voxel.
  const float resolution_;
};

// Points are expected to be close to the origin. Points far from the origin
// require the grid to grow dynamically. For centimeter resolution, points
// can only be tens of meters from the origin.
// The hard limit of cell indexes is +/- 8192 around the origin.
class HybridGridImpl : public HybridGridBase<PointCloudPtr> {
 public:
  explicit HybridGridImpl(const float resolution)
      : HybridGridBase<PointCloudPtr>(resolution) {}

  PointCloudPtr GetSurroundedCloud(const PointCloudPtr& scan,
                                   const Rigid3d& pose) {
    boost::unordered_set<PointCloudPtr> inserted_grids;
    for (auto& point : *scan) {
      if (point.getVector3fMap().norm() > kDist) continue;
      auto new_point = pose.cast<float>() * point.getVector3fMap();
      // TODO
      // Need to get more points!
      auto cloud_in_grid = this->value(GetCellIndex(new_point));
      if (cloud_in_grid) {
        inserted_grids.insert(cloud_in_grid);
      }
    }
    PointCloudPtr cloud_surround(new PointCloud);
    for (auto& cloud_in_grid : inserted_grids) {
      *cloud_surround += *cloud_in_grid;
    }
    return cloud_surround;
  }

  void InsertScan(const PointCloudPtr& scan, pcl::Filter<PointType>& filter) {
    if (scan->empty()) return;
    // 添加scan到点云
    for (auto& point : *scan) {
      PointCloudPtr& cloud_in_grid =
          *this->mutable_value(GetCellIndex(point.getArray3fMap()));
      if (!cloud_in_grid) cloud_in_grid.reset(new PointCloud);
      cloud_in_grid->push_back(point);
    }
    // 降采样
    boost::unordered_set<PointCloudPtr> inserted_grids;
    for (auto& point : *scan) {
      auto cloud_in_grid = this->value(GetCellIndex(point.getArray3fMap()));
      if (cloud_in_grid) inserted_grids.insert(cloud_in_grid);
    }
    for (auto& cloud_in_grid : inserted_grids) {
      filter.setInputCloud(cloud_in_grid);
      filter.filter(*cloud_in_grid);
    }
  }

 private:
  const double kDist = 100.0;
};

HybridGrid::HybridGrid(const float& resolution)
    : hybrid_grid_(new HybridGridImpl(resolution)) {}

HybridGrid::~HybridGrid() { delete hybrid_grid_; }

PointCloudPtr HybridGrid::GetSurroundedCloud(const PointCloudPtr& scan,
                                             const Rigid3d& pose) {
  return hybrid_grid_->GetSurroundedCloud(scan, pose);
}

void HybridGrid::InsertScan(const PointCloudPtr& scan,
                            pcl::Filter<PointType>& filter) {
  hybrid_grid_->InsertScan(scan, filter);
}
