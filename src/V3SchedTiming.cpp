// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Code scheduling
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2023 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// Functions defined in this file are used by V3Sched.cpp to properly integrate
// static scheduling with timing features. They create external domains for
// variables, remap them to trigger vectors, and create timing resume/commit
// calls for the global eval loop. There is also a function that transforms
// forks into emittable constructs.
//
// See the internals documentation docs/internals.rst for more details.
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3EmitCBase.h"
#include "V3Error.h"
#include "V3Sched.h"

#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

//============================================================================
// Remaps external domains using the specified trigger map

std::map<const AstVarScope*, std::vector<AstSenTree*>>
TimingKit::remapDomains(const std::unordered_map<const AstSenTree*, AstSenTree*>& trigMap) const {
    std::map<const AstVarScope*, std::vector<AstSenTree*>> remappedDomainMap;
    for (const auto& vscpDomains : m_externalDomains) {
        const AstVarScope* const vscp = vscpDomains.first;
        const auto& domains = vscpDomains.second;
        auto& remappedDomains = remappedDomainMap[vscp];
        remappedDomains.reserve(domains.size());
        for (AstSenTree* const domainp : domains) {
            remappedDomains.push_back(trigMap.at(domainp));
        }
    }
    return remappedDomainMap;
}

//============================================================================
// Creates a timing resume call (if needed, else returns null)

AstCCall* TimingKit::createResume(AstNetlist* const netlistp) {
    if (!m_resumeFuncp) {
        if (m_lbs.empty()) return nullptr;
        // Create global resume function
        AstScope* const scopeTopp = netlistp->topScopep()->scopep();
        m_resumeFuncp = new AstCFunc{netlistp->fileline(), "_timing_resume", scopeTopp, ""};
        m_resumeFuncp->dontCombine(true);
        m_resumeFuncp->isLoose(true);
        m_resumeFuncp->isConst(false);
        m_resumeFuncp->declPrivate(true);
        scopeTopp->addBlocksp(m_resumeFuncp);
        for (auto& p : m_lbs) {
            // Put all the timing actives in the resume function
            AstActive* const activep = p.second;
            m_resumeFuncp->addStmtsp(activep);
        }
    }
    AstCCall* const callp = new AstCCall{m_resumeFuncp->fileline(), m_resumeFuncp};
    callp->dtypeSetVoid();
    return callp;
}

//============================================================================
// Creates a timing commit call (if needed, else returns null)

AstCCall* TimingKit::createCommit(AstNetlist* const netlistp) {
    if (!m_commitFuncp) {
        for (auto& p : m_lbs) {
            AstActive* const activep = p.second;
            auto* const resumep = VN_AS(VN_AS(activep->stmtsp(), StmtExpr)->exprp(), CMethodHard);
            UASSERT_OBJ(!resumep->nextp(), resumep, "Should be the only statement here");
            AstVarScope* const schedulerp = VN_AS(resumep->fromp(), VarRef)->varScopep();
            UASSERT_OBJ(schedulerp->dtypep()->basicp()->isDelayScheduler()
                            || schedulerp->dtypep()->basicp()->isTriggerScheduler()
                            || schedulerp->dtypep()->basicp()->isDynamicTriggerScheduler(),
                        schedulerp, "Unexpected type");
            if (!schedulerp->dtypep()->basicp()->isTriggerScheduler()) continue;
            // Create the global commit function only if we have trigger schedulers
            if (!m_commitFuncp) {
                AstScope* const scopeTopp = netlistp->topScopep()->scopep();
                m_commitFuncp
                    = new AstCFunc{netlistp->fileline(), "_timing_commit", scopeTopp, ""};
                m_commitFuncp->dontCombine(true);
                m_commitFuncp->isLoose(true);
                m_commitFuncp->isConst(false);
                m_commitFuncp->declPrivate(true);
                scopeTopp->addBlocksp(m_commitFuncp);
            }
            AstSenTree* const sensesp = activep->sensesp();
            FileLine* const flp = sensesp->fileline();
            // Negate the sensitivity. We will commit only if the event wasn't triggered on the
            // current iteration
            auto* const negSensesp = sensesp->cloneTree(false);
            negSensesp->sensesp()->sensp(
                new AstLogNot{flp, negSensesp->sensesp()->sensp()->unlinkFrBack()});
            sensesp->addNextHere(negSensesp);
            auto* const newactp = new AstActive{flp, "", negSensesp};
            // Create the commit call and put it in the commit function
            auto* const commitp = new AstCMethodHard{
                flp, new AstVarRef{flp, schedulerp, VAccess::READWRITE}, "commit"};
            if (resumep->pinsp()) commitp->addPinsp(resumep->pinsp()->cloneTree(false));
            commitp->dtypeSetVoid();
            newactp->addStmtsp(commitp->makeStmt());
            m_commitFuncp->addStmtsp(newactp);
        }
        // We still haven't created a commit function (no trigger schedulers), return null
        if (!m_commitFuncp) return nullptr;
    }
    AstCCall* const callp = new AstCCall{m_commitFuncp->fileline(), m_commitFuncp};
    callp->dtypeSetVoid();
    return callp;
}

