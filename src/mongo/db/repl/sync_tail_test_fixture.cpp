/**
 *    Copyright 2017 (C) MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/sync_tail_test_fixture.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"

namespace mongo {
namespace repl {

void SyncTailOpObserver::onInserts(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   OptionalCollectionUUID uuid,
                                   std::vector<InsertStatement>::const_iterator begin,
                                   std::vector<InsertStatement>::const_iterator end,
                                   bool fromMigrate) {
    if (!onInsertsFn) {
        return;
    }
    std::vector<BSONObj> docs;
    for (auto it = begin; it != end; ++it) {
        const InsertStatement& insertStatement = *it;
        docs.push_back(insertStatement.doc.getOwned());
    }
    onInsertsFn(opCtx, nss, docs);
}

void SyncTailOpObserver::onDelete(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  OptionalCollectionUUID uuid,
                                  StmtId stmtId,
                                  bool fromMigrate,
                                  const boost::optional<BSONObj>& deletedDoc) {
    if (!onDeleteFn) {
        return;
    }
    onDeleteFn(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
}

void SyncTailOpObserver::onCreateCollection(OperationContext* opCtx,
                                            Collection* coll,
                                            const NamespaceString& collectionName,
                                            const CollectionOptions& options,
                                            const BSONObj& idIndex) {
    if (!onCreateCollectionFn) {
        return;
    }
    onCreateCollectionFn(opCtx, coll, collectionName, options, idIndex);
}

void SyncTailTest::setUp() {
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    ReplicationCoordinator::set(service, stdx::make_unique<ReplicationCoordinatorMock>(service));
    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

    StorageInterface::set(service, stdx::make_unique<StorageInterfaceImpl>());
    auto storageInterface = StorageInterface::get(service);

    DropPendingCollectionReaper::set(
        service, stdx::make_unique<DropPendingCollectionReaper>(storageInterface));
    repl::setOplogCollectionName(service);
    repl::createOplog(_opCtx.get());

    _replicationProcess =
        new ReplicationProcess(storageInterface,
                               stdx::make_unique<ReplicationConsistencyMarkersMock>(),
                               stdx::make_unique<ReplicationRecoveryMock>());
    ReplicationProcess::set(cc().getServiceContext(),
                            std::unique_ptr<ReplicationProcess>(_replicationProcess));

    // Set up an OpObserver to track the documents SyncTail inserts.
    auto opObserver = std::make_unique<SyncTailOpObserver>();
    _opObserver = opObserver.get();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));

    _opsApplied = 0;
    _applyOp = [](OperationContext* opCtx,
                  Database* db,
                  const BSONObj& op,
                  bool alwaysUpsert,
                  OplogApplication::Mode oplogApplicationMode,
                  stdx::function<void()>) { return Status::OK(); };
    _applyCmd = [](OperationContext* opCtx,
                   const BSONObj& op,
                   OplogApplication::Mode oplogApplicationMode) { return Status::OK(); };
    _incOps = [this]() { _opsApplied++; };

    // Initialize the featureCompatibilityVersion server parameter. This is necessary because this
    // test fixture does not create a featureCompatibilityVersion document from which to initialize
    // the server parameter.
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40);
}

void SyncTailTest::tearDown() {
    auto service = getServiceContext();
    _opCtx.reset();
    ReplicationProcess::set(service, {});
    DropPendingCollectionReaper::set(service, {});
    StorageInterface::set(service, {});
    ServiceContextMongoDTest::tearDown();
}

void SyncTailTest::_testSyncApplyCrudOperation(ErrorCodes::Error expectedError,
                                               const BSONObj& op,
                                               bool expectedApplyOpCalled) {
    bool applyOpCalled = false;
    SyncTail::ApplyOperationInLockFn applyOp = [&](OperationContext* opCtx,
                                                   Database* db,
                                                   const BSONObj& theOperation,
                                                   bool alwaysUpsert,
                                                   OplogApplication::Mode oplogApplicationMode,
                                                   stdx::function<void()>) {
        applyOpCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode("test", MODE_IX));
        ASSERT_FALSE(opCtx->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode("test.t", MODE_IX));
        ASSERT_FALSE(opCtx->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(opCtx));
        ASSERT_TRUE(db);
        ASSERT_BSONOBJ_EQ(op, theOperation);
        ASSERT_TRUE(alwaysUpsert);
        ASSERT_EQUALS(oplogApplicationMode, OplogApplication::Mode::kSecondary);
        return Status::OK();
    };
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_opCtx.get()));
    ASSERT_EQ(SyncTail::syncApply(_opCtx.get(),
                                  op,
                                  OplogApplication::Mode::kSecondary,
                                  applyOp,
                                  failedApplyCommand,
                                  _incOps),
              expectedError);
    ASSERT_EQ(applyOpCalled, expectedApplyOpCalled);
}

void SyncTailTest::_testSyncApplyInsertDocument(ErrorCodes::Error expectedError) {
    _testSyncApplyCrudOperation(expectedError,
                                BSON("op"
                                     << "i"
                                     << "ns"
                                     << "test.t"),
                                expectedError == ErrorCodes::OK);
}

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode) {
    FAIL("applyCommand unexpectedly invoked.");
    return Status::OK();
}

Status SyncTailTest::runOpSteadyState(const OplogEntry& op) {
    return runOpsSteadyState({op});
}

Status SyncTailTest::runOpsSteadyState(std::vector<OplogEntry> ops) {
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    MultiApplier::OperationPtrs opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    WorkerMultikeyPathInfo pathInfo;
    return multiSyncApply(_opCtx.get(), &opsPtrs, &syncTail, &pathInfo);
}

Status SyncTailTest::runOpInitialSync(const OplogEntry& op) {
    return runOpsInitialSync({op});
}

Status SyncTailTest::runOpsInitialSync(std::vector<OplogEntry> ops) {
    SyncTail syncTail(nullptr, SyncTail::MultiSyncApplyFunc(), nullptr);
    MultiApplier::OperationPtrs opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    AtomicUInt32 fetchCount(0);
    WorkerMultikeyPathInfo pathInfo;
    return multiInitialSyncApply(_opCtx.get(), &opsPtrs, &syncTail, &fetchCount, &pathInfo);
}


}  // namespace repl
}  // namespace mongo
