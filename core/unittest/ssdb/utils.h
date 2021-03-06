// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#pragma once

#include <gtest/gtest.h>
#include <random>
#include <memory>
#include <tuple>
#include <vector>
#include <set>
#include <string>

#include "db/SSDBImpl.h"
#include "db/snapshot/CompoundOperations.h"
#include "db/snapshot/Context.h"
#include "db/snapshot/EventExecutor.h"
#include "db/snapshot/OperationExecutor.h"
#include "db/snapshot/ReferenceProxy.h"
#include "db/snapshot/ResourceHolders.h"
#include "db/snapshot/ScopedResource.h"
#include "db/snapshot/Snapshots.h"
#include "db/snapshot/Store.h"
#include "db/snapshot/WrappedTypes.h"

using ID_TYPE = milvus::engine::snapshot::ID_TYPE;
using IDS_TYPE = milvus::engine::snapshot::IDS_TYPE;
using LSN_TYPE = milvus::engine::snapshot::LSN_TYPE;
using MappingT = milvus::engine::snapshot::MappingT;
using LoadOperationContext = milvus::engine::snapshot::LoadOperationContext;
using CreateCollectionContext = milvus::engine::snapshot::CreateCollectionContext;
using SegmentFileContext = milvus::engine::snapshot::SegmentFileContext;
using OperationContext = milvus::engine::snapshot::OperationContext;
using PartitionContext = milvus::engine::snapshot::PartitionContext;
using DropIndexOperation = milvus::engine::snapshot::DropIndexOperation;
using DropAllIndexOperation = milvus::engine::snapshot::DropAllIndexOperation;
using BuildOperation = milvus::engine::snapshot::BuildOperation;
using MergeOperation = milvus::engine::snapshot::MergeOperation;
using CreateCollectionOperation = milvus::engine::snapshot::CreateCollectionOperation;
using NewSegmentOperation = milvus::engine::snapshot::NewSegmentOperation;
using DropPartitionOperation = milvus::engine::snapshot::DropPartitionOperation;
using CreatePartitionOperation = milvus::engine::snapshot::CreatePartitionOperation;
using DropCollectionOperation = milvus::engine::snapshot::DropCollectionOperation;
using CollectionCommitsHolder = milvus::engine::snapshot::CollectionCommitsHolder;
using CollectionsHolder = milvus::engine::snapshot::CollectionsHolder;
using CollectionScopedT = milvus::engine::snapshot::CollectionScopedT;
using Collection = milvus::engine::snapshot::Collection;
using CollectionPtr = milvus::engine::snapshot::CollectionPtr;
using Partition = milvus::engine::snapshot::Partition;
using PartitionPtr = milvus::engine::snapshot::PartitionPtr;
using Segment = milvus::engine::snapshot::Segment;
using SegmentPtr = milvus::engine::snapshot::SegmentPtr;
using SegmentFile = milvus::engine::snapshot::SegmentFile;
using SegmentFilePtr = milvus::engine::snapshot::SegmentFilePtr;
using Field = milvus::engine::snapshot::Field;
using FieldElement = milvus::engine::snapshot::FieldElement;
using Snapshots = milvus::engine::snapshot::Snapshots;
using ScopedSnapshotT = milvus::engine::snapshot::ScopedSnapshotT;
using ReferenceProxy = milvus::engine::snapshot::ReferenceProxy;
using Queue = milvus::BlockingQueue<ID_TYPE>;
using TQueue = milvus::BlockingQueue<std::tuple<ID_TYPE, ID_TYPE>>;
using SoftDeleteCollectionOperation = milvus::engine::snapshot::SoftDeleteOperation<Collection>;
using ParamsField = milvus::engine::snapshot::ParamsField;
using IteratePartitionHandler = milvus::engine::snapshot::IterateHandler<Partition>;
using IterateSegmentFileHandler = milvus::engine::snapshot::IterateHandler<SegmentFile>;
using SSDBImpl = milvus::engine::SSDBImpl;
using Status = milvus::Status;

inline int
RandomInt(int start, int end) {
    std::random_device dev;
    std::mt19937 rng(dev());
    std::uniform_int_distribution<std::mt19937::result_type> dist(start, end);
    return dist(rng);
}

inline void
SFContextBuilder(SegmentFileContext& ctx, ScopedSnapshotT sss) {
    auto field = sss->GetResources<Field>().begin()->second;
    ctx.field_name = field->GetName();
    for (auto& kv : sss->GetResources<FieldElement>()) {
        ctx.field_element_name = kv.second->GetName();
        break;
    }
    auto& segments =  sss->GetResources<Segment>();
    if (segments.size() == 0) {
        return;
    }

    ctx.segment_id = sss->GetResources<Segment>().begin()->second->GetID();
    ctx.partition_id = sss->GetResources<Segment>().begin()->second->GetPartitionId();
}

