/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include <string>
#include <sstream>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/hint_commands.h"
#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/catalog/collection.h"

namespace {

    using std::string;
    using std::vector;
    using namespace mongo;

    /**
     * Releases memory for container of pointers.
     * XXX: move elsewhere when something similar is available in util libraries.
     */
    template <typename T>
    class ContainerPointersDeleter {
    public:
        ContainerPointersDeleter(T* container) : _container(container) {
            invariant(_container);
        }

        ~ContainerPointersDeleter() {
            for (typename T::const_iterator i = _container->begin();
                 i != _container->end(); ++i) {
                invariant(*i);
                delete *i;
            }
            _container->clear();
        }
    private:
        MONGO_DISALLOW_COPYING(ContainerPointersDeleter);
        T* _container;
    };

    /**
     * Utility function to extract error code and message from status
     * and append to BSON results.
     */
    void addStatus(const Status& status, BSONObjBuilder& builder) {
        builder.append("ok", status.isOK() ? 1.0 : 0.0);
        if (!status.isOK()) {
            builder.append("code", status.code());
        }
        if (!status.reason().empty()) {
            builder.append("errmsg", status.reason());
        }
    }

    /**
     * Retrieves a collection's query settings from the database.
     */
    Status getQuerySettings(Database* db, const string& ns, QuerySettings** querySettingsOut) {
        invariant(db);

        Collection* collection = db->getCollection(ns);
        if (NULL == collection) {
            return Status(ErrorCodes::BadValue, "no such collection");
        }

        CollectionInfoCache* infoCache = collection->infoCache();
        invariant(infoCache);

        QuerySettings* querySettings = infoCache->getQuerySettings();
        invariant(querySettings);

        *querySettingsOut = querySettings;

        return Status::OK();
    }

    /**
     * Retrieves a collection's plan cache from the database.
     */
    Status getPlanCache(Database* db, const string& ns, PlanCache** planCacheOut) {
        invariant(db);

        Collection* collection = db->getCollection(ns);
        if (NULL == collection) {
            return Status(ErrorCodes::BadValue, "no such collection");
        }

        CollectionInfoCache* infoCache = collection->infoCache();
        invariant(infoCache);

        PlanCache* planCache = infoCache->getPlanCache();
        invariant(planCache);

        *planCacheOut = planCache;
        return Status::OK();
    }

    //
    // Command instances.
    // Registers commands with the command system and make commands
    // available to the client.
    //

    MONGO_INITIALIZER_WITH_PREREQUISITES(SetupHintCommands, MONGO_NO_PREREQUISITES)(
            InitializerContext* context) {

        new ListHints();
        new ClearHints();
        new SetHint();

        return Status::OK();
    }

} // namespace

namespace mongo {

    using std::string;
    using std::stringstream;
    using std::vector;
    using boost::scoped_ptr;

    HintCommand::HintCommand(const string& name, const string& helpText)
        : Command(name),
          helpText(helpText) { }

    bool HintCommand::run(const string& dbname, BSONObj& cmdObj, int options,
                           string& errmsg, BSONObjBuilder& result, bool fromRepl) {
        string ns = parseNs(dbname, cmdObj);

        Status status = runHintCommand(ns, cmdObj, &result);

        if (!status.isOK()) {
            addStatus(status, result);
            return false;
        }

        return true;
    }

    Command::LockType HintCommand::locktype() const {
        return NONE;
    }

    bool HintCommand::slaveOk() const {
        return false;
    }

    void HintCommand::help(stringstream& ss) const {
        ss << helpText;
    }

    Status HintCommand::checkAuthForCommand(ClientBasic* client, const std::string& dbname,
                                            const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = client->getAuthorizationSession();
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern, ActionType::planCacheHint)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    ListHints::ListHints() : HintCommand("planCacheListHints",
        "Displays admin hints for all query shapes in a collection.") { }

