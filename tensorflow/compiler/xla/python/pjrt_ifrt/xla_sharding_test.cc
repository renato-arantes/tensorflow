/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/python/pjrt_ifrt/xla_sharding.h"

#include <memory>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/status_matchers.h"

namespace xla {
namespace ifrt {
namespace {

using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::HasSubstr;
using ::testing::SizeIs;
using ::tsl::testing::StatusIs;

DeviceList CreateDummyDevices(int count) {
  DeviceList::Devices devices;
  devices.reserve(count);
  for (int i = 0; i < count; ++i) {
    devices.push_back(reinterpret_cast<Device*>(i + 1));
  }
  return DeviceList(std::move(devices));
}

TEST(HloShardingTest, IndexDomainsWithReplication) {
  auto device_list = CreateDummyDevices(2);
  // Fully replicated.
  auto xla_hlo_sharding = xla::HloSharding::Replicate();
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(shape), IndexDomain(shape)));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithReplication) {
  auto device_list = CreateDummyDevices(2);
  // Fully replicated.
  auto xla_hlo_sharding = xla::HloSharding::Replicate();
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({10, 20}));
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, IndexDomainsWithTile) {
  auto device_list = CreateDummyDevices(2);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithTile) {
  auto device_list = CreateDummyDevices(2);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, IndexDomainsWithUnevenTile) {
  auto device_list = CreateDummyDevices(2);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({11, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({6, 20})),
                          IndexDomain(Index({6, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithUnevenTile) {
  auto device_list = CreateDummyDevices(2);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({11, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(2));
  for (int i = 0; i < 2; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    if (i == 0) {
      EXPECT_EQ(shape, Shape({6, 20}));
    } else {
      EXPECT_EQ(shape, Shape({5, 20}));
    }
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, IndexDomainsWithPartialTile) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding =
      xla::HloSharding::PartialTile(xla::TileAssignment({2, 1, 3}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithPartialTile) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding =
      xla::HloSharding::PartialTile(xla::TileAssignment({2, 1, 3}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, IndexDomainsWithSubgroupReplicated) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::REPLICATED});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithSubgroupReplicated) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // replicated by 3 times.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::REPLICATED});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, IndexDomainsWithSubgroupMaximalSlowPath) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // maximal-replicated by 3 times, device#0 in each replication is maximal.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::MAXIMAL});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto index_domains, sharding->IndexDomains(shape));

  EXPECT_THAT(index_domains,
              ElementsAre(IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({0, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20})),
                          IndexDomain(Index({5, 0}), Shape({5, 20}))));
  EXPECT_THAT(
      index_domains,
      ElementsAreArray(TEST_HloShardingIndexDomainsSlowPath(*sharding, shape)));
}

TEST(HloShardingTest, DisassembleWithSubgroupMaximalSlowPath) {
  auto device_list = CreateDummyDevices(6);
  // 2-way sharded along axis 0, 1-way sharded along axis 1, each shard
  // maximal-replicated by 3 times, device#0 in each replication is maximal.
  auto xla_hlo_sharding = xla::HloSharding::Subgroup(
      xla::TileAssignment({2, 1, 3}), {xla::OpSharding::MAXIMAL});
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  TF_ASSERT_OK_AND_ASSIGN(auto disassembled, sharding->Disassemble(shape));

  ASSERT_THAT(disassembled, SizeIs(6));
  for (int i = 0; i < 6; ++i) {
    const auto& [shape, sharding] = disassembled[i];
    EXPECT_EQ(shape, Shape({5, 20}));
    EXPECT_TRUE(llvm::isa<SingleDeviceSharding>(*sharding));
    EXPECT_THAT(sharding->devices().devices(),
                ElementsAre(device_list.devices()[i]));
  }
}

TEST(HloShardingTest, DisassembleFailsWithInvalidDeviceCount) {
  auto device_list = CreateDummyDevices(1);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10, 20});
  EXPECT_THAT(sharding->Disassemble(shape),
              StatusIs(tsl::error::INVALID_ARGUMENT,
                       HasSubstr("sharding's tile_assignment_devices and "
                                 "device count does not match: 2 vs. 1")));
}

TEST(HloShardingTest, DisassembleFailsWithMismatchingShapeDimsSize) {
  auto device_list = CreateDummyDevices(2);
  // 2-way sharded along axis 0, 1-way sharded along axis 1.
  auto xla_hlo_sharding = xla::HloSharding::Tile(xla::TileAssignment({2, 1}));
  std::shared_ptr<const HloSharding> sharding =
      HloSharding::Create(device_list, xla_hlo_sharding);

  Shape shape({10});
  EXPECT_THAT(
      sharding->Disassemble(shape),
      StatusIs(
          tsl::error::INVALID_ARGUMENT,
          HasSubstr("shape must have 2 dimensions, but has 1 dimensions")));
}

}  // namespace
}  // namespace ifrt
}  // namespace xla
