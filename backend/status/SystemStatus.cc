// Copyright 2015 Google Inc. All Rights Reserved.
// Author: Sebastian Schaffert <schaffert@google.com>

#include <util/MemStat.h>
#include <util/Retry.h>
#include <status/Version.h>
#include <ctime>
#include <glog/logging.h>

#include "SystemStatus.h"

using wikidata::primarysources::model::ApprovalState;

namespace wikidata {
namespace primarysources {
namespace status {

namespace {
// format a time_t using ISO8601 GMT time
inline std::string formatGMT(time_t* time) {
    char result[128];
    std::strftime(result, 128, "%Y-%m-%dT%H:%M:%SZ", gmtime(time));
    return std::string(result);
}
}  // namespace


StatusService::StatusService(const std::string& connstr)
        : connstr_(connstr), dirty_(true), shutdown_(false) {
    // set system startup time
    time_t startupTime = std::time(nullptr);
    status_.mutable_system()->set_startup(formatGMT(&startupTime));
    status_.mutable_system()->set_version(std::string(GIT_SHA1));

    updater_ = std::thread([&](){
        LOG(INFO) << "Starting status updater thread ...";
        while (!shutdown_) {
            if (dirty_) {
                LOG(INFO) << "Updating cached status ...";

                // Trigger caching of values.
                Status();
            }
            std::unique_lock<std::mutex> lck(status_mutex_);
            notify_dirty_.wait(lck);
        }
    });
}

void StatusService::AddCacheHit() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_system()->set_cache_hits(
            status_.system().cache_hits() + 1);
}
void StatusService::AddCacheMiss() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_system()->set_cache_misses(
            status_.system().cache_misses() + 1);
}

void StatusService::AddGetEntityRequest() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_requests()->set_get_entity(
            status_.requests().get_entity() + 1);
}

void StatusService::AddGetRandomRequest() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_requests()->set_get_random(
            status_.requests().get_random() + 1);
}

void StatusService::AddGetStatementRequest() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_requests()->set_get_statement(
            status_.requests().get_statement() + 1);

}

void StatusService::AddUpdateStatementRequest() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_requests()->set_update_statement(
            status_.requests().update_statement() + 1);
}

void StatusService::AddGetStatusRequest() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.mutable_requests()->set_get_status(
            status_.requests().get_status() + 1);
}

// Update the system status and return a constant reference.
model::Status StatusService::Status(const std::string& dataset) {
    MemStat memstat;

    LOG_IF(INFO, memstat.getSharedMem() > status_.system().shared_memory())
        << "Increase of shared memory from " << status_.system().shared_memory()
        << " to " << memstat.getSharedMem();
    LOG_IF(INFO, memstat.getPrivateMem() > status_.system().private_memory())
        << "Increase of private memory from " << status_.system().private_memory()
        << " to " << memstat.getPrivateMem();
    LOG_IF(INFO, memstat.getRSS() > status_.system().resident_set_size())
        << "Increase of resident memory from " << status_.system().resident_set_size()
        << " to " << memstat.getRSS();

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.mutable_system()->set_shared_memory(memstat.getSharedMem());
        status_.mutable_system()->set_private_memory(memstat.getPrivateMem());
        status_.mutable_system()->set_resident_set_size(memstat.getRSS());
    }

    model::Status copy;
    model::Status* work;

    // work directly on the status in case we do not request a specific 
    // dataset, otherwise make a copy.
    if (dataset == "") {
        work = &status_;
    } else {
        copy = status_;
        work = &copy;
    }


    RETRY({
        if (dirty_) {
            cppdb::session sql(connstr_); // released when sql is destroyed
            Persistence p(sql, true);
            sql.begin();

            auto st_total = p.countStatements(dataset);
            auto st_approved = p.countStatements(ApprovalState::APPROVED, dataset);
            auto st_unapproved = p.countStatements(ApprovalState::UNAPPROVED, dataset);
            auto st_duplicate = p.countStatements(ApprovalState::DUPLICATE, dataset);
            auto st_blacklisted = p.countStatements(ApprovalState::BLACKLISTED, dataset);
            auto st_wrong = p.countStatements(ApprovalState::WRONG, dataset);
            auto users = p.countUsers();

            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                work->mutable_statements()->set_statements(st_total);
                work->mutable_statements()->set_approved(st_approved);
                work->mutable_statements()->set_unapproved(st_unapproved);
                work->mutable_statements()->set_duplicate(st_duplicate);
                work->mutable_statements()->set_blacklisted(st_blacklisted);
                work->mutable_statements()->set_wrong(st_wrong);
                work->set_total_users(users);
                work->clear_top_users();
            }

            for (model::UserStatus &st : p.getTopUsers(10)) {
                std::lock_guard<std::mutex> lock(status_mutex_);
                work->add_top_users()->Swap(&st);
            }

            if (dataset == "") {
                dirty_ = false;
            }

            sql.commit();
        }
    }, 3, cppdb::cppdb_error);


    return *work;
}

std::string StatusService::Version() const {
    return std::string(GIT_SHA1);
}

}  // namespace status
}  // namespace primarysources
}  // namespace wikidata
