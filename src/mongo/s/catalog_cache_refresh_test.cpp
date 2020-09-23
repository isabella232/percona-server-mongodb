/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/query/query_request.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

class CatalogCacheRefreshTest : public CatalogCacheTestFixture {
protected:
    void setUp() override {
        CatalogCacheTestFixture::setUp();

        setupNShards(2);
    }

    void expectGetDatabase() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(kNss.db().toString(), {"0"}, true, databaseVersion::makeNew());
            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollection(OID epoch, const ShardKeyPattern& shardKeyPattern) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            CollectionType collType;
            collType.setNs(kNss);
            collType.setEpoch(epoch);
            collType.setKeyPattern(shardKeyPattern.toBSON());
            collType.setUnique(false);

            return std::vector<BSONObj>{collType.toBSON()};
        }());
    }
};

TEST_F(CatalogCacheRefreshTest, FullLoad) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                         version,
                         {"0"});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{chunk1.toConfigBSON(),
                                    chunk2.toConfigBSON(),
                                    chunk3.toConfigBSON(),
                                    chunk4.toConfigBSON()};
    }());

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(4, cm->numChunks());
}

TEST_F(CatalogCacheRefreshTest, NoLoadIfShardNotMarkedStaleInOperationContext) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}));
    ASSERT_EQ(2, initialRoutingInfo->numChunks());

    auto futureNoRefresh = scheduleRoutingInfoUnforcedRefresh(kNss);
    auto routingInfo = futureNoRefresh.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();
    ASSERT_EQ(2, cm->numChunks());
}

class MockLockerAlwaysReportsToBeLocked : public LockerNoop {
public:
    using LockerNoop::LockerNoop;

    bool isLocked() const final {
        return true;
    }
};

DEATH_TEST_F(CatalogCacheRefreshTest, ShouldFailToRefreshWhenLocksAreHeld, "Invariant") {
    operationContext()->setLockState(std::make_unique<MockLockerAlwaysReportsToBeLocked>());
    scheduleRoutingInfoUnforcedRefresh(kNss);
}

