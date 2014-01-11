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

#include "mongo/db/query/planner_analysis.h"

#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    const size_t QueryPlannerAnalysis::kMaxScansToExplode = 50;

    //
    // Helpers for bounds explosion AKA quick-and-dirty SERVER-1205.
    //

    namespace {
        /**
         * Walk the tree 'root' and output all leaf nodes into 'leafNodes'.
         */
        void getLeafNodes(QuerySolutionNode* root, vector<QuerySolutionNode*>* leafNodes) {
            if (0 == root->children.size()) {
                leafNodes->push_back(root);
            }
            else {
                for (size_t i = 0; i < root->children.size(); ++i) {
                    getLeafNodes(root->children[i], leafNodes);
                }
            }
        }

        /**
         * Returns true if every interval in 'oil' is a point, false otherwise.
         */
        bool isUnionOfPoints(const OrderedIntervalList& oil) {
            for (size_t i = 0; i < oil.intervals.size(); ++i) {
                if (!oil.intervals[i].isPoint()) {
                    return false;
                }
            }
            return true;
        }

        /**
         * Should we try to expand the index scan(s) in 'solnRoot' to pull out an indexed sort?
         */
        bool structureOKForExplode(QuerySolutionNode* solnRoot) {
            // For now we only explode if we *know* we will pull the sort out.  We can look at
            // more structure (or just explode and recalculate properties and see what happens)
            // but for now we just explode if it's a sure bet.
            //
            // TODO: Can also try exploding if root is OR and children are ixscans, or root is AND_HASH
            // (last child dictates order.), or other less obvious cases...
            if (STAGE_IXSCAN == solnRoot->getType()) {
                return true;
            }

            if (STAGE_FETCH == solnRoot->getType()) {
                 if (STAGE_IXSCAN == solnRoot->children[0]->getType()) {
                     return true;
                 }
            }

            return false;
        }

        // vectors of vectors can be > > annoying.
        typedef vector<Interval> PointPrefix;

        /**
         * The first 'fieldsToExplode' fields of 'bounds' are points.  Compute the Cartesian product
         * of those fields and place it in 'prefixOut'.
         */
        void makeCartesianProduct(const IndexBounds& bounds,
                                  size_t fieldsToExplode,
                                  vector<PointPrefix>* prefixOut) {

            vector<PointPrefix> prefixForScans;

            // We dump the Cartesian product of bounds into prefixForScans, starting w/the first
            // field's points.
            verify(fieldsToExplode >= 1);
            const OrderedIntervalList& firstOil = bounds.fields[0];
            verify(firstOil.intervals.size() >= 1);
            for (size_t i = 0; i < firstOil.intervals.size(); ++i) {
                const Interval& ival = firstOil.intervals[i];
                verify(ival.isPoint());
                PointPrefix pfix;
                pfix.push_back(ival);
                prefixForScans.push_back(pfix);
            }

            // For each subsequent field...
            for (size_t i = 1; i < fieldsToExplode; ++i) {
                vector<PointPrefix> newPrefixForScans;
                const OrderedIntervalList& oil = bounds.fields[i];
                verify(oil.intervals.size() >= 1);
                // For each point interval in that field (all ivals must be points)...
                for (size_t j = 0; j < oil.intervals.size(); ++j) {
                    const Interval& ival = oil.intervals[j];
                    verify(ival.isPoint());
                    // Make a new scan by appending it to all scans in prefixForScans.
                    for (size_t k = 0; k < prefixForScans.size(); ++k) {
                        PointPrefix pfix = prefixForScans[k];
                        pfix.push_back(ival);
                        newPrefixForScans.push_back(pfix);
                    }
                }
                // And update prefixForScans.
                newPrefixForScans.swap(prefixForScans);
            }

            prefixOut->swap(prefixForScans);
        }

        /**
         * Take the provided index scan node 'isn' and return a logically equivalent node that
         * provides the same data but provides the sort order 'sort'.
         *
         * fieldsToExplode is a count of how many fields in the scan's bounds are the union of point
         * intervals.  This is computed beforehand and provided as a small optimization.
         *
         * Example:
         *
         * For the query find({a: {$in: [1,2]}}).sort({b: 1}) using the index {a:1, b:1}:
         * 'isn' will be scan with bounds a:[[1,1],[2,2]] & b: [MinKey, MaxKey]
         * 'sort' will be {b: 1}
         * 'fieldsToExplode' will be 1 (as only one field isUnionOfPoints).
         *
         * The solution returned will be a mergesort of the two scans:
         * a:[[1,1]], b:[MinKey, MaxKey]
         * a:[[2,2]], b:[MinKey, MaxKey]
         */
        QuerySolutionNode* explodeScan(IndexScanNode* isn,
                                       const BSONObj& sort,
                                       size_t fieldsToExplode) {

            // Turn the compact bounds in 'isn' into a bunch of points...
            vector<PointPrefix> prefixForScans;
            makeCartesianProduct(isn->bounds, fieldsToExplode, &prefixForScans);

            // And merge-sort the scans over those points.
            auto_ptr<MergeSortNode> merge(new MergeSortNode());
            merge->sort = sort;

            for (size_t i = 0; i < prefixForScans.size(); ++i) {
                const PointPrefix& prefix = prefixForScans[i];
                verify(prefix.size() == fieldsToExplode);

                // Copy boring fields into new child.
                IndexScanNode* child = new IndexScanNode();
                child->indexKeyPattern = isn->indexKeyPattern;
                child->direction = isn->direction;
                child->maxScan = isn->maxScan;
                child->addKeyMetadata = isn->addKeyMetadata;
                child->indexIsMultiKey = isn->indexIsMultiKey;

                // Create child bounds.
                child->bounds.fields.resize(isn->bounds.fields.size());
                for (size_t j = 0; j < fieldsToExplode; ++j) {
                    child->bounds.fields[j].intervals.push_back(prefix[j]);
                    child->bounds.fields[j].name = isn->bounds.fields[j].name;
                }
                for (size_t j = fieldsToExplode; j < isn->bounds.fields.size(); ++j) {
                    child->bounds.fields[j] = isn->bounds.fields[j];
                }
                merge->children.push_back(child);
            }

            merge->computeProperties();
            return merge.release();
        }

        /**
         * In the tree '*root', replace 'oldNode' with 'newNode'.
         */
        void replaceNodeInTree(QuerySolutionNode** root,
                              QuerySolutionNode* oldNode,
                              QuerySolutionNode* newNode) {
            if (*root == oldNode) {
                *root = newNode;
            }
            else {
                for (size_t i = 0 ; i < (*root)->children.size(); ++i) {
                    replaceNodeInTree(&(*root)->children[i], oldNode, newNode);
                }
            }
        }

    }  // namespace

    bool QueryPlannerAnalysis::explodeForSort(const CanonicalQuery& query,
                                              const QueryPlannerParams& params,
                                              QuerySolutionNode** solnRoot) {
        vector<QuerySolutionNode*> leafNodes;

        if (!structureOKForExplode(*solnRoot)) {
            return false;
        }

        getLeafNodes(*solnRoot, &leafNodes);

        const BSONObj& desiredSort = query.getParsed().getSort();

        // How many scan leaves will result from our expansion?
        size_t totalNumScans = 0;

        // The value of entry i is how many scans we want to blow up for leafNodes[i].
        // We calculate this in the loop below and might as well reuse it if we blow up
        // that scan.
        vector<size_t> fieldsToExplode;

        // The sort order we're looking for has to possibly be provided by each of the index scans
        // upon explosion.
        for (size_t i = 0; i < leafNodes.size(); ++i) {
            // We can do this because structureOKForExplode is only true if the leaves are index
            // scans.
            IndexScanNode* isn = static_cast<IndexScanNode*>(leafNodes[i]);
            const IndexBounds& bounds = isn->bounds;

            // Not a point interval prefix, can't try to rewrite.
            if (bounds.isSimpleRange) {
                return false;
            }

            // How many scans will we create if we blow up this ixscan?
            size_t numScans = 1;

            // Skip every field that is a union of point intervals and build the resulting sort
            // order from the remaining fields.
            BSONObjIterator kpIt(isn->indexKeyPattern);
            size_t boundsIdx = 0;
            while (kpIt.more()) {
                const OrderedIntervalList& oil = bounds.fields[boundsIdx];
                if (!isUnionOfPoints(oil)) {
                    break;
                }
                numScans *= oil.intervals.size();
                kpIt.next();
                ++boundsIdx;
            }

            // There's no sort order left to gain by exploding.  Just go home.  TODO: verify nothing
            // clever we can do here.
            if (!kpIt.more()) {
                return false;
            }

            // The rest of the fields define the sort order we could obtain by exploding
            // the bounds.
            BSONObjBuilder resultingSortBob;
            while (kpIt.more()) {
                resultingSortBob.append(kpIt.next());
            }

            // See if it's the order we're looking for.
            BSONObj possibleSort = resultingSortBob.obj();
            if (0 != possibleSort.woCompare(desiredSort)) {
                return false;
            }

            // Do some bookkeeping to see how many ixscans we'll create total.
            totalNumScans += numScans;

            // And for this scan how many fields we expand.
            fieldsToExplode.push_back(boundsIdx);
        }

        // Too many ixscans spoil the performance.
        if (totalNumScans > QueryPlannerAnalysis::kMaxScansToExplode) {
            QLOG() << "Could expand ixscans to pull out sort order but resulting scan count"
                   << "(" << totalNumScans << ") is too high.";
            return false;
        }

        // If we're here, we can (probably?  depends on how restrictive the structure check is)
        // get our sort order via ixscan blow-up.
        for (size_t i = 0; i < leafNodes.size(); ++i) {
            IndexScanNode* isn = static_cast<IndexScanNode*>(leafNodes[i]);
            QuerySolutionNode* newNode = explodeScan(isn, desiredSort, fieldsToExplode[i]);
            // Replace 'isn' with 'newNode'
            replaceNodeInTree(solnRoot, isn, newNode);
            // And get rid of the old data access node.
            delete isn;
        }

        return true;
    }

    // static
    QuerySolutionNode* QueryPlannerAnalysis::analyzeSort(const CanonicalQuery& query,
                                                         const QueryPlannerParams& params,
                                                         QuerySolutionNode* solnRoot,
                                                         bool* blockingSortOut) {
        *blockingSortOut = false;

        const BSONObj& sortObj = query.getParsed().getSort();

        if (sortObj.isEmpty()) {
            return solnRoot;
        }

        // TODO: We could check sortObj for any projections other than :1 and :-1
        // and short-cut some of this.

        // If the sort is $natural, we ignore it, assuming that the caller has detected that and
        // outputted a collscan to satisfy the desired order.
        BSONElement natural = sortObj.getFieldDotted("$natural");
        if (!natural.eoo()) {
            return solnRoot;
        }

        // See if solnRoot gives us the sort.  If so, we're done.
        BSONObjSet sorts = solnRoot->getSort();

        // If the sort we want is in the set of sort orders provided already, bail out.
        if (sorts.end() != sorts.find(sortObj)) {
            return solnRoot;
        }

        // Sort is not provided.  See if we provide the reverse of our sort pattern.
        // If so, we can reverse the scan direction(s).
        BSONObj reverseSort = QueryPlannerCommon::reverseSortObj(sortObj);
        if (sorts.end() != sorts.find(reverseSort)) {
            QueryPlannerCommon::reverseScans(solnRoot);
            QLOG() << "Reversing ixscan to provide sort.  Result: "
                   << solnRoot->toString() << endl;
            return solnRoot;
        }

        // Sort not provided, can't reverse scans to get the sort.  One last trick: We can "explode"
        // index scans over point intervals to an OR of sub-scans in order to pull out a sort.
        // Let's try this.
        if (explodeForSort(query, params, &solnRoot)) {
            return solnRoot;
        }

        // If we're here, we need to add a sort stage.

        // If we're not allowed to put a blocking sort in, bail out.
        if (params.options & QueryPlannerParams::NO_BLOCKING_SORT) {
            delete solnRoot;
            return NULL;
        }

        // Add a fetch stage so we have the full object when we hit the sort stage.  XXX TODO: Can
        // we pull values out of the key and if so in what cases?  (covered_index_sort_3.js)
        if (!solnRoot->fetched()) {
            FetchNode* fetch = new FetchNode();
            fetch->children.push_back(solnRoot);
            solnRoot = fetch;
        }

        // And build the full sort stage.
        SortNode* sort = new SortNode();
        sort->pattern = sortObj;
        sort->query = query.getParsed().getFilter();
        // When setting the limit on the sort, we need to consider both
        // the limit N and skip count M. The sort should return an ordered list
        // N + M items so that the skip stage can discard the first M results.
        if (0 != query.getParsed().getNumToReturn()) {
            sort->limit = query.getParsed().getNumToReturn() +
                          query.getParsed().getSkip();
        }
        else {
            sort->limit = 0;
        }
        sort->children.push_back(solnRoot);
        solnRoot = sort;
        *blockingSortOut = true;

        return solnRoot;
    }

    // static
    QuerySolution* QueryPlannerAnalysis::analyzeDataAccess(const CanonicalQuery& query,
                                                   const QueryPlannerParams& params,
                                                   QuerySolutionNode* solnRoot) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        solnRoot->computeProperties();

        // solnRoot finds all our results.  Let's see what transformations we must perform to the
        // data.

        // If we're answering a query on a sharded system, we need to drop documents that aren't
        // logically part of our shard (XXX GREG elaborate more precisely)
        if (params.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            // XXX TODO: use params.shardKey to do fetch analysis instead of always fetching.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->children.push_back(solnRoot);
                solnRoot = fetch;
            }
            ShardingFilterNode* sfn = new ShardingFilterNode();
            sfn->children.push_back(solnRoot);
            solnRoot = sfn;
        }

        solnRoot = analyzeSort(query, params, solnRoot, &soln->hasSortStage);
        // This can happen if we need to create a blocking sort stage and we're not allowed to.
        if (NULL == solnRoot) { return NULL; }

        // Project the results.
        if (NULL != query.getProj()) {
            QLOG() << "PROJECTION: fetched status: " << solnRoot->fetched() << endl;
            QLOG() << "PROJECTION: Current plan is:\n" << solnRoot->toString() << endl;
            if (query.getProj()->requiresDocument()) {
                QLOG() << "PROJECTION: claims to require doc adding fetch.\n";
                // If the projection requires the entire document, somebody must fetch.
                if (!solnRoot->fetched()) {
                    FetchNode* fetch = new FetchNode();
                    fetch->children.push_back(solnRoot);
                    solnRoot = fetch;
                }
            }
            else {
                QLOG() << "PROJECTION: requires fields\n";
                const vector<string>& fields = query.getProj()->getRequiredFields();
                bool covered = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (!solnRoot->hasField(fields[i])) {
                        QLOG() << "PROJECTION: not covered cuz doesn't have field "
                             << fields[i] << endl;
                        covered = false;
                        break;
                    }
                }
                QLOG() << "PROJECTION: is covered?: = " << covered << endl;
                // If any field is missing from the list of fields the projection wants,
                // a fetch is required.
                if (!covered) {
                    FetchNode* fetch = new FetchNode();
                    fetch->children.push_back(solnRoot);
                    solnRoot = fetch;
                }
            }

            // We now know we have whatever data is required for the projection.
            ProjectionNode* projNode = new ProjectionNode();
            projNode->children.push_back(solnRoot);
            projNode->fullExpression = query.root();
            projNode->projection = query.getParsed().getProj();
            solnRoot = projNode;
        }
        else {
            // If there's no projection, we must fetch, as the user wants the entire doc.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->children.push_back(solnRoot);
                solnRoot = fetch;
            }
        }

        if (0 != query.getParsed().getSkip()) {
            SkipNode* skip = new SkipNode();
            skip->skip = query.getParsed().getSkip();
            skip->children.push_back(solnRoot);
            solnRoot = skip;
        }

        // When there is both a blocking sort and a limit, the limit will
        // be enforced by the blocking sort.
        // Otherwise, we need to limit the results in the case of a hard limit
        // (ie. limit in raw query is negative)
        if (0 != query.getParsed().getNumToReturn() &&
            !soln->hasSortStage &&
            !query.getParsed().wantMore()) {

            LimitNode* limit = new LimitNode();
            limit->limit = query.getParsed().getNumToReturn();
            limit->children.push_back(solnRoot);
            solnRoot = limit;
        }

        soln->root.reset(solnRoot);
        return soln.release();
    }

}  // namespace mongo
