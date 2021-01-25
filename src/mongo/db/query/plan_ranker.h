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

#pragma once

#include <memory>
#include <queue>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {

struct CandidatePlan;

/**
 * Ranks 2 or more plans.
 */
class PlanRanker {
public:
    /**
     * Returns index in 'candidates' of which plan is best.
     * Populates 'why' with information relevant to how each plan fared in the ranking process.
     * Caller owns pointers in 'why'.
     * 'candidateOrder' holds indices into candidates ordered by score (winner in first element).
     */
    static size_t pickBestPlan(const std::vector<CandidatePlan>& candidates,
                               PlanRankingDecision* why);

    /**
     * Assign the stats tree a 'goodness' score. The higher the score, the better
     * the plan. The exact value isn't meaningful except for imposing a ranking.
     */
    static double scoreTree(const PlanStageStats* stats);
};

/**
 * A container holding one to-be-ranked plan and its associated/relevant data.
 * Does not own any of its pointers.
 */
struct CandidatePlan {
    CandidatePlan(std::unique_ptr<QuerySolution> solution, PlanStage* r, WorkingSet* w)
        : solution(std::move(solution)), root(r), ws(w), failed(false) {}

    std::unique_ptr<QuerySolution> solution;
    PlanStage* root;  // Not owned here.
    WorkingSet* ws;   // Not owned here.

    // Any results produced during the plan's execution prior to ranking are retained here.
    std::queue<WorkingSetID> results;

    bool failed;
};

}  // namespace mongo
