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

namespace {

    using std::auto_ptr;
    using std::string;
    using namespace mongo;

    void encodePlanCacheKeyTree(const MatchExpression* tree, mongoutils::str::stream* os);

    /**
     * Comparator for MatchExpression nodes. nodes by:
     * 1) operator type (MatchExpression::MatchType)
     * 2) path name (MatchExpression::path())
     * 3) cache key of the subtree
     *
     * The third item is needed to break ties, thus ensuring that
     * match expression trees which should have the same cache key
     * always sort the same way. If you're wondering when the tuple
     * (operator type, path name) could ever be equal, consider this
     * query:
     *
     * {$and:[{$or:[{a:1},{a:2}]},{$or:[{b:1},{b:2}]}]}
     *
     * The two OR nodes would compare as equal in this case were it
     * not for tuple item #3 (cache key of the subtree).
     */
    bool OperatorAndFieldNameComparison(const MatchExpression* lhs, const MatchExpression* rhs) {
        // First compare by MatchType
        MatchExpression::MatchType lhsMatchType = lhs->matchType();
        MatchExpression::MatchType rhsMatchType = rhs->matchType();
        if (lhsMatchType != rhsMatchType) {
            return lhsMatchType < rhsMatchType;
        }
        // Second, path.
        StringData lhsPath = lhs->path();
        StringData rhsPath = rhs->path();
        if (lhsPath != rhsPath) {
            return lhsPath < rhsPath;
        }
        // Third, cache key.
        mongoutils::str::stream ssLeft, ssRight;
        encodePlanCacheKeyTree(lhs, &ssLeft);
        encodePlanCacheKeyTree(rhs, &ssRight);
        string strLeft(ssLeft);
        string strRight(ssRight);
        return strLeft < strRight;
    }

    /**
     * 2-character encoding of MatchExpression::MatchType.
     */
    const char* encodeMatchType(MatchExpression::MatchType mt) {
        switch(mt) {
        case MatchExpression::AND: return "an"; break;
        case MatchExpression::OR: return "or"; break;
        case MatchExpression::NOR: return "nr"; break;
        case MatchExpression::NOT: return "nt"; break;
        case MatchExpression::ALL: return "al"; break;
        case MatchExpression::ELEM_MATCH_OBJECT: return "eo"; break;
        case MatchExpression::ELEM_MATCH_VALUE: return "ev"; break;
        case MatchExpression::SIZE: return "sz"; break;
        case MatchExpression::LTE: return "le"; break;
        case MatchExpression::LT: return "lt"; break;
        case MatchExpression::EQ: return "eq"; break;
        case MatchExpression::GT: return "gt"; break;
        case MatchExpression::GTE: return "ge"; break;
        case MatchExpression::REGEX: return "re"; break;
        case MatchExpression::MOD: return "mo"; break;
        case MatchExpression::EXISTS: return "ex"; break;
        case MatchExpression::MATCH_IN: return "in"; break;
        case MatchExpression::NIN: return "ni"; break;
        case MatchExpression::TYPE_OPERATOR: return "ty"; break;
        case MatchExpression::GEO: return "go"; break;
        case MatchExpression::WHERE: return "wh"; break;
        case MatchExpression::ATOMIC: return "at"; break;
        case MatchExpression::ALWAYS_FALSE: return "af"; break;
        case MatchExpression::GEO_NEAR: return "gn"; break;
        case MatchExpression::TEXT: return "te"; break;
        }
        // Unreachable code.
        // All MatchType values have been handled in switch().
        verify(0);
        return "";
    }

    /**
     * Traverses expression tree pre-order.
     * Appends an encoding of each node's match type and path name
     * to the output stream.
     */
    void encodePlanCacheKeyTree(const MatchExpression* tree, mongoutils::str::stream* os) {
        // Encode match type and path.
        *os << encodeMatchType(tree->matchType()) << tree->path();
        // Traverse child nodes.
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            encodePlanCacheKeyTree(tree->getChild(i), os);
        }
    }

    /**
     * Encodes sort order into cache key.
     * Sort order is normalized because it provided by
     * LiteParsedQuery.
     */
    void encodePlanCacheKeySort(const BSONObj& sortObj, mongoutils::str::stream* os) {
        BSONObjIterator it(sortObj);
        while (it.more()) {
            BSONElement elt = it.next();
            // $meta text score
            if (LiteParsedQuery::isTextScoreMeta(elt)) {
                *os << "t";
            }
            // Ascending
            else if (elt.numberInt() == 1) {
                *os << "a";
            }
            // Descending
            else {
                *os << "d";
            }
            *os << elt.fieldName();
        }
    }

    /**
     * Encodes parsed projection into cache key.
     * Does a simple toString() on each projected field
     * in the BSON object.
     * This handles all the special projection types ($meta, $elemMatch, etc.)
     */
    void encodePlanCacheKeyProj(const BSONObj& projObj, mongoutils::str::stream* os) {
        if (projObj.isEmpty()) {
            return;
        }

        *os << "p";

        BSONObjIterator it(projObj);
        while (it.more()) {
            BSONElement elt = it.next();
            // BSONElement::toString() arguments
            // includeFieldName - skip field name (appending after toString() result). false.
            // full: choose less verbose representation of child/data values. false.
            *os << elt.toString(false, false);
            *os << elt.fieldName();
        }
    }

} // namespace

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

    const PlanCacheKey& CanonicalQuery::getPlanCacheKey() const {
        return _cacheKey;
    }

    void CanonicalQuery::generateCacheKey(void) {
        mongoutils::str::stream ss;
        encodePlanCacheKeyTree(_root.get(), &ss);
        encodePlanCacheKeySort(_pq->getSort(), &ss);
        encodePlanCacheKeyProj(_pq->getProj(), &ss);
        _cacheKey = ss;
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

    // static
    void CanonicalQuery::sortTree(MatchExpression* tree) {
        for (size_t i = 0; i < tree->numChildren(); ++i) {
            sortTree(tree->getChild(i));
        }
        std::vector<MatchExpression*>* children = tree->getChildVector();
        if (NULL != children) {
            std::sort(children->begin(), children->end(), OperatorAndFieldNameComparison);
        }
    }

    Status CanonicalQuery::normalize(MatchExpression* root) {
        _root.reset(normalizeTree(root));
        sortTree(_root.get());
        return isValid(_root.get());
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
        Status validStatus = this->normalize(root);
        if (!validStatus.isOK()) {
            return validStatus;
        }
        this->generateCacheKey();

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
        mongoutils::str::stream ss;
        ss << "ns=" << _pq->ns() << " limit=" << _pq->getNumToReturn()
           << " skip=" << _pq->getSkip() << '\n';
        // The expression tree puts an endl on for us.
        ss << "Tree: " << _root->toString();
        ss << "Sort: " << _pq->getSort().toString() << '\n';
        ss << "Proj: " << _pq->getProj().toString() << '\n';
        return ss;
    }

}  // namespace mongo
