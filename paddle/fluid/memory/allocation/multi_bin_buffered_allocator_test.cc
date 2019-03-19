// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/memory/allocation/multi_bin_buffered_allocator.h"
#include <gtest/gtest.h>
#include <utility>
#include <vector>
#include "paddle/fluid/memory/allocation/best_fit_allocator.h"
#include "paddle/fluid/memory/allocation/cpu_allocator.h"
#include "paddle/fluid/memory/allocation/locked_allocator.h"

namespace paddle {
namespace memory {
namespace allocation {

inline std::shared_ptr<MultiBinBufferedAllocator> GetBufferedAllocator(
    Allocation *allocation, bool thread_safe) {
  std::shared_ptr<Allocator> allocator(new BestFitAllocator(allocation));
  if (thread_safe) {
    allocator.reset(new LockedAllocator(std::move(allocator)));
  }

  return std::make_shared<MultiBinBufferedAllocator>(allocator);
}

TEST(buffered_allocator, thread_safety) {
  std::unique_ptr<CPUAllocator> allocator(new CPUAllocator());
  auto chunk = allocator->Allocate(1 << 20, allocator->kDefault);
  {
    auto buf_allocator = GetBufferedAllocator(chunk.get(), true);
    ASSERT_EQ(buf_allocator->IsAllocThreadSafe(), true);
  }

  {
    auto buf_allocator = GetBufferedAllocator(chunk.get(), false);
    ASSERT_EQ(buf_allocator->IsAllocThreadSafe(), false);
  }
}

class StubAllocation : public Allocation {
 public:
  using Allocation::Allocation;
};

class StubAllocator : public Allocator {
 public:
  void ResetCounter() {
    construct_count_ = 0;
    destruct_count_ = 0;
  }

  size_t GetAllocCount() const { return construct_count_; }

  size_t GetFreeCount() const { return destruct_count_; }

 protected:
  void FreeImpl(Allocation *allocation) override {
    auto *alloc = dynamic_cast<StubAllocation *>(allocation);
    PADDLE_ENFORCE_NOT_NULL(alloc);
    if (alloc->ptr()) delete[] static_cast<uint8_t *>(alloc->ptr());
    ++destruct_count_;
    delete allocation;
  }

  Allocation *AllocateImpl(size_t size, Allocator::Attr attr) override {
    ++construct_count_;
    if (size == 0) {
      return new StubAllocation(nullptr, 0, platform::CPUPlace());
    } else {
      return new StubAllocation(new uint8_t[size], size, platform::CPUPlace());
    }
  }

 private:
  size_t construct_count_ = 0;
  size_t destruct_count_ = 0;
};

constexpr size_t kZero = 0;
constexpr size_t kOne = 1;
constexpr size_t kTwo = 2;

TEST(buffered_allocator, lazy_free) {
  std::vector<int> original_alloc_size({1022, 1023, 1024, 1025, 1026});
  for (auto alloc_size : original_alloc_size) {
    auto stub_allocator = std::make_shared<StubAllocator>();
    auto *underlying_allocator = stub_allocator.get();
    auto allocator =
        std::make_shared<MultiBinBufferedAllocator>(stub_allocator);

    {
      underlying_allocator->ResetCounter();
      auto x = allocator->Allocate(alloc_size, allocator->kDefault);
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kOne);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
      x = nullptr;
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
    }

    {
      underlying_allocator->ResetCounter();
      auto x = allocator->Allocate(900, allocator->kDefault);
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kZero);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
      auto y = allocator->Allocate(2048, allocator->kDefault);
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kOne);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
      x = nullptr;
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
      y = nullptr;
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
    }

    {
      underlying_allocator->ResetCounter();
      size_t cache_size = allocator->ClearCache();
      ASSERT_EQ(cache_size, static_cast<size_t>(alloc_size + 2048));
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kZero);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kTwo);
    }

    {
      underlying_allocator->ResetCounter();
      auto p = allocator->Allocate(allocator->DivisionPlan().back(),
                                   allocator->kDefault);
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kOne);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
    }

    ASSERT_EQ(underlying_allocator->GetFreeCount(), kOne);

    {
      underlying_allocator->ResetCounter();
      auto p = allocator->Allocate(allocator->DivisionPlan().back() - 1,
                                   allocator->kDefault);
      ASSERT_EQ(underlying_allocator->GetAllocCount(), kOne);
      ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
    }

    ASSERT_EQ(underlying_allocator->GetFreeCount(), kZero);
  }
}

TEST(buffered_allocator, garbage_collection) {
  std::unique_ptr<CPUAllocator> cpu_allocator(new CPUAllocator());
  auto chunk = cpu_allocator->Allocate(2048, cpu_allocator->kDefault);
  auto allocator = GetBufferedAllocator(chunk.get(), false);
  auto x1 = allocator->Allocate(1600, allocator->kDefault);
  auto x2 = allocator->Allocate(400, allocator->kDefault);
  x1 = nullptr;
  x2 = nullptr;
  auto x3 = allocator->Allocate(1600, allocator->kDefault);
  ASSERT_NE(x3, nullptr);
  ASSERT_NE(x3->ptr(), nullptr);
}

}  // namespace allocation
}  // namespace memory
}  // namespace paddle