struct PartitionCollector : public IteratePartitionHandler {
    using ResourceT = Partition;
    using BaseT = IteratePartitionHandler;
    explicit PartitionCollector(ScopedSnapshotT ss) : BaseT(ss) {}

    Status
    PreIterate() override {
        partition_names_.clear();
        return Status::OK();
    }

    Status
    Handle(const typename ResourceT::Ptr& partition) override {
        partition_names_.push_back(partition->GetName());
        return Status::OK();
    }

    std::vector<std::string> partition_names_;
};

using FilterT = std::function<bool(SegmentFile::Ptr)>;
struct SegmentFileCollector : public IterateSegmentFileHandler {
    using ResourceT = SegmentFile;
    using BaseT = IterateSegmentFileHandler;
    explicit SegmentFileCollector(ScopedSnapshotT ss, const FilterT& filter)
        : filter_(filter), BaseT(ss) {}

    Status
    PreIterate() override {
        segment_files_.clear();
        return Status::OK();
    }

    Status
    Handle(const typename ResourceT::Ptr& segment_file) override {
        if (!filter_(segment_file)) {
            return Status::OK();
        }
        segment_files_.insert(segment_file->GetID());
        return Status::OK();
    }

    FilterT filter_;
    std::set<ID_TYPE> segment_files_;
};

struct WaitableObj {
    bool notified_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;

    void
    Wait() {
        std::unique_lock<std::mutex> lck(mutex_);
        if (!notified_) {
            cv_.wait(lck);
        }
        notified_ = false;
    }

    void
    Notify() {
        std::unique_lock<std::mutex> lck(mutex_);
        notified_ = true;
        lck.unlock();
        cv_.notify_one();
    }
};

inline ScopedSnapshotT
CreateCollection(const std::string& collection_name, const LSN_TYPE& lsn) {
    CreateCollectionContext context;
    context.lsn = lsn;
    auto collection_schema = std::make_shared<Collection>(collection_name);
    context.collection = collection_schema;
    auto vector_field = std::make_shared<Field>("vector", 0,
            milvus::engine::snapshot::FieldType::VECTOR);
    auto vector_field_element = std::make_shared<FieldElement>(0, 0, "ivfsq8",
            milvus::engine::snapshot::FieldElementType::IVFSQ8);
    auto int_field = std::make_shared<Field>("int", 0,
            milvus::engine::snapshot::FieldType::INT32);
    context.fields_schema[vector_field] = {vector_field_element};
    context.fields_schema[int_field] = {};

    auto op = std::make_shared<CreateCollectionOperation>(context);
    op->Push();
    ScopedSnapshotT ss;
    auto status = op->GetSnapshot(ss);
    return ss;
}

inline ScopedSnapshotT
CreatePartition(const std::string& collection_name, const PartitionContext& p_context, const LSN_TYPE& lsn) {
    ScopedSnapshotT curr_ss;
    ScopedSnapshotT ss;
    auto status = Snapshots::GetInstance().GetSnapshot(ss, collection_name);
    if (!status.ok()) {
        std::cout << status.ToString() << std::endl;
        return curr_ss;
    }

    OperationContext context;
    context.lsn = lsn;
    auto op = std::make_shared<CreatePartitionOperation>(context, ss);

    PartitionPtr partition;
    status = op->CommitNewPartition(p_context, partition);
    if (!status.ok()) {
        std::cout << status.ToString() << std::endl;
        return curr_ss;
    }

    status = op->Push();
    if (!status.ok()) {
        std::cout << status.ToString() << std::endl;
        return curr_ss;
    }

    status = op->GetSnapshot(curr_ss);
    if (!status.ok()) {
        std::cout << status.ToString() << std::endl;
        return curr_ss;
    }
    return curr_ss;
}

inline Status
CreateSegment(ScopedSnapshotT ss, ID_TYPE partition_id, LSN_TYPE lsn, const SegmentFileContext& sf_context) {
    OperationContext context;
    context.lsn = lsn;
    context.prev_partition = ss->GetResource<Partition>(partition_id);
    auto op = std::make_shared<NewSegmentOperation>(context, ss);
    SegmentPtr new_seg;
    STATUS_CHECK(op->CommitNewSegment(new_seg));
    SegmentFilePtr seg_file;
    auto nsf_context = sf_context;
    nsf_context.segment_id = new_seg->GetID();
    nsf_context.partition_id = new_seg->GetPartitionId();
    STATUS_CHECK(op->CommitNewSegmentFile(nsf_context, seg_file));
    STATUS_CHECK(op->Push());

    return op->GetSnapshot(ss);
}

class BaseTest : public ::testing::Test {
 protected:
    void
    InitLog();

    void
    SetUp() override;
    void
    TearDown() override;
};

class SnapshotTest : public BaseTest {
 protected:
    void
    SetUp() override;
    void
    TearDown() override;
};

class SSDBTest : public BaseTest {
 protected:
    std::shared_ptr<SSDBImpl> db_;

    void
    SetUp() override;
    void
    TearDown() override;
};