    Status ListHints::runHintCommand(const string& ns, BSONObj& cmdObj, BSONObjBuilder* bob) {
        // This is a read lock. The query settings is owned by the collection.
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        QuerySettings* querySettings;
        Status status = getQuerySettings(ctx.db(), ns, &querySettings);
        if (!status.isOK()) {
            return status;
        }
        return list(*querySettings, bob);
    }

    // static
    Status ListHints::list(const QuerySettings& querySettings, BSONObjBuilder* bob) {
        invariant(bob);

        // Format of BSON result:
        //
        // {
        //     hints: [
        //         {
        //             query: <query>,
        //             sort: <sort>,
        //             projection: <projection>,
        //             indexes: [<index1>, <index2>, <index3>, ...]
        //         }
        //  }
        BSONArrayBuilder hintsBuilder(bob->subarrayStart("hints"));
        vector<AllowedIndexEntry*> entries = querySettings.getAllAllowedIndices();
        // Frees resources in entries on destruction.
        ContainerPointersDeleter<vector<AllowedIndexEntry*> > deleter(&entries);
        for (vector<AllowedIndexEntry*>::const_iterator i = entries.begin();
             i != entries.end(); ++i) {
            AllowedIndexEntry* entry = *i;
            invariant(entry);

            BSONObjBuilder hintBob(hintsBuilder.subobjStart());
            hintBob.append("query", entry->query);
            hintBob.append("sort", entry->sort);
            hintBob.append("projection", entry->projection);
            BSONArrayBuilder indexesBuilder(hintBob.subarrayStart("indexes"));
            for (vector<BSONObj>::const_iterator j = entry->indexKeyPatterns.begin();
                 j != entry->indexKeyPatterns.end(); ++j) {
                const BSONObj& index = *j;
                indexesBuilder.append(index);
            }
            indexesBuilder.doneFast();
        }
        hintsBuilder.doneFast();
        return Status::OK();
    }

    ClearHints::ClearHints() : HintCommand("planCacheClearHints",
        "Clears all admin hints for a single query shape or, "
        "if the query shape is omitted, for the entire collection.") { }

    Status ClearHints::runHintCommand(const string& ns, BSONObj& cmdObj, BSONObjBuilder* bob) {
        // This is a read lock. The query settings is owned by the collection.
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        QuerySettings* querySettings;
        Status status = getQuerySettings(ctx.db(), ns, &querySettings);
        if (!status.isOK()) {
            return status;
        }
        PlanCache* planCache;
        status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return clear(querySettings, planCache, ns, cmdObj);
    }

