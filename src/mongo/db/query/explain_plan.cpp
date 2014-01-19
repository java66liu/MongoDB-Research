/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/explain_plan.h"

#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    namespace {

        bool isOrStage(StageType stageType) {
            return stageType == STAGE_OR || stageType == STAGE_SORT_MERGE;
        }

        bool isIntersectPlan(const PlanStageStats& stats) {
            if (stats.stageType == STAGE_AND_HASH || stats.stageType == STAGE_AND_SORTED) {
                return true;
            }
            for (size_t i = 0; i < stats.children.size(); ++i) {
                if (isIntersectPlan(*stats.children[i])) {
                    return true;
                }
            }
            return false;
        }

        void getLeafNodes(const PlanStageStats& stats, vector<const PlanStageStats*>* leafNodesOut) {
            if (0 == stats.children.size()) {
                leafNodesOut->push_back(&stats);
            }
            for (size_t i = 0; i < stats.children.size(); ++i) {
                getLeafNodes(*stats.children[i], leafNodesOut);
            }
        }

        const PlanStageStats* findNode(const PlanStageStats* root, StageType type) {
            if (root->stageType == type) {
                return root;
            }
            for (size_t i = 0; i < root->children.size(); ++i) {
                const PlanStageStats* ret = findNode(root->children[i], type);
                if (NULL != ret) {
                    return ret;
                }
            }
            return NULL;
        }

    }  // namespace

    Status explainIntersectPlan(const PlanStageStats& stats, TypeExplain** explain, bool fullDetails) {
        auto_ptr<TypeExplain> res(new TypeExplain);
        res->setCursor("Complex Plan");
        res->setN(stats.common.advanced);

        // Sum the various counters at the leaves.
        vector<const PlanStageStats*> leaves;
        getLeafNodes(stats, &leaves);

        long long nScanned = 0;
        long long nScannedObjects = 0;
        for (size_t i = 0; i < leaves.size(); ++i) {
            TypeExplain* leafExplain;
            explainPlan(*leaves[i], &leafExplain, false);
            nScanned += leafExplain->getNScanned();
            nScannedObjects += leafExplain->getNScannedObjects();
            delete leafExplain;
        }

        res->setNScanned(nScanned);
        // XXX: this isn't exactly "correct" -- for ixscans we have to find out if it's part of a
        // subtree rooted at a fetch, etc. etc.  XXX: do we want to just add the # of advances of a
        // fetch node minus the number of alreadyHasObj for those nodes?
        res->setNScannedObjects(nScannedObjects);

        uint64_t chunkSkips = 0;
        const PlanStageStats* shardFilter = findNode(&stats, STAGE_SHARDING_FILTER);
        if (NULL != shardFilter) {
            const ShardingFilterStats* sfs
                = static_cast<const ShardingFilterStats*>(shardFilter->specific.get());
            chunkSkips = sfs->chunkSkips;
        }

        res->setNChunkSkips(chunkSkips);

        if (fullDetails) {
            res->setNYields(stats.common.yields);
            res->stats = statsToBSON(stats);
        }

        *explain = res.release();
        return Status::OK();
    }

    Status explainPlan(const PlanStageStats& stats, TypeExplain** explain, bool fullDetails) {
        //
        // Temporary explain for index intersection
        //

        if (isIntersectPlan(stats)) {
            return explainIntersectPlan(stats, explain, fullDetails);
        }

        //
        // Legacy explain implementation
        //

        // Descend the plan looking for structural properties:
        // + Are there any OR clauses?  If so, explain each branch.
        // + What type(s) are the leaf nodes and what are their properties?
        // + Did we need a sort?

        bool covered = true;
        bool sortPresent = false;
        size_t chunkSkips = 0;

        const PlanStageStats* orStage = NULL;
        const PlanStageStats* root = &stats;
        const PlanStageStats* leaf = root;

        while (leaf->children.size() > 0) {
            // We shouldn't be here if there are any ANDs
            if (leaf->children.size() > 1) {
                verify(isOrStage(leaf->stageType));
            }

            if (isOrStage(leaf->stageType)) {
                orStage = leaf;
                break;
            }

            if (leaf->stageType == STAGE_FETCH) {
                covered = false;
            }

            if (leaf->stageType == STAGE_SORT) {
                sortPresent = true;
            }

            if (STAGE_SHARDING_FILTER == leaf->stageType) {
                const ShardingFilterStats* sfs
                    = static_cast<const ShardingFilterStats*>(leaf->specific.get());
                chunkSkips = sfs->chunkSkips;
            }

            leaf = leaf->children[0];
        }

        auto_ptr<TypeExplain> res(new TypeExplain);

        // Accounting for 'nscanned' and 'nscannedObjects' is specific to the kind of leaf:
        //
        // + on collection scan, both are the same; all the documents retrieved were
        //   fetched in practice. To get how many documents were retrieved, one simply
        //   looks at the number of 'advanced' in the stats.
        //
        // + on an index scan, we'd neeed to look into the index scan cursor to extract the
        //   number of keys that cursor retrieved, and into the stage's stats 'advanced' for
        //   nscannedObjects', which would be the number of keys that survived the IXSCAN
        //   filter. Those keys would have been FETCH-ed, if a fetch is present.

        if (orStage != NULL) {
            size_t nScanned = 0;
            size_t nScannedObjects = 0;
            const std::vector<PlanStageStats*>& children = orStage->children;
            for (std::vector<PlanStageStats*>::const_iterator it = children.begin();
                 it != children.end();
                 ++it) {
                TypeExplain* childExplain = NULL;
                explainPlan(**it, &childExplain, false /* no full details */);
                if (childExplain) {
                    // 'res' takes ownership of 'childExplain'.
                    res->addToClauses(childExplain);
                    nScanned += childExplain->getNScanned();

                    // We don't necessarilly fetch on a branch, but the old query framework
                    // did. We're still emulating the number it would have produced.
                    nScannedObjects += childExplain->getNScanned();
                }
            }
            res->setNScanned(nScanned);
            res->setNScannedObjects(nScannedObjects);
        }
        else if (leaf->stageType == STAGE_COLLSCAN) {
            CollectionScanStats* csStats = static_cast<CollectionScanStats*>(leaf->specific.get());
            res->setCursor("BasicCursor");
            res->setNScanned(csStats->docsTested);
            res->setNScannedObjects(csStats->docsTested);
            res->setIndexOnly(false);
        }
        else if (leaf->stageType == STAGE_GEO_NEAR_2DSPHERE) {
            // TODO: This is kind of a lie for STAGE_GEO_NEAR_2DSPHERE.
            res->setCursor("S2NearCursor");
            // The first work() is an init.  Every subsequent work examines a document.
            res->setNScanned(leaf->common.works);
            res->setNScannedObjects(leaf->common.works);
            // TODO: Could be multikey.
            res->setIsMultiKey(false);
            res->setIndexOnly(false);
        }
        else if (leaf->stageType == STAGE_GEO_NEAR_2D) {
            TwoDNearStats* nStats = static_cast<TwoDNearStats*>(leaf->specific.get());
            res->setCursor("GeoSearchCursor");
            // The first work() is an init.  Every subsequent work examines a document.
            res->setNScanned(nStats->nscanned);
            res->setNScannedObjects(nStats->objectsLoaded);
            // TODO: Could be multikey.
            res->setIsMultiKey(false);
            res->setIndexOnly(false);
        }
        else if (leaf->stageType == STAGE_TEXT) {
            TextStats* tStats = static_cast<TextStats*>(leaf->specific.get());
            res->setCursor("TextCursor");
            res->setNScanned(tStats->keysExamined);
            res->setNScannedObjects(tStats->fetches);
        }
        else if (leaf->stageType == STAGE_IXSCAN) {
            IndexScanStats* indexStats = static_cast<IndexScanStats*>(leaf->specific.get());
            verify(indexStats);
            string direction = indexStats->direction > 0 ? "" : " reverse";
            res->setCursor(indexStats->indexType + " " + indexStats->indexName + direction);
            res->setNScanned(indexStats->keysExamined);

            // If we're covered, that is, no FETCH is present, then, by definition,
            // nScannedObject would be zero because no full document would have been fetched
            // from disk.
            res->setNScannedObjects(covered ? 0 : leaf->common.advanced);

            res->setIndexBounds(indexStats->indexBounds);
            res->setIsMultiKey(indexStats->isMultiKey);
            res->setIndexOnly(covered);
        }
        else {
            return Status(ErrorCodes::InternalError, "cannot interpret execution plan");
        }

        // How many documents did the query return?
        res->setN(root->common.advanced);
        res->setScanAndOrder(sortPresent);
        res->setNChunkSkips(chunkSkips);

        // Statistics for the plan (appear only in a detailed mode)
        // TODO: if we can get this from the runner, we can kill "detailed mode"
        if (fullDetails) {
            res->setNYields(root->common.yields);
            res->stats = statsToBSON(*root);
        }

        *explain = res.release();
        return Status::OK();
    }

    // XXX: where does this really live?  stage_types.h?
    string stageTypeString(StageType type) {
        if (STAGE_AND_HASH == type) {
            return "AND_HASH";
        }
        else if (STAGE_AND_SORTED == type) {
            return "AND_SORTED";
        }
        else if (STAGE_COLLSCAN == type) {
            return "COLLSCAN";
        }
        else if (STAGE_FETCH == type) {
            return "FETCH";
        }
        else if (STAGE_GEO_2D == type) {
            return "GEO_2D";
        }
        else if (STAGE_GEO_NEAR_2D == type) {
            return "GEO_NEAR_2D";
        }
        else if (STAGE_GEO_NEAR_2DSPHERE == type) {
            return "GEO_NEAR_2DSPHERE";
        }
        else if (STAGE_IXSCAN == type) {
            return "IXSCAN";
        }
        else if (STAGE_LIMIT == type) {
            return "LIMIT";
        }
        else if (STAGE_OR == type) {
            return "OR";
        }
        else if (STAGE_PROJECTION == type) {
            return "PROJECTION";
        }
        else if (STAGE_SHARDING_FILTER == type) {
            return "SHARDING_FILTER";
        }
        else if (STAGE_SKIP == type) {
            return "SKIP";
        }
        else if (STAGE_SORT == type) {
            return "SORT";
        }
        else if (STAGE_SORT_MERGE == type) {
            return "SORT_MERGE";
        }
        else if (STAGE_TEXT == type) {
            return "TEXT";
        }
        else {
            return "UNKNOWN.  SHOULD NOT SEE THIS.";
        }
    }

    BSONObj statsToBSON(const PlanStageStats& stats) {
        BSONObjBuilder bob;

        // Common details.
        bob.append("type", stageTypeString(stats.stageType));
        bob.appendNumber("works", stats.common.works);
        bob.appendNumber("yields", stats.common.yields);
        bob.appendNumber("unyields", stats.common.unyields);
        bob.appendNumber("invalidates", stats.common.invalidates);
        bob.appendNumber("advanced", stats.common.advanced);
        bob.appendNumber("needTime", stats.common.needTime);
        bob.appendNumber("needFetch", stats.common.needFetch);
        bob.appendNumber("isEOF", stats.common.isEOF);

        // Stage-specific stats
        if (STAGE_AND_HASH == stats.stageType) {
            AndHashStats* spec = static_cast<AndHashStats*>(stats.specific.get());
            bob.appendNumber("flaggedButPassed", spec->flaggedButPassed);
            bob.appendNumber("flaggedInProgress", spec->flaggedInProgress);
            for (size_t i = 0; i < spec->mapAfterChild.size(); ++i) {
                bob.appendNumber(string(stream() << "mapAfterChild_" << i), spec->mapAfterChild[i]);
            }
        }
        else if (STAGE_AND_SORTED == stats.stageType) {
            AndSortedStats* spec = static_cast<AndSortedStats*>(stats.specific.get());
            bob.appendNumber("flagged", spec->flagged);
            bob.appendNumber("matchTested", spec->matchTested);
            for (size_t i = 0; i < spec->failedAnd.size(); ++i) {
                bob.appendNumber(string(stream() << "failedAnd_" << i), spec->failedAnd[i]);
            }
        }
        else if (STAGE_COLLSCAN == stats.stageType) {
            CollectionScanStats* spec = static_cast<CollectionScanStats*>(stats.specific.get());
            bob.appendNumber("docsTested", spec->docsTested);
        }
        else if (STAGE_FETCH == stats.stageType) {
            FetchStats* spec = static_cast<FetchStats*>(stats.specific.get());
            bob.appendNumber("alreadyHasObj", spec->alreadyHasObj);
            bob.appendNumber("forcedFetches", spec->forcedFetches);
            bob.appendNumber("matchTested", spec->matchTested);
        }
        else if (STAGE_GEO_NEAR_2D == stats.stageType) {
            TwoDNearStats* spec = static_cast<TwoDNearStats*>(stats.specific.get());
            bob.appendNumber("objectsLoaded", spec->objectsLoaded);
            bob.appendNumber("nscanned", spec->nscanned);
        }
        else if (STAGE_IXSCAN == stats.stageType) {
            IndexScanStats* spec = static_cast<IndexScanStats*>(stats.specific.get());
            // XXX: how much do we really want here?  runtime stats vs. tree structure (soln
            // tostring).
            bob.append("keyPattern", spec->keyPattern.toString());
            bob.append("bounds", spec->indexBounds);
            bob.appendNumber("isMultiKey", spec->isMultiKey);

            bob.appendNumber("yieldMovedCursor", spec->yieldMovedCursor);
            bob.appendNumber("dupsTested", spec->dupsTested);
            bob.appendNumber("dupsDropped", spec->dupsDropped);
            bob.appendNumber("seenInvalidated", spec->seenInvalidated);
            bob.appendNumber("matchTested", spec->matchTested);
            bob.appendNumber("keysExamined", spec->keysExamined);
        }
        else if (STAGE_OR == stats.stageType) {
            OrStats* spec = static_cast<OrStats*>(stats.specific.get());
            bob.appendNumber("dupsTested", spec->dupsTested);
            bob.appendNumber("dupsDropped", spec->dupsDropped);
            bob.appendNumber("locsForgotten", spec->locsForgotten);
            for (size_t i = 0 ; i < spec->matchTested.size(); ++i) {
                bob.appendNumber(string(stream() << "matchTested_" << i), spec->matchTested[i]);
            }
        }
        else if (STAGE_SHARDING_FILTER == stats.stageType) {
            ShardingFilterStats* spec = static_cast<ShardingFilterStats*>(stats.specific.get());
            bob.appendNumber("chunkSkips", spec->chunkSkips);
        }
        else if (STAGE_SORT == stats.stageType) {
            SortStats* spec = static_cast<SortStats*>(stats.specific.get());
            bob.appendNumber("forcedFetches", spec->forcedFetches);
        }
        else if (STAGE_SORT_MERGE == stats.stageType) {
            MergeSortStats* spec = static_cast<MergeSortStats*>(stats.specific.get());
            bob.appendNumber("dupsTested", spec->dupsTested);
            bob.appendNumber("dupsDropped", spec->dupsDropped);
            bob.appendNumber("forcedFetches", spec->forcedFetches);
        }
        else if (STAGE_TEXT == stats.stageType) {
            TextStats* spec = static_cast<TextStats*>(stats.specific.get());
            bob.appendNumber("keysExamined", spec->keysExamined);
            bob.appendNumber("fetches", spec->fetches);
        }

        BSONArrayBuilder childBob(bob.subarrayStart("children"));
        for (size_t i = 0; i < stats.children.size(); ++i) {
            childBob.append(statsToBSON(*stats.children[i]));
        }

        childBob.done();
        return bob.obj();
    }

} // namespace mongo