//============================================================================
// Creates the timing kit and marks variables written by suspendables

TimingKit prepareTiming(AstNetlist* const netlistp) {
    if (!v3Global.usesTiming()) return {};
    class AwaitVisitor final : public VNVisitor {
    private:
        // NODE STATE
        //  AstSenTree::user1()  -> bool.  Set true if the sentree has been visited.
        const VNUser1InUse m_inuser1;

        // STATE
        bool m_inProcess = false;  // Are we in a process?
        bool m_gatherVars = false;  // Should we gather vars in m_writtenBySuspendable?
        AstScope* const m_scopeTopp;  // Scope at the top
        LogicByScope& m_lbs;  // Timing resume actives
        AstNodeStmt*& m_postUpdatesr;  // Post updates for the trigger eval function
        // Additional var sensitivities
        std::map<const AstVarScope*, std::set<AstSenTree*>>& m_externalDomains;
        std::set<AstSenTree*> m_processDomains;  // Sentrees from the current process
        // Variables written by suspendable processes
        std::vector<AstVarScope*> m_writtenBySuspendable;

        // METHODS
        // Create an active with a timing scheduler resume() call
        void createResumeActive(AstCAwait* const awaitp) {
            auto* const methodp = VN_AS(awaitp->exprp(), CMethodHard);
            AstVarScope* const schedulerp = VN_AS(methodp->fromp(), VarRef)->varScopep();
            AstSenTree* const sensesp = awaitp->sensesp();
            FileLine* const flp = sensesp->fileline();
            // Create a resume() call on the timing scheduler
            auto* const resumep = new AstCMethodHard{
                flp, new AstVarRef{flp, schedulerp, VAccess::READWRITE}, "resume"};
            resumep->dtypeSetVoid();
            if (schedulerp->dtypep()->basicp()->isTriggerScheduler()) {
                if (methodp->pinsp()) resumep->addPinsp(methodp->pinsp()->cloneTree(false));
            } else if (schedulerp->dtypep()->basicp()->isDynamicTriggerScheduler()) {
                auto* const postp = resumep->cloneTree(false);
                postp->name("doPostUpdates");
                m_postUpdatesr = AstNode::addNext(m_postUpdatesr, postp->makeStmt());
            }
            // Put it in an active and put that in the global resume function
            auto* const activep = new AstActive{flp, "_timing", sensesp};
            activep->addStmtsp(resumep->makeStmt());
            m_lbs.emplace_back(m_scopeTopp, activep);
        }

        // VISITORS
        void visit(AstNodeProcedure* const nodep) override {
            UASSERT_OBJ(!m_inProcess && !m_gatherVars && m_processDomains.empty()
                            && m_writtenBySuspendable.empty(),
                        nodep, "Process in process?");
            m_inProcess = true;
            m_gatherVars = nodep->isSuspendable();  // Only gather vars in a suspendable
            const VNUser2InUse user2InUse;  // AstVarScope -> bool: Set true if var has been added
                                            // to m_writtenBySuspendable
            iterateChildren(nodep);
            for (AstVarScope* const vscp : m_writtenBySuspendable) {
                m_externalDomains[vscp].insert(m_processDomains.begin(), m_processDomains.end());
                vscp->varp()->setWrittenBySuspendable();
            }
            m_processDomains.clear();
            m_writtenBySuspendable.clear();
            m_inProcess = false;
            m_gatherVars = false;
        }
        void visit(AstFork* nodep) override {
            VL_RESTORER(m_gatherVars);
            if (m_inProcess) m_gatherVars = true;
            // If not in a process, we don't need to gather variables or domains
            iterateChildren(nodep);
        }
        void visit(AstCAwait* nodep) override {
            if (AstSenTree* const sensesp = nodep->sensesp()) {
                if (!sensesp->user1SetOnce()) createResumeActive(nodep);
                nodep->clearSensesp();  // Clear as these sentrees will get deleted later
                if (m_inProcess) m_processDomains.insert(sensesp);
            }
        }
        void visit(AstNodeVarRef* nodep) override {
            if (m_gatherVars && nodep->access().isWriteOrRW()
                && !nodep->varScopep()->user2SetOnce()) {
                m_writtenBySuspendable.push_back(nodep->varScopep());
            }
        }

        //--------------------
        void visit(AstNodeExpr*) override {}  // Accelerate
        void visit(AstNode* nodep) override { iterateChildren(nodep); }

    public:
        // CONSTRUCTORS
        explicit AwaitVisitor(AstNetlist* nodep, LogicByScope& lbs, AstNodeStmt*& postUpdatesr,
                              std::map<const AstVarScope*, std::set<AstSenTree*>>& externalDomains)
            : m_scopeTopp{nodep->topScopep()->scopep()}
            , m_lbs{lbs}
            , m_postUpdatesr{postUpdatesr}
            , m_externalDomains{externalDomains} {
            iterate(nodep);
        }
        ~AwaitVisitor() override = default;
    };
    LogicByScope lbs;
    AstNodeStmt* postUpdates = nullptr;
    std::map<const AstVarScope*, std::set<AstSenTree*>> externalDomains;
    AwaitVisitor{netlistp, lbs, postUpdates, externalDomains};
    return {std::move(lbs), postUpdates, std::move(externalDomains)};
}