    // static
    Status ClearHints::clear(QuerySettings* querySettings, PlanCache* planCache,
                             const std::string& ns, const BSONObj& cmdObj) {
        invariant(querySettings);

        // According to the specification, the planCacheClearHints command runs in two modes:
        // - clear all hints; or
        // - clear hints for single query shape when a query shape is described in the
        //   command arguments.
        if (cmdObj.hasField("query")) {
            CanonicalQuery* cqRaw;
            Status status = PlanCacheCommand::canonicalize(ns, cmdObj, &cqRaw);
            if (!status.isOK()) {
                return status;
            }

            scoped_ptr<CanonicalQuery> cq(cqRaw);
            querySettings->removeAllowedIndices(*cq);

            // Remove entry from plan cache
            planCache->remove(*cq);
            return Status::OK();
        }

        // If query is not provided, make sure sort and projection are not in arguments.
        // We do not want to clear the entire cache inadvertently when the user
        // forgot to provide a value for "query".
        if (cmdObj.hasField("sort") || cmdObj.hasField("projection")) {
            return Status(ErrorCodes::BadValue, "sort or projection provided without query");
        }

        // Get entries from query settings. We need to remove corresponding entries from the plan
        // cache shortly.
        vector<AllowedIndexEntry*> entries = querySettings->getAllAllowedIndices();
        // Frees resources in entries on destruction.
        ContainerPointersDeleter<vector<AllowedIndexEntry*> > deleter(&entries);

        // OK to proceed with clearing entire cache.
        querySettings->clearAllowedIndices();

        // Remove corresponding entries from plan cache.
        // Admin hints affect the planning process directly. If there were
        // plans generated as a result of applying admin hints, these need to be
        // invalidated. This allows the planner to re-populate the plan cache with
        // non-admin hinted solutions next time the query is run.
        // Resolve plan cache key from (query, sort, projection) in query settings entry.
        // Concurrency note: There's no harm in removing plan cache entries one at at time.
        // Only way that PlanCache::remove() can fail is when the query shape has been removed from
        // the cache by some other means (re-index, collection info reset, ...). This is OK since
        // that's the intended effect of calling the remove() function with the key from the hint entry.
        for (vector<AllowedIndexEntry*>::const_iterator i = entries.begin();
             i != entries.end(); ++i) {
            AllowedIndexEntry* entry = *i;
            invariant(entry);

            // Create canonical query.
            CanonicalQuery* cqRaw;
            Status result = CanonicalQuery::canonicalize(ns, entry->query, entry->sort,
                                                         entry->projection, &cqRaw);
            invariant(result.isOK());
            scoped_ptr<CanonicalQuery> cq(cqRaw);

            // Remove plan cache entry.
            planCache->remove(*cq);
        }

        return Status::OK();
    }

    SetHint::SetHint() : HintCommand("planCacheSetHint",
        "Sets admin hints for a query shape. Overrides existing hints.") { }

    Status SetHint::runHintCommand(const string& ns, BSONObj& cmdObj, BSONObjBuilder* bob) {
        // This is a read lock. The query settings is owned by the collection.
        Client::ReadContext readCtx(ns);
        Client::Context& ctx = readCtx.ctx();
        QuerySettings* querySettings;
        Status status = getQuerySettings(ctx.db(), ns, &querySettings);
        if (!status.isOK()) {
            return status;
        }
        PlanCache* planCache;
        status = getPlanCache(ctx.db(), ns, &planCache);
        if (!status.isOK()) {
            return status;
        }
        return set(querySettings, planCache, ns, cmdObj);
    }

    // static
    Status SetHint::set(QuerySettings* querySettings, PlanCache* planCache,
                        const string& ns, const BSONObj& cmdObj) {
        // indexes - required
        BSONElement indexesElt = cmdObj.getField("indexes");
        if (indexesElt.eoo()) {
            return Status(ErrorCodes::BadValue, "required field indexes missing");
        }
        if (indexesElt.type() != mongo::Array) {
            return Status(ErrorCodes::BadValue, "required field indexes must be an array");
        }
        vector<BSONElement> indexesEltArray = indexesElt.Array();
        if (indexesEltArray.empty()) {
            return Status(ErrorCodes::BadValue,
                          "required field indexes must contain at least one index");
        }
        vector<BSONObj> indexes;
        for (vector<BSONElement>::const_iterator i = indexesEltArray.begin();
             i != indexesEltArray.end(); ++i) {
            const BSONElement& elt = *i;
            if (!elt.isABSONObj()) {
                return Status(ErrorCodes::BadValue, "each item in indexes must be an object");
            }
            BSONObj obj = elt.Obj();
            if (obj.isEmpty()) {
                return Status(ErrorCodes::BadValue, "index specification cannot be empty");
            }
            indexes.push_back(obj.copy());
        }

        CanonicalQuery* cqRaw;
        Status status = PlanCacheCommand::canonicalize(ns, cmdObj, &cqRaw);
        if (!status.isOK()) {
            return status;
        }
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        // Add allowed indices to query settings, overriding any previous entries.
        querySettings->setAllowedIndices(*cq, indexes);

        // Remove entry from plan cache.
        planCache->remove(*cq);

        return Status::OK();
    }

} // namespace mongo
