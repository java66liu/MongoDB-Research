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

/**
 * This file contains tests for mongo/db/commands/hint_commands.h
 */

#include "mongo/db/commands/hint_commands.h"

#include "mongo/db/json.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    using std::string;
    using std::vector;

    static const char* ns = "somebogusns";

    /**
     * Utility function to get list of hints from the query settings.
     */
    vector<BSONObj> getHints(const QuerySettings& querySettings) {
        BSONObjBuilder bob;
        ASSERT_OK(ListHints::list(querySettings, &bob));
        BSONObj resultObj = bob.obj();
        BSONElement hintsElt = resultObj.getField("hints");
        ASSERT_EQUALS(hintsElt.type(), mongo::Array);
        vector<BSONElement> hintsEltArray = hintsElt.Array();
        vector<BSONObj> hints;
        for (vector<BSONElement>::const_iterator i = hintsEltArray.begin();
             i != hintsEltArray.end(); ++i) {
            const BSONElement& elt = *i;

            ASSERT_TRUE(elt.isABSONObj());
            BSONObj obj = elt.Obj();

            // Check required fields.
            // query
            BSONElement queryElt = obj.getField("query");
            ASSERT_TRUE(queryElt.isABSONObj());

            // sort
            BSONElement sortElt = obj.getField("sort");
            ASSERT_TRUE(sortElt.isABSONObj());

            // projection
            BSONElement projectionElt = obj.getField("projection");
            ASSERT_TRUE(projectionElt.isABSONObj());

            // indexes
            BSONElement indexesElt = obj.getField("indexes");
            ASSERT_EQUALS(indexesElt.type(), mongo::Array);

            // All fields OK. Append to vector.
            hints.push_back(obj.copy());
        }

        return hints;
    }

    /**
     * Injects an entry into plan cache for query shape.
     */
    void addQueryShapeToPlanCache(PlanCache* planCache, const char* queryStr, const char* sortStr,
                                  const char* projectionStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projectionObj = fromjson(projectionStr);

        // Create canonical query.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, sortObj, projectionObj, &cqRaw));
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        QuerySolution qs;
        qs.cacheData.reset(new SolutionCacheData());
        qs.cacheData->tree.reset(new PlanCacheIndexTree());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        ASSERT_OK(planCache->add(*cq, solns, new PlanRankingDecision()));
    }

    /**
     * Checks if plan cache contains query shape.
     */
    bool planCacheContains(const PlanCache& planCache, const char* queryStr, const char* sortStr,
                           const char* projectionStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projectionObj = fromjson(projectionStr);

        // Create canonical query.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, sortObj, projectionObj, &cqRaw));
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        // Retrieve cached solutions from plan cache.
        vector<CachedSolution*> solutions = planCache.getAllSolutions();

        // Search keys.
        bool found = false;
        for (vector<CachedSolution*>::const_iterator i = solutions.begin(); i != solutions.end(); i++) {
            CachedSolution* cs = *i;
            const PlanCacheKey& currentKey = cs->key;
            if (currentKey == cq->getPlanCacheKey()) {
                found = true;
            }
            // Release resources for cached solution after extracting key.
            delete cs;
        }
        return found;
    }

    /**
     * Tests for ListHints
     */

    TEST(HintCommandsTest, ListHintsEmpty) {
        QuerySettings empty;
        vector<BSONObj> hints = getHints(empty);
        ASSERT_TRUE(hints.empty());
    }

    /**
     * Tests for ClearHints
     */

    TEST(HintCommandsTest, ClearHintsInvalidParameter) {
        QuerySettings empty;
        PlanCache planCache;
        // If present, query has to be an object.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns, fromjson("{query: 1234}")));
        // If present, sort must be an object.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns,
                                        fromjson("{query: {a: 1}, sort: 1234}")));
        // If present, projection must be an object.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns,
                                        fromjson("{query: {a: 1}, projection: 1234}")));
        // Query must pass canonicalization.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns,
                                        fromjson("{query: {a: {$no_such_op: 1}}}")));
        // Sort present without query is an error.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns, fromjson("{sort: {a: 1}}")));
        // Projection present without query is an error.
        ASSERT_NOT_OK(ClearHints::clear(&empty, &planCache, ns,
                                        fromjson("{projection: {_id: 0, a: 1}}")));
    }

    TEST(HintCommandsTest, ClearNonexistentHint) {
        QuerySettings querySettings;
        PlanCache planCache;
        ASSERT_OK(SetHint::set(&querySettings, &planCache, ns,
            fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
        vector<BSONObj> hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 1U);

        // Clear nonexistent hint.
        // Command should succeed and cache should remain unchanged.
        ASSERT_OK(ClearHints::clear(&querySettings, &planCache, ns, fromjson("{query: {b: 1}}")));
        hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 1U);
    }

    /**
     * Tests for SetHint
     */

    TEST(HintCommandsTest, SetHintInvalidParameter) {
        QuerySettings empty;
        PlanCache planCache;
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns, fromjson("{}")));
        // Missing required query field.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns, fromjson("{indexes: [{a: 1}]}")));
        // Missing required indexes field.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns, fromjson("{query: {a: 1}}")));
        // Query has to be an object.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
            fromjson("{query: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // Indexes field has to be an array.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: 1234}")));
        // Array indexes field cannot empty.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: []}")));
        // Elements in indexes have to be objects.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: [{a: 1}, 99]}")));
        // Objects in indexes cannot be empty.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: [{a: 1}, {}]}")));
        // If present, sort must be an object.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
            fromjson("{query: {a: 1}, sort: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // If present, projection must be an object.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
            fromjson("{query: {a: 1}, projection: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // Query must pass canonicalization.
        ASSERT_NOT_OK(SetHint::set(&empty, &planCache, ns,
            fromjson("{query: {a: {$no_such_op: 1}}, indexes: [{a: 1}, {b: 1}]}")));
    }

    TEST(HintCommandsTest, SetAndClearHints) {
        QuerySettings querySettings;
        PlanCache planCache;

        // Inject query shape into plan cache.
        addQueryShapeToPlanCache(&planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}");
        ASSERT_TRUE(planCacheContains(planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}"));

        ASSERT_OK(SetHint::set(&querySettings, &planCache, ns,
            fromjson("{query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 0, a: 1}, "
                     "indexes: [{a: 1}]}")));
        vector<BSONObj> hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 1U);

        // Query shape should not exist in plan cache after hint is updated.
        ASSERT_FALSE(planCacheContains(planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}"));

        // Value of entries in hints should match criteria in most recent query settings update.
        ASSERT_EQUALS(hints[0].getObjectField("query"), fromjson("{a: 1, b: 1}"));
        ASSERT_EQUALS(hints[0].getObjectField("sort"), fromjson("{a: -1}"));
        ASSERT_EQUALS(hints[0].getObjectField("projection"), fromjson("{_id: 0, a: 1}"));

        // Replacing the hint for the same query shape ({a: 1, b: 1} and {b: 2, a: 3}
        // share same shape) should not change the query settings size.
        ASSERT_OK(SetHint::set(&querySettings, &planCache, ns,
            fromjson("{query: {b: 2, a: 3}, sort: {a: -1}, projection: {_id: 0, a: 1}, "
                     "indexes: [{a: 1, b: 1}]}")));
        hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 1U);

        // Add hint for different query shape.
        ASSERT_OK(SetHint::set(&querySettings, &planCache, ns,
                               fromjson("{query: {b: 1}, indexes: [{b: 1}]}")));
        hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 2U);

        // Add hint for 3rd query shape. This is to prepare for ClearHint tests.
        ASSERT_OK(SetHint::set(&querySettings, &planCache, ns,
                               fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
        hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 3U);

        // Add 2 entries to plan cache and check plan cache after clearing one/all hints.
        addQueryShapeToPlanCache(&planCache, "{a: 1}", "{}", "{}");
        addQueryShapeToPlanCache(&planCache, "{b: 1}", "{}", "{}");

        // Clear single hint.
        ASSERT_OK(ClearHints::clear(&querySettings, &planCache, ns,
                                    fromjson("{query: {a: 1}}")));
        hints = getHints(querySettings);
        ASSERT_EQUALS(hints.size(), 2U);

        // Query shape should not exist in plan cache after cleaing 1 hint.
        ASSERT_FALSE(planCacheContains(planCache, "{a: 1}", "{}", "{}"));
        ASSERT_TRUE(planCacheContains(planCache, "{b: 1}", "{}", "{}"));

        // Clear all hints
        ASSERT_OK(ClearHints::clear(&querySettings, &planCache, ns, fromjson("{}")));
        hints = getHints(querySettings);
        ASSERT_TRUE(hints.empty());

        // {b: 1} should be gone from plan cache after flushing query settings.
        ASSERT_FALSE(planCacheContains(planCache, "{b: 1}", "{}", "{}"));
    }

}  // namespace
