/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "base/NebulaKeyUtils.h"
#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include "fs/TempDir.h"
#include "storage/test/TestUtils.h"
#include "storage/AddEdgesProcessor.h"

namespace nebula {
namespace storage {

TEST(AddEdgesTest, SimpleTest) {
    fs::TempDir rootPath("/tmp/AddEdgesTest.XXXXXX");
    std::unique_ptr<kvstore::KVStore> kv = TestUtils::initKV(rootPath.path());
    auto* schemaMan = new AdHocSchemaManager();
    auto* processor = AddEdgesProcessor::instance(kv.get(), schemaMan);

    LOG(INFO) << "Build AddEdgesRequest...";
    cpp2::AddEdgesRequest req;
    req.space_id = 0;
    req.overwritable = true;
    // partId => List<Edge>
    // Edge => {EdgeKey, props}
    for (auto partId = 0; partId < 3; partId++) {
        std::vector<cpp2::Edge> edges;
        for (auto srcId = partId * 10; srcId < 10 * (partId + 1); srcId++) {
            edges.emplace_back(apache::thrift::FragileConstructor::FRAGILE,
                               cpp2::EdgeKey(apache::thrift::FragileConstructor::FRAGILE,
                                             srcId, srcId*100 + 1, srcId*100 + 2, srcId*100 + 3),
                               folly::stringPrintf("%d_%d", partId, srcId));
        }
        req.parts.emplace(partId, std::move(edges));
    }

    LOG(INFO) << "Test AddEdgesProcessor...";
    auto fut = processor->getFuture();
    processor->process(req);
    auto resp = std::move(fut).get();
    EXPECT_EQ(0, resp.result.failed_codes.size());

    LOG(INFO) << "Check data in kv store...";
    for (auto partId = 0; partId < 3; partId++) {
        for (auto srcId = 10 * partId; srcId < 10 * (partId + 1); srcId++) {
            auto prefix = NebulaKeyUtils::edgePrefix(partId, srcId, srcId*100 + 1);
            std::unique_ptr<kvstore::KVIterator> iter;
            EXPECT_EQ(kvstore::ResultCode::SUCCEEDED, kv->prefix(0, partId, prefix, &iter));
            int num = 0;
            while (iter->valid()) {
                EXPECT_EQ(folly::stringPrintf("%d_%d", partId, srcId), iter->val());
                num++;
                iter->next();
            }
            EXPECT_EQ(1, num);
        }
    }
}


TEST(AddEdgesTest, VersionTest) {
    fs::TempDir rootPath("/tmp/AddEdgesTest.XXXXXX");
    std::unique_ptr<kvstore::KVStore> kv = TestUtils::initKV(rootPath.path());

    auto addEdges = [&](nebula::meta::SchemaManager* schemaMan, PartitionID partId,
                        VertexID srcId) {
        for (auto version = 1; version <= 10000; version++) {
            auto* processor = AddEdgesProcessor::instance(kv.get(), schemaMan);
            cpp2::AddEdgesRequest req;
            req.space_id = 0;
            req.overwritable = false;

            std::vector<cpp2::Edge> edges;
            edges.emplace_back(apache::thrift::FragileConstructor::FRAGILE,
                               cpp2::EdgeKey(apache::thrift::FragileConstructor::FRAGILE,
                                             srcId, srcId*100 + 1, srcId*100 + 2, srcId*100 + 3),
                               folly::stringPrintf("%d_%ld_%d", partId, srcId, version));
            req.parts.emplace(partId, std::move(edges));

            auto fut = processor->getFuture();
            processor->process(req);
            auto resp = std::move(fut).get();
            EXPECT_EQ(0, resp.result.failed_codes.size());
        }
    };

    auto checkEdgeByPrefix = [&](PartitionID partId, VertexID vertexId,
                                 EdgeType edge, int32_t startValue, int32_t expectedNum) {
        auto prefix = NebulaKeyUtils::prefix(partId, vertexId, edge);
        std::unique_ptr<kvstore::KVIterator> iter;
        EXPECT_EQ(kvstore::ResultCode::SUCCEEDED, kv->prefix(0, partId, prefix, &iter));
        int num = 0;
        while (iter->valid()) {
            EXPECT_EQ(folly::stringPrintf("%d_%ld_%d", partId, vertexId, startValue - num),
                      iter->val());
            num++;
            iter->next();
        }
        EXPECT_EQ(expectedNum, num);
    };

    auto checkEdgeByRange = [&](PartitionID partId, VertexID vertexId, EdgeType edge,
                                EdgeRanking rank, VertexID dstId, int32_t startValue,
                                int32_t expectedNum) {
        auto start = NebulaKeyUtils::edgeKey(partId,
                                             vertexId,
                                             edge,
                                             rank,
                                             dstId,
                                             0);
        auto end = NebulaKeyUtils::edgeKey(partId,
                                           vertexId,
                                           edge,
                                           rank,
                                           dstId,
                                           std::numeric_limits<int64_t>::max());
        std::unique_ptr<kvstore::KVIterator> iter;
        ASSERT_EQ(kvstore::ResultCode::SUCCEEDED, kv->range(0, partId, start, end, &iter));
        int32_t num = 0;
        while (iter->valid()) {
            EXPECT_EQ(folly::stringPrintf("%d_%ld_%d", partId, vertexId, startValue - num),
                      iter->val());
            num++;
            iter->next();
        }
        ASSERT_EQ(expectedNum, num);
    };

    {
        PartitionID partitionId = 0;
        VertexID srcId = 100;
        auto* schemaMan = new AdHocSchemaManager();
        schemaMan->setSpaceTimeSeries(0, true);

        LOG(INFO) << "Add edges with multiple versions...";
        addEdges(schemaMan, partitionId, srcId);
        LOG(INFO) << "Check data in kv store...";
        checkEdgeByPrefix(partitionId, srcId, srcId*100 + 1, 10000, 10000);
        checkEdgeByRange(partitionId, srcId, srcId*100 + 1, srcId*100 + 3,
                         srcId*100 + 2, 10000, 10000);
    }

    {
        PartitionID partitionId = 0;
        VertexID srcId = 101;
        auto* schemaMan = new AdHocSchemaManager();
        schemaMan->setSpaceTimeSeries(0, false);

        LOG(INFO) << "Add edges with only one version...";
        addEdges(schemaMan, partitionId, srcId);
        LOG(INFO) << "Check data in kv store...";
        checkEdgeByPrefix(partitionId, srcId, srcId*100 + 1, 10000, 1);
        checkEdgeByRange(partitionId, srcId, srcId*100 + 1, srcId*100 + 3, srcId*100 + 2, 10000, 1);
    }
}

}  // namespace storage
}  // namespace nebula


int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    folly::init(&argc, &argv, true);
    google::SetStderrLogging(google::INFO);
    return RUN_ALL_TESTS();
}