TEST_F(CatalogCacheRefreshTest, DatabaseNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return an empty database (need to return it twice because for missing databases, the
    // CatalogClient tries twice)
    expectFindSendBSONObjVector(kConfigHostAndPort, {});
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning no database did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NamespaceNotFound, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, DatabaseBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return a corrupted database entry
    expectFindSendBSONObjVector(kConfigHostAndPort,
                                {BSON(
                                    "BadValue"
                                    << "This value should not be in a database config document")});

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning corrupted database entry did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, CollectionNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    auto routingInfo = future.default_timed_get();
    ASSERT(!routingInfo->cm());
    ASSERT(routingInfo->db().primary());
    ASSERT_EQ(ShardId{"0"}, routingInfo->db().primaryId());
}

TEST_F(CatalogCacheRefreshTest, CollectionBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return a corrupted collection entry
    expectFindSendBSONObjVector(
        kConfigHostAndPort,
        {BSON("BadValue"
              << "This value should not be in a collection config document")});

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning corrupted collection entry did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadNoChunksFound) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadNoChunksFound) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo->getVersion().epoch();

    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunksBSONCorrupted) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        return std::vector<BSONObj>{ChunkType(kNss,
                                              {shardKeyPattern.getKeyPattern().globalMin(),
                                               BSON("_id" << 0)},
                                              ChunkVersion(1, 0, epoch),
                                              {"0"})
                                        .toConfigBSON(),
                                    BSON("BadValue"
                                         << "This value should not be in a chunk config document")};
    }());

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithLowestVersion) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{
            chunk2.toConfigBSON(), chunk3.toConfigBSON(), chunk4.toConfigBSON()};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithHighestVersion) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{
            chunk2.toConfigBSON(), chunk3.toConfigBSON(), chunk4.toConfigBSON()};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithLowestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo->getVersion().epoch();

    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{
            chunk2.toConfigBSON(), chunk3.toConfigBSON(), chunk4.toConfigBSON()};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithHighestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    const OID epoch = initialRoutingInfo->getVersion().epoch();

    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<BSONObj>{
            chunk2.toConfigBSON(), chunk3.toConfigBSON(), chunk4.toConfigBSON()};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, incompleteChunks);

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunkEpochChangeDuringIncrementalLoad) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    ChunkVersion version = initialRoutingInfo->getVersion();

    const auto inconsistentChunks = [&]() {
        version.incMajor();
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        chunk1.setName(OID::gen());

        ChunkType chunk2(kNss,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         ChunkVersion(1, 0, OID::gen()),
                         {"1"});
        chunk2.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }();

    // Return set of chunks, one of which has different epoch. Do it three times, which is how
    // frequently the catalog cache retries.
    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, inconsistentChunks);
    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, inconsistentChunks);

    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindSendBSONObjVector(kConfigHostAndPort, inconsistentChunks);

    try {
        auto routingInfo = future.default_timed_get();
        auto cm = routingInfo->cm();
        auto primary = routingInfo->db().primary();

        FAIL(str::stream()
             << "Returning chunks with different epoch for collection did not fail and returned "
             << (cm ? cm->toString() : routingInfo->db().primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunkEpochChangeDuringIncrementalLoadRecoveryAfterRetry) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    setupNShards(2);

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    ChunkVersion oldVersion = initialRoutingInfo->getVersion();
    const OID newEpoch = OID::gen();

    // On the first attempt, return set of chunks, one of which has different epoch. This simulates
    // the situation where a collection existed with epoch0, we started a refresh for that
    // collection, the cursor yielded and while it yielded another node dropped the collection and
    // recreated it with different epoch and chunks.
    expectGetCollection(oldVersion.epoch(), shardKeyPattern);
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto diffQuery =
            assertGet(QueryRequest::makeFromFindCommand(kNss, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("ns" << kNss.ns() << "lastmod"
                                    << BSON("$gte" << Timestamp(oldVersion.majorVersion(),
                                                                oldVersion.minorVersion()))),
                          diffQuery->getFilter());

        oldVersion.incMajor();
        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         oldVersion,
                         {"0"});
        chunk1.setName(OID::gen());

        // "Yield" happens here with drop and recreate in between. This is the "last" chunk from the
        // recreated collection.
        ChunkType chunk3(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         ChunkVersion(5, 2, newEpoch),
                         {"1"});
        chunk3.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk3.toConfigBSON()};
    });

    // On the second retry attempt, return the correct set of chunks from the recreated collection
    expectGetCollection(newEpoch, shardKeyPattern);

    ChunkVersion newVersion(5, 0, newEpoch);
    onFindCommand([&](const RemoteCommandRequest& request) {
        // Ensure it is a differential query but starting from version zero (to fetch all the
        // chunks) since the incremental refresh above produced a different version
        const auto diffQuery =
            assertGet(QueryRequest::makeFromFindCommand(kNss, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("ns" << kNss.ns() << "lastmod" << BSON("$gte" << Timestamp(0, 0))),
                          diffQuery->getFilter());

        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         newVersion,
                         {"0"});
        chunk1.setName(OID::gen());

        newVersion.incMinor();
        ChunkType chunk2(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, newVersion, {"0"});
        chunk2.setName(OID::gen());

        newVersion.incMinor();
        ChunkType chunk3(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         newVersion,
                         {"1"});
        chunk3.setName(OID::gen());

        return std::vector<BSONObj>{
            chunk1.toConfigBSON(), chunk2.toConfigBSON(), chunk3.toConfigBSON()};
    });

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(3, cm->numChunks());
    ASSERT_EQ(newVersion, cm->getVersion());
    ASSERT_EQ(ChunkVersion(5, 1, newVersion.epoch()), cm->getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(5, 2, newVersion.epoch()), cm->getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterCollectionEpochChange) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    setupNShards(2);

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    ChunkVersion newVersion(1, 0, OID::gen());

    // Return collection with a different epoch
    expectGetCollection(newVersion.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        // Ensure it is a differential query but starting from version zero
        const auto diffQuery =
            assertGet(QueryRequest::makeFromFindCommand(kNss, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(BSON("ns" << kNss.ns() << "lastmod" << BSON("$gte" << Timestamp(0, 0))),
                          diffQuery->getFilter());

        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         newVersion,
                         {"0"});
        chunk1.setName(OID::gen());
        newVersion.incMinor();

        ChunkType chunk2(kNss,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         newVersion,
                         {"1"});
        chunk2.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    });

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(2, cm->numChunks());
    ASSERT_EQ(newVersion, cm->getVersion());
    ASSERT_EQ(ChunkVersion(1, 0, newVersion.epoch()), cm->getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(1, 1, newVersion.epoch()), cm->getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterSplit) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        // Ensure it is a differential query
        const auto diffQuery =
            assertGet(QueryRequest::makeFromFindCommand(kNss, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(
            BSON("ns" << kNss.ns() << "lastmod"
                      << BSON("$gte" << Timestamp(version.majorVersion(), version.minorVersion()))),
            diffQuery->getFilter());

        version.incMajor();
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        chunk1.setName(OID::gen());

        version.incMinor();
        ChunkType chunk2(
            kNss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});
        chunk2.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    });

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(2, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(version, cm->getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch()), cm->getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMove) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}));
    ASSERT_EQ(2, initialRoutingInfo->numChunks());

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    ChunkVersion expectedDestShardVersion;

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a move
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        version.incMajor();
        expectedDestShardVersion = version;
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"1"});
        chunk1.setName(OID::gen());

        version.incMinor();
        ChunkType chunk2(
            kNss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});
        chunk2.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }());

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(2, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(version, cm->getVersion({"0"}));
    ASSERT_EQ(expectedDestShardVersion, cm->getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMoveLastChunk) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    setupNShards(2);

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a move
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        version.incMajor();
        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(),
                          shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk1.setName(OID::gen());

        return std::vector<BSONObj>{chunk1.toConfigBSON()};
    }());

    auto routingInfo = future.default_timed_get();
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(1, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch()), cm->getVersion({"0"}));
    ASSERT_EQ(version, cm->getVersion({"1"}));
}

}  // namespace
}  // namespace mongo
