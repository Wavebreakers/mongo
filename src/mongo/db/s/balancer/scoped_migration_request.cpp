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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/scoped_migration_request.h"

#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kWriteConcernTimeoutMigration);
const int kDuplicateKeyErrorMaxRetries = 2;

}  // namespace

ScopedMigrationRequest::ScopedMigrationRequest(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const BSONObj& minKey)
    : _opCtx(opCtx), _nss(nss), _minKey(minKey) {}

ScopedMigrationRequest::~ScopedMigrationRequest() {
    if (!_opCtx) {
        // If the opCtx object was cleared, nothing should happen in the destructor.
        return;
    }

    // Try to delete the entry in the config.migrations collection. If the command fails, that is
    // okay.
    BSONObj migrationDocumentIdentifier =
        BSON(MigrationType::ns(_nss.ns()) << MigrationType::min(_minKey));
    Status result = Grid::get(_opCtx)->catalogClient()->removeConfigDocuments(
        _opCtx, MigrationType::ConfigNS, migrationDocumentIdentifier, kMajorityWriteConcern);

    if (!result.isOK()) {
        LOGV2(21900,
              "Failed to remove config.migrations document for migration '{migration}': {error}",
              "Failed to remove config.migrations document for migration",
              "migration"_attr = migrationDocumentIdentifier,
              "error"_attr = redact(result));
    }
}

ScopedMigrationRequest::ScopedMigrationRequest(ScopedMigrationRequest&& other) {
    // This function relies on the move assigment to nullify 'other._opCtx'. If this is no longer
    // the case, this function should be updated to ensure that it nulls out 'other._opCtx'.
    *this = std::move(other);
}

ScopedMigrationRequest& ScopedMigrationRequest::operator=(ScopedMigrationRequest&& other) {
    if (this != &other) {
        _opCtx = other._opCtx;
        _nss = other._nss;
        _minKey = other._minKey;
        // Set opCtx to null so that the destructor will do nothing.
        other._opCtx = nullptr;
    }

    return *this;
}

StatusWith<ScopedMigrationRequest> ScopedMigrationRequest::writeMigration(
    OperationContext* opCtx, const MigrateInfo& migrateInfo, bool waitForDelete) {
    auto const grid = Grid::get(opCtx);

    const auto nssStatus = migrateInfo.getNss(opCtx);
    if (!nssStatus.isOK()) {
        return nssStatus.getStatus();
    }
    const auto nss = nssStatus.getValue();

    // Try to write a unique migration document to config.migrations.
    const MigrationType migrationType(nss, migrateInfo, waitForDelete);

    for (int retry = 0; retry < kDuplicateKeyErrorMaxRetries; ++retry) {
        Status result = grid->catalogClient()->insertConfigDocument(
            opCtx, MigrationType::ConfigNS, migrationType.toBSON(), kMajorityWriteConcern);

        if (result == ErrorCodes::DuplicateKey) {
            // If the exact migration described by "migrateInfo" is active, return a scoped object
            // for the request because this migration request will join the active one once
            // scheduled.
            auto statusWithMigrationQueryResult =
                grid->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    repl::ReadConcernLevel::kLocalReadConcern,
                    MigrationType::ConfigNS,
                    migrateInfo.getMigrationTypeQuery(nss),
                    BSONObj(),
                    boost::none);
            if (!statusWithMigrationQueryResult.isOK()) {
                return statusWithMigrationQueryResult.getStatus().withContext(
                    str::stream() << "Failed to verify whether conflicting migration is in "
                                  << "progress for migration '" << redact(migrateInfo.toString())
                                  << "' while trying to query config.migrations.");
            }
            if (statusWithMigrationQueryResult.getValue().docs.empty()) {
                // The document that caused the DuplicateKey error is no longer in the collection,
                // so retrying the insert might succeed.
                continue;
            }
            invariant(statusWithMigrationQueryResult.getValue().docs.size() == 1);

            BSONObj activeMigrationBSON = statusWithMigrationQueryResult.getValue().docs.front();
            auto statusWithActiveMigration = MigrationType::fromBSON(activeMigrationBSON);
            if (!statusWithActiveMigration.isOK()) {
                return statusWithActiveMigration.getStatus().withContext(
                    str::stream() << "Failed to verify whether conflicting migration is in "
                                  << "progress for migration '" << redact(migrateInfo.toString())
                                  << "' while trying to parse active migration document '"
                                  << redact(activeMigrationBSON.toString()) << "'.");
            }

            MigrateInfo activeMigrateInfo =
                statusWithActiveMigration.getValue().toMigrateInfo(opCtx);
            if (activeMigrateInfo.to != migrateInfo.to ||
                activeMigrateInfo.from != migrateInfo.from) {
                LOGV2(
                    21901,
                    "Failed to write document '{newMigration}' to config.migrations because there "
                    "is already an active migration for that chunk: "
                    "'{activeMigration}': {error}",
                    "Failed to write document to config.migrations because there "
                    "is already an active migration for that chunk",
                    "newMigration"_attr = redact(migrateInfo.toString()),
                    "activeMigration"_attr = redact(activeMigrateInfo.toString()),
                    "error"_attr = redact(result));
                return result;
            }

            result = Status::OK();
        }

        // As long as there isn't a DuplicateKey error, the document may have been written, and it's
        // safe (won't delete another migration's document) and necessary to try to clean up the
        // document via the destructor.
        ScopedMigrationRequest scopedMigrationRequest(opCtx, nss, migrateInfo.minKey);

        // If there was a write error, let the object go out of scope and clean up in the
        // destructor.
        if (!result.isOK()) {
            return result;
        }

        return std::move(scopedMigrationRequest);
    }

    return Status(ErrorCodes::OperationFailed,
                  str::stream() << "Failed to insert the config.migrations document after max "
                                << "number of retries. Chunk '"
                                << ChunkRange(migrateInfo.minKey, migrateInfo.maxKey).toString()
                                << "' in collection '" << nss
                                << "' was being moved (somewhere) by another operation.");
}

ScopedMigrationRequest ScopedMigrationRequest::createForRecovery(OperationContext* opCtx,
                                                                 const NamespaceString& nss,
                                                                 const BSONObj& minKey) {
    return ScopedMigrationRequest(opCtx, nss, minKey);
}

Status ScopedMigrationRequest::tryToRemoveMigration() {
    invariant(_opCtx);
    BSONObj migrationDocumentIdentifier =
        BSON(MigrationType::ns(_nss.ns()) << MigrationType::min(_minKey));
    Status status = Grid::get(_opCtx)->catalogClient()->removeConfigDocuments(
        _opCtx, MigrationType::ConfigNS, migrationDocumentIdentifier, kMajorityWriteConcern);
    if (status.isOK()) {
        // Don't try to do a no-op remove in the destructor.
        _opCtx = nullptr;
    }
    return status;
}

void ScopedMigrationRequest::keepDocumentOnDestruct() {
    invariant(_opCtx);
    _opCtx = nullptr;
    LOGV2_DEBUG(21902,
                1,
                "Keeping config.migrations document with namespace {namespace} and minKey "
                "{minKey} for balancer recovery",
                "Keeping config.migrations document for balancer recovery",
                "namespace"_attr = _nss,
                "minKey"_attr = _minKey);
}

}  // namespace mongo
