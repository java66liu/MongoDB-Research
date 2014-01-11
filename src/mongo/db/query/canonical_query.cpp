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

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo {

    // static
    Status CanonicalQuery::canonicalize(const QueryMessage& qm, CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(qm, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, emptyObj, emptyObj, 0, 0, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        long long skip, long long limit,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, emptyObj, emptyObj, skip, limit, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        CanonicalQuery** out) {
        return CanonicalQuery::canonicalize(ns, query, sort, proj, 0, 0, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, emptyObj, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        const BSONObj& hint,
                                        CanonicalQuery** out) {
        BSONObj emptyObj;
        return CanonicalQuery::canonicalize(ns, query, sort, proj, skip, limit, hint,
                                            emptyObj, emptyObj, false, out);
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        const BSONObj& sort, const BSONObj& proj,
                                        long long skip, long long limit,
                                        const BSONObj& hint,
                                        const BSONObj& minObj, const BSONObj& maxObj,
                                        bool snapshot, CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, skip, limit, 0, query, proj, sort,
                                                   hint, minObj, maxObj, snapshot, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    // static
    MatchExpression* CanonicalQuery::normalizeTree(MatchExpression* root) {
        // root->isLogical() is true now.  We care about AND and OR.  Negations currently scare us.
        if (MatchExpression::AND == root->matchType() || MatchExpression::OR == root->matchType()) {
            // We could have AND of AND of AND.  Make sure we clean up our children before merging
            // them.
            // UNITTEST 11738048
            for (size_t i = 0; i < root->getChildVector()->size(); ++i) {
                (*root->getChildVector())[i] = normalizeTree(root->getChild(i));
            }

            // If any of our children are of the same logical operator that we are, we remove the
            // child's children and append them to ourselves after we examine all children.
            vector<MatchExpression*> absorbedChildren;

            for (size_t i = 0; i < root->numChildren();) {
                MatchExpression* child = root->getChild(i);
                if (child->matchType() == root->matchType()) {
                    // AND of an AND or OR of an OR.  Absorb child's children into ourself.
                    for (size_t j = 0; j < child->numChildren(); ++j) {
                        absorbedChildren.push_back(child->getChild(j));
                    }
                    // TODO(opt): this is possibly n^2-ish
                    root->getChildVector()->erase(root->getChildVector()->begin() + i);
                    child->getChildVector()->clear();
                    // Note that this only works because we cleared the child's children
                    delete child;
                    // Don't increment 'i' as the current child 'i' used to be child 'i+1'
                }
                else {
                    ++i;
                }
            }

            root->getChildVector()->insert(root->getChildVector()->end(),
                                           absorbedChildren.begin(),
                                           absorbedChildren.end());

            // AND of 1 thing is the thing, OR of 1 thing is the thing.
            if (1 == root->numChildren()) {
                MatchExpression* ret = root->getChild(0);
                root->getChildVector()->clear();
                delete root;
                return ret;
            }
        }

        return root;
    }

    size_t countNodes(MatchExpression* root, MatchExpression::MatchType type) {
        size_t sum = 0;
        if (type == root->matchType()) {
            sum = 1;
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            sum += countNodes(root->getChild(i), type);
        }
        return sum;
    }

    /**
     * Does 'root' have a subtree of type 'subtreeType' with a node of type 'childType' inside?
     */
    bool hasNodeInSubtree(MatchExpression* root, MatchExpression::MatchType childType,
                          MatchExpression::MatchType subtreeType) {
        if (subtreeType == root->matchType()) {
            return QueryPlannerCommon::hasNode(root, childType);
        }
        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (hasNodeInSubtree(root->getChild(i), childType, subtreeType)) {
                return true;
            }
        }
        return false;
    }

    // static
    Status CanonicalQuery::isValid(MatchExpression* root) {
        // Analysis below should be done after squashing the tree to make it clearer.

        // There can only be one TEXT.  If there is a TEXT, it cannot appear inside a NOR.
        //
        // Note that the query grammar (as enforced by the MatchExpression parser) forbids TEXT
        // inside of value-expression clauses like NOT, so we don't check those here.
        size_t numText = countNodes(root, MatchExpression::TEXT);
        if (numText > 1) {
            return Status(ErrorCodes::BadValue, "Too many text expressions");
        }
        else if (1 == numText) {
            if (hasNodeInSubtree(root, MatchExpression::TEXT, MatchExpression::NOR)) {
                return Status(ErrorCodes::BadValue, "text expression not allowed in nor");
            }
        }

        // There can only be one NEAR.  If there is a NEAR, it must be either the root or the root
        // must be an AND and its child must be a NEAR.
        size_t numGeoNear = countNodes(root, MatchExpression::GEO_NEAR);
        if (numGeoNear > 1) {
            return Status(ErrorCodes::BadValue, "Too many geoNear expressions");
        }
        else if (1 == numGeoNear) {
            bool topLevel = false;
            if (MatchExpression::GEO_NEAR == root->matchType()) {
                topLevel = true;
            }
            else if (MatchExpression::AND == root->matchType()) {
                for (size_t i = 0; i < root->numChildren(); ++i) {
                    if (MatchExpression::GEO_NEAR == root->getChild(i)->matchType()) {
                        topLevel = true;
                        break;
                    }
                }
            }
            if (!topLevel) {
                return Status(ErrorCodes::BadValue, "geoNear must be top-level expr");
            }
        }

        // TEXT and NEAR cannot both be in the query.
        if (numText > 0 && numGeoNear > 0) {
            return Status(ErrorCodes::BadValue, "text and geoNear not allowed in same query");
        }

        return Status::OK();
    }

    Status CanonicalQuery::init(LiteParsedQuery* lpq) {
        _pq.reset(lpq);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        MatchExpression* root = swme.getValue();
        root = normalizeTree(root);
        Status validStatus = isValid(root);
        if (!validStatus.isOK()) {
            return validStatus;
        }

        _root.reset(root);

        // Validate the projection if there is one.
        if (!_pq->getProj().isEmpty()) {
            ParsedProjection* pp;
            Status projStatus = ParsedProjection::make(_pq->getProj(), root, &pp);
            if (!projStatus.isOK()) {
                return projStatus;
            }
            _proj.reset(pp);
        }

        return Status::OK();
    }

    string CanonicalQuery::toString() const {
        stringstream ss;
        ss << "ns=" << _pq->ns() << " limit=" << _pq->getNumToReturn()
           << " skip=" << _pq->getSkip() << endl;
        // The expression tree puts an endl on for us.
        ss << "Tree: " << _root->toString();
        ss << "Sort: " << _pq->getSort().toString() << endl;
        ss << "Proj: " << _pq->getProj().toString() << endl;
        return ss.str();
    }

}  // namespace mongo