//============================================================================
// Visits all forks and transforms their sub-statements into separate functions.

void transformForks(AstNetlist* const netlistp) {
    if (!v3Global.usesTiming()) return;
    // Transform all forked processes into functions
    class ForkVisitor final : public VNVisitor {
    private:
        // NODE STATE
        //  AstVar::user1()  -> bool.  Set true if the variable was declared before the current
        //                             fork.
        const VNUser1InUse m_inuser1;

        // STATE
        bool m_inClass = false;  // Are we in a class?
        bool m_beginHasAwaits = false;  // Does the current begin have awaits?
        AstFork* m_forkp = nullptr;  // Current fork
        AstCFunc* m_funcp = nullptr;  // Current function

        // METHODS
        // Remap local vars referenced by the given fork function
        // TODO: We should only pass variables to the fork that are
        // live in the fork body, but for that we need a proper data
        // flow analysis framework which we don't have at the moment
        void remapLocals(AstCFunc* const funcp, AstCCall* const callp) {
            const VNUser2InUse user2InUse;  // AstVarScope -> AstVarScope: var to remap to
            funcp->foreach([&](AstNodeVarRef* refp) {
                AstVar* const varp = refp->varp();
                AstBasicDType* const dtypep = varp->dtypep()->basicp();
                // If it a fork sync or an intra-assignment variable, pass it by value
                const bool passByValue = (dtypep && dtypep->isForkSync())
                                         || VString::startsWith(varp->name(), "__Vintra");
                if (passByValue) {
                    // We can just pass it to the new function
                } else if (!varp->user1() || !varp->isFuncLocal()) {
                    // Not func local, or not declared before the fork. Their lifetime is longer
                    // than the forked process. Skip
                    return;
                } else if (m_forkp->joinType().join()) {
                    // If it's fork..join, we can refer to variables from the parent process
                } else {
                    // TODO: It is possible to relax this by allowing the use of such variables up
                    // until the first await. Also, variables defined within a forked process
                    // (inside a begin) are extracted out by V3Begin, so they also trigger this
                    // error. Preventing this (or detecting such cases and moving the vars back)
                    // would also allow for using them freely.
                    refp->v3warn(E_UNSUPPORTED, "Unsupported: variable local to a forking process "
                                                "accessed in a fork..join_any or fork..join_none");
                    return;
                }
                // Remap the reference
                AstVarScope* const vscp = refp->varScopep();
                if (!vscp->user2p()) {
                    // Clone the var to the new function
                    AstVar* const newvarp
                        = new AstVar{varp->fileline(), VVarType::BLOCKTEMP, varp->name(), varp};
                    newvarp->funcLocal(true);
                    newvarp->direction(passByValue ? VDirection::INPUT : VDirection::REF);
                    funcp->addArgsp(newvarp);
                    AstVarScope* const newvscp
                        = new AstVarScope{newvarp->fileline(), funcp->scopep(), newvarp};
                    funcp->scopep()->addVarsp(newvscp);
                    vscp->user2p(newvscp);
                    callp->addArgsp(new AstVarRef{
                        refp->fileline(), vscp, passByValue ? VAccess::READ : VAccess::READWRITE});
                }
                AstVarScope* const newvscp = VN_AS(vscp->user2p(), VarScope);
                refp->varScopep(newvscp);
                refp->varp(newvscp->varp());
            });
        }

        // VISITORS
        void visit(AstNodeModule* nodep) override {
            VL_RESTORER(m_inClass);
            m_inClass = VN_IS(nodep, Class);
            iterateChildren(nodep);
        }
        void visit(AstCFunc* nodep) override {
            m_funcp = nodep;
            iterateChildren(nodep);
            m_funcp = nullptr;
        }
        void visit(AstVar* nodep) override {
            if (!m_forkp) nodep->user1(true);
        }
        void visit(AstFork* nodep) override {
            if (m_forkp) return;  // Handle forks in forks after moving them to new functions
            VL_RESTORER(m_forkp);
            m_forkp = nodep;
            iterateChildrenConst(nodep);  // Const, so we don't iterate the calls twice
            // Replace self with the function calls (no co_await, as we don't want the main
            // process to suspend whenever any of the children do)
            // V3Dead could have removed all statements from the fork, so guard against it
            AstNode* const stmtsp = nodep->stmtsp();
            if (stmtsp) nodep->addNextHere(stmtsp->unlinkFrBackWithNext());
            VL_DO_DANGLING(nodep->unlinkFrBack()->deleteTree(), nodep);
        }
        void visit(AstBegin* nodep) override {
            UASSERT_OBJ(m_forkp, nodep, "Begin outside of a fork");
            // Start with children, so later we only find awaits that are actually in this begin
            m_beginHasAwaits = false;
            iterateChildrenConst(nodep);
            if (m_beginHasAwaits) {
                UASSERT_OBJ(!nodep->name().empty(), nodep, "Begin needs a name");
                // Create a function to put this begin's statements in
                FileLine* const flp = nodep->fileline();
                AstCFunc* const newfuncp
                    = new AstCFunc{flp, nodep->name(), m_funcp->scopep(), "VlCoroutine"};
                m_funcp->addNextHere(newfuncp);
                newfuncp->isLoose(m_funcp->isLoose());
                newfuncp->slow(m_funcp->slow());
                newfuncp->isConst(m_funcp->isConst());
                newfuncp->declPrivate(true);
                // Replace the begin with a call to the newly created function
                AstCCall* const callp = new AstCCall{flp, newfuncp};
                callp->dtypeSetVoid();
                nodep->replaceWith(callp->makeStmt());
                // If we're in a class, add a vlSymsp arg
                if (m_inClass) {
                    newfuncp->addInitsp(new AstCStmt{nodep->fileline(), "VL_KEEP_THIS;\n"});
                    newfuncp->argTypes(EmitCBase::symClassVar());
                    callp->argTypes("vlSymsp");
                }
                // Put the begin's statements in the function, delete the begin
                newfuncp->addStmtsp(nodep->stmtsp()->unlinkFrBackWithNext());
                remapLocals(newfuncp, callp);
            } else {
                // No awaits, just inline the forked process
                nodep->replaceWith(nodep->stmtsp()->unlinkFrBackWithNext());
            }
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
        }
        void visit(AstCAwait* nodep) override {
            m_beginHasAwaits = true;
            iterateChildrenConst(nodep);
        }

        //--------------------
        void visit(AstNodeExpr*) override {}  // Accelerate
        void visit(AstNode* nodep) override { iterateChildren(nodep); }

    public:
        // CONSTRUCTORS
        explicit ForkVisitor(AstNetlist* nodep) { iterate(nodep); }
        ~ForkVisitor() override = default;
    };
    ForkVisitor{netlistp};
    V3Global::dumpCheckGlobalTree("sched_forks", 0, dumpTreeLevel() >= 6);
}

}  // namespace V3Sched
