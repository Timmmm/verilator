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
// V3Sched::schedule is the top level entry-point to the scheduling algorithm
// at a high level, the process is:
//
//  - Gather and classify all logic in the design based on what triggers its execution
//  - Schedule static, initial and final logic classes in source order
//  - Break combinational cycles by introducing hybrid logic
//  - Create 'settle' region that restores the combinational invariant
//  - Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
//    All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
//    (including combinationally generated signals) are computed within the act region.
//  - Replicate combinational logic
//  - Create input combinational logic loop
//  - Create the pre/act/nba triggers
//  - Create the 'act' region evaluation function
//  - Create the 'nba' region evaluation function
//  - Bolt it all together to create the '_eval' function
//
// Details of the algorithm are described in the internals documentation docs/internals.rst
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Sched.h"

#include "V3Ast.h"
#include "V3EmitCBase.h"
#include "V3EmitV.h"
#include "V3Order.h"
#include "V3SenExprBuilder.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

namespace {

//============================================================================
// Utility functions

AstCFunc* makeSubFunction(AstNetlist* netlistp, const string& name, bool slow) {
    AstScope* const scopeTopp = netlistp->topScopep()->scopep();
    AstCFunc* const funcp = new AstCFunc{netlistp->fileline(), name, scopeTopp, ""};
    funcp->dontCombine(true);
    funcp->isStatic(false);
    funcp->isLoose(true);
    funcp->slow(slow);
    funcp->isConst(false);
    funcp->declPrivate(true);
    scopeTopp->addBlocksp(funcp);
    return funcp;
}

AstCFunc* makeTopFunction(AstNetlist* netlistp, const string& name, bool slow) {
    AstCFunc* const funcp = makeSubFunction(netlistp, name, slow);
    funcp->entryPoint(true);
    return funcp;
}

std::vector<const AstSenTree*> getSenTreesUsedBy(const std::vector<const LogicByScope*>& lbsps) {
    const VNUser1InUse user1InUse;
    std::vector<const AstSenTree*> result;
    for (const LogicByScope* const lbsp : lbsps) {
        for (const auto& pair : *lbsp) {
            AstActive* const activep = pair.second;
            AstSenTree* const senTreep = activep->sensesp();
            if (senTreep->user1SetOnce()) continue;
            if (senTreep->hasClocked() || senTreep->hasHybrid()) result.push_back(senTreep);
        }
    }
    return result;
}

AstAssign* setVar(AstVarScope* vscp, uint32_t val) {
    FileLine* const flp = vscp->fileline();
    AstVarRef* const refp = new AstVarRef{flp, vscp, VAccess::WRITE};
    AstConst* const valp = new AstConst{flp, AstConst::DTyped{}, vscp->dtypep()};
    valp->num().setLong(val);
    return new AstAssign{flp, refp, valp};
};

void remapSensitivities(const LogicByScope& lbs,
                        std::unordered_map<const AstSenTree*, AstSenTree*> senTreeMap) {
    for (const auto& pair : lbs) {
        AstActive* const activep = pair.second;
        AstSenTree* const senTreep = activep->sensesp();
        if (senTreep->hasCombo()) continue;
        activep->sensesp(senTreeMap.at(senTreep));
    }
}

void invertAndMergeSenTreeMap(
    std::unordered_map<const AstSenItem*, const AstSenTree*>& result,
    const std::unordered_map<const AstSenTree*, AstSenTree*>& senTreeMap) {
    for (const auto& pair : senTreeMap) {
        UASSERT_OBJ(!pair.second->sensesp()->nextp(), pair.second, "Should be single AstSenIem");
        result.emplace(pair.second->sensesp(), pair.first);
    }
}

//============================================================================
// Split large function according to --output-split-cfuncs

void splitCheck(AstCFunc* ofuncp) {
    if (!v3Global.opt.outputSplitCFuncs() || !ofuncp->stmtsp()) return;
    if (ofuncp->nodeCount() < v3Global.opt.outputSplitCFuncs()) return;

    int funcnum = 0;
    int func_stmts = 0;
    AstCFunc* funcp = nullptr;

    // Unlink all statements, then add item by item to new sub-functions
    AstBegin* const tempp = new AstBegin{ofuncp->fileline(), "[EditWrapper]",
                                         ofuncp->stmtsp()->unlinkFrBackWithNext()};
    // Currently we do not use finalsp in V3Sched, if we do, it needs to be handled here
    UASSERT_OBJ(!ofuncp->finalsp(), ofuncp, "Should not have any finalps");
    while (tempp->stmtsp()) {
        AstNode* const itemp = tempp->stmtsp()->unlinkFrBack();
        const int stmts = itemp->nodeCount();
        if (!funcp || (func_stmts + stmts) > v3Global.opt.outputSplitCFuncs()) {
            // Make a new function
            funcp = new AstCFunc{ofuncp->fileline(), ofuncp->name() + "__" + cvtToStr(funcnum++),
                                 ofuncp->scopep()};
            funcp->dontCombine(true);
            funcp->isStatic(false);
            funcp->isLoose(true);
            funcp->slow(ofuncp->slow());
            ofuncp->scopep()->addBlocksp(funcp);
            //
            AstCCall* const callp = new AstCCall{funcp->fileline(), funcp};
            callp->dtypeSetVoid();
            ofuncp->addStmtsp(callp->makeStmt());
            func_stmts = 0;
        }
        funcp->addStmtsp(itemp);
        func_stmts += stmts;
    }
    VL_DO_DANGLING(tempp->deleteTree(), tempp);
}

//============================================================================
// Collect and classify all logic in the design

LogicClasses gatherLogicClasses(AstNetlist* netlistp) {
    LogicClasses result;

    netlistp->foreach([&](AstScope* scopep) {
        std::vector<AstActive*> empty;

        scopep->foreach([&](AstActive* activep) {
            AstSenTree* const senTreep = activep->sensesp();
            if (!activep->stmtsp()) {
                // Some AstActives might be empty due to previous optimizations
                empty.push_back(activep);
            } else if (senTreep->hasStatic()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "static initializer with additional sensitivities");
                result.m_static.emplace_back(scopep, activep);
            } else if (senTreep->hasInitial()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'initial' logic with additional sensitivities");
                result.m_initial.emplace_back(scopep, activep);
            } else if (senTreep->hasFinal()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'final' logic with additional sensitivities");
                result.m_final.emplace_back(scopep, activep);
            } else if (senTreep->hasCombo()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "combinational logic with additional sensitivities");
                if (VN_IS(activep->stmtsp(), AlwaysPostponed)) {
                    result.m_postponed.emplace_back(scopep, activep);
                } else {
                    result.m_comb.emplace_back(scopep, activep);
                }
            } else {
                UASSERT_OBJ(senTreep->hasClocked(), activep, "What else could it be?");
                if (VN_IS(activep->stmtsp(), AlwaysObserved)) {
                    result.m_observed.emplace_back(scopep, activep);
                } else if (VN_IS(activep->stmtsp(), AlwaysReactive)) {
                    result.m_reactive.emplace_back(scopep, activep);
                } else {
                    result.m_clocked.emplace_back(scopep, activep);
                }
            }
        });

        for (AstActive* const activep : empty) activep->unlinkFrBack()->deleteTree();
    });

    return result;
}

//============================================================================
// Simple ordering in source order

void orderSequentially(AstCFunc* funcp, const LogicByScope& lbs) {
    // Create new subfunc for scope
    const auto createNewSubFuncp = [&](AstScope* const scopep) {
        const string subName{funcp->name() + "__" + scopep->nameDotless()};
        AstCFunc* const subFuncp = new AstCFunc{scopep->fileline(), subName, scopep};
        subFuncp->isLoose(true);
        subFuncp->isConst(false);
        subFuncp->declPrivate(true);
        subFuncp->slow(funcp->slow());
        scopep->addBlocksp(subFuncp);
        // Call it from the top function
        AstCCall* const callp = new AstCCall{scopep->fileline(), subFuncp};
        callp->dtypeSetVoid();
        funcp->addStmtsp(callp->makeStmt());
        return subFuncp;
    };
    const VNUser1InUse user1InUse;  // AstScope -> AstCFunc: the sub-function for the scope
    const VNUser2InUse user2InUse;  // AstScope -> int: sub-function counter used for names
    for (const auto& pair : lbs) {
        AstScope* const scopep = pair.first;
        AstActive* const activep = pair.second;
        // Create a sub-function per scope so we can V3Combine them later
        if (!scopep->user1p()) scopep->user1p(createNewSubFuncp(scopep));
        // Add statements to sub-function
        for (AstNode *logicp = activep->stmtsp(), *nextp; logicp; logicp = nextp) {
            auto* subFuncp = VN_AS(scopep->user1p(), CFunc);
            nextp = logicp->nextp();
            if (AstNodeProcedure* const procp = VN_CAST(logicp, NodeProcedure)) {
                if (AstNode* bodyp = procp->stmtsp()) {
                    bodyp->unlinkFrBackWithNext();
                    // If the process is suspendable, we need a separate function (a coroutine)
                    if (procp->isSuspendable()) {
                        funcp->slow(false);
                        subFuncp = createNewSubFuncp(scopep);
                        subFuncp->name(subFuncp->name() + "__" + cvtToStr(scopep->user2Inc()));
                        subFuncp->rtnType("VlCoroutine");
                        if (VN_IS(procp, Always)) {
                            subFuncp->slow(false);
                            FileLine* const flp = procp->fileline();
                            bodyp
                                = new AstWhile{flp, new AstConst{flp, AstConst::BitTrue{}}, bodyp};
                        }
                    }
                    subFuncp->addStmtsp(bodyp);
                }
            } else {
                logicp->unlinkFrBack();
                subFuncp->addStmtsp(logicp);
            }
        }
        if (activep->backp()) activep->unlinkFrBack();
        VL_DO_DANGLING(activep->deleteTree(), activep);
    }
}

//============================================================================
// Create simply ordered functions

void createStatic(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_static", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_static);
    splitCheck(funcp);
}

AstCFunc* createInitial(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_initial", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_initial);
    return funcp;  // Not splitting yet as it is not final
}

AstCFunc* createPostponed(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    if (logicClasses.m_postponed.empty()) return nullptr;
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_postponed", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_postponed);
    splitCheck(funcp);
    return funcp;
}

void createFinal(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_final", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_final);
    splitCheck(funcp);
}

//============================================================================
// A TriggerKit holds all the components related to a TRIGGERVEC variable

struct TriggerKit {
    // The TRIGGERVEC AstVarScope representing these trigger flags
    AstVarScope* const m_vscp;
    // The AstCFunc that computes the current active triggers
    AstCFunc* const m_funcp;
    // The AstCFunc that dumps the current active triggers
    AstCFunc* const m_dumpp;
    // The map from input sensitivity list to trigger sensitivity list
    const std::unordered_map<const AstSenTree*, AstSenTree*> m_map;

    // No VL_UNCOPYABLE(TriggerKit) as causes C++20 errors on MSVC

    // Utility that assigns the given index trigger to fire when the given variable is zero
    void addFirstIterationTriggerAssignment(AstVarScope* counterp, uint32_t index) const {
        FileLine* const flp = counterp->fileline();
        AstVarRef* const vrefp = new AstVarRef{flp, m_vscp, VAccess::WRITE};
        AstCMethodHard* const callp = new AstCMethodHard{flp, vrefp, "set"};
        callp->addPinsp(new AstConst{flp, index});
        callp->addPinsp(
            new AstEq{flp, new AstVarRef{flp, counterp, VAccess::READ}, new AstConst{flp, 0}});
        callp->dtypeSetVoid();
        m_funcp->stmtsp()->addHereThisAsNext(callp->makeStmt());
    }

    // Utility to set then clear the dpiExportTrigger trigger
    void addDpiExportTriggerAssignment(AstVarScope* dpiExportTriggerVscp, uint32_t index) const {
        FileLine* const flp = dpiExportTriggerVscp->fileline();
        AstVarRef* const vrefp = new AstVarRef{flp, m_vscp, VAccess::WRITE};
        AstCMethodHard* const callp = new AstCMethodHard{flp, vrefp, "set"};
        callp->addPinsp(new AstConst{flp, index});
        callp->addPinsp(new AstVarRef{flp, dpiExportTriggerVscp, VAccess::READ});
        callp->dtypeSetVoid();
        AstNode* const stmtp = callp->makeStmt();
        stmtp->addNext(new AstAssign{flp, new AstVarRef{flp, dpiExportTriggerVscp, VAccess::WRITE},
                                     new AstConst{flp, AstConst::BitFalse{}}});
        m_funcp->stmtsp()->addHereThisAsNext(stmtp);
    }
};

//============================================================================
// EvalKit groups items that have to be passed to createEval() for a given eval region

struct EvalKit {
    // The TRIGGERVEC AstVarScope representing the region's trigger flags
    AstVarScope* const m_vscp = nullptr;
    // The AstCFunc that computes the region's active triggers
    AstCFunc* const m_triggerComputep = nullptr;
    // The AstCFunc that dumps the region's active triggers
    AstCFunc* const m_dumpp = nullptr;
    // The AstCFunc that evaluates the region's logic
    AstCFunc* const m_funcp = nullptr;
};

// Create an AstSenTree that is sensitive to the given trigger index. Must not exist yet!
AstSenTree* createTriggerSenTree(AstNetlist* netlistp, AstVarScope* const vscp, uint32_t index) {
    UASSERT_OBJ(index != std::numeric_limits<unsigned>::max(), netlistp, "Invalid trigger index");
    AstTopScope* const topScopep = netlistp->topScopep();
    FileLine* const flp = topScopep->fileline();
    AstVarRef* const vrefp = new AstVarRef{flp, vscp, VAccess::READ};
    const uint32_t wordIndex = index / 64;
    const uint32_t bitIndex = index % 64;
    AstCMethodHard* const callp
        = new AstCMethodHard{flp, vrefp, "word", new AstConst{flp, wordIndex}};
    callp->dtypeSetUInt64();
    callp->pure(true);
    AstNodeExpr* const termp
        = new AstAnd{flp, new AstConst{flp, AstConst::Unsized64{}, 1ULL << bitIndex}, callp};
    AstSenItem* const senItemp = new AstSenItem{flp, VEdgeType::ET_TRUE, termp};
    AstSenTree* const resultp = new AstSenTree{flp, senItemp};
    topScopep->addSenTreesp(resultp);
    return resultp;
}

//============================================================================
// Utility for extra trigger allocation

class ExtraTriggers final {
    std::vector<string> m_descriptions;  // Human readable description of extra triggers

public:
    ExtraTriggers() = default;

    size_t allocate(const string& description) {
        const size_t index = m_descriptions.size();
        m_descriptions.push_back(description);
        return index;
    }
    size_t size() const { return m_descriptions.size(); }
    const string& description(size_t index) const { return m_descriptions[index]; }
};

//============================================================================
// Create a TRIGGERVEC and the related TriggerKit for the given AstSenTree vector

const TriggerKit createTriggers(AstNetlist* netlistp, AstCFunc* const initFuncp,
                                SenExprBuilder& senExprBuilder,
                                const std::vector<const AstSenTree*>& senTreeps,
                                const string& name, const ExtraTriggers& extraTriggers,
                                bool slow = false) {
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();

    std::unordered_map<const AstSenTree*, AstSenTree*> map;

    const uint32_t nTriggers = senTreeps.size() + extraTriggers.size();

    // Create the TRIGGERVEC variable
    AstBasicDType* const tDtypep
        = new AstBasicDType{flp, VBasicDTypeKwd::TRIGGERVEC, VSigning::UNSIGNED,
                            static_cast<int>(nTriggers), static_cast<int>(nTriggers)};
    netlistp->typeTablep()->addTypesp(tDtypep);
    AstVarScope* const vscp = scopeTopp->createTemp("__V" + name + "Triggered", tDtypep);

    // Create the trigger computation function
    AstCFunc* const funcp = makeSubFunction(netlistp, "_eval_triggers__" + name, slow);

    // Create the trigger dump function (for debugging, always 'slow')
    AstCFunc* const dumpp = makeSubFunction(netlistp, "_dump_triggers__" + name, true);
    dumpp->ifdef("VL_DEBUG");

    // Add a print to the dumping function if there are no triggers pending
    {
        AstCMethodHard* const callp
            = new AstCMethodHard{flp, new AstVarRef{flp, vscp, VAccess::READ}, "any"};
        callp->dtypeSetBit();
        AstIf* const ifp = new AstIf{flp, callp};
        dumpp->addStmtsp(ifp);
        ifp->addElsesp(
            new AstText{flp, "VL_DBG_MSGF(\"         No triggers active\\n\");\n", true});
    }

    // Set the given trigger to the given value
    const auto setTrig = [&](uint32_t index, AstNodeExpr* valp) {
        AstVarRef* const vrefp = new AstVarRef{flp, vscp, VAccess::WRITE};
        AstCMethodHard* const callp = new AstCMethodHard{flp, vrefp, "set"};
        callp->addPinsp(new AstConst{flp, index});
        callp->addPinsp(valp);
        callp->dtypeSetVoid();
        return callp->makeStmt();
    };

    // Create a reference to a trigger flag
    const auto getTrig = [&](uint32_t index) {
        AstVarRef* const vrefp = new AstVarRef{flp, vscp, VAccess::READ};
        const uint32_t wordIndex = index / 64;
        const uint32_t bitIndex = index % 64;
        AstCMethodHard* const callp
            = new AstCMethodHard{flp, vrefp, "word", new AstConst{flp, wordIndex}};
        callp->dtypeSetUInt64();
        callp->pure(true);
        AstNodeExpr* const termp
            = new AstAnd{flp, new AstConst{flp, AstConst::Unsized64{}, 1ULL << bitIndex}, callp};
        return termp;
    };

    // Add a debug dumping statement for this trigger
    const auto addDebug = [&](uint32_t index, const string& text = "") {
        std::stringstream ss;
        ss << "VL_DBG_MSGF(\"         '" << name << "' region trigger index " << cvtToStr(index)
           << " is active";
        if (!text.empty()) ss << ": " << text;
        ss << "\\n\");\n";
        const string message{ss.str()};

        AstIf* const ifp = new AstIf{flp, getTrig(index)};
        dumpp->addStmtsp(ifp);
        ifp->addThensp(new AstText{flp, message, true});
    };

    // Add a print for each of the extra triggers
    for (unsigned i = 0; i < extraTriggers.size(); ++i) {
        addDebug(i, "Internal '" + name + "' trigger - " + extraTriggers.description(i));
    }

    // Add trigger computation
    uint32_t triggerNumber = extraTriggers.size();
    AstNodeStmt* initialTrigsp = nullptr;
    for (const AstSenTree* const senTreep : senTreeps) {
        UASSERT_OBJ(senTreep->hasClocked() || senTreep->hasHybrid(), senTreep,
                    "Cannot create trigger expression for non-clocked sensitivity");

        // Create the trigger AstSenTrees and associate them with the original AstSenTree
        AstNodeExpr* const senp = getTrig(triggerNumber);
        AstSenItem* const senItemp = new AstSenItem{flp, VEdgeType::ET_TRUE, senp};
        AstSenTree* const trigpSenp = new AstSenTree{flp, senItemp};
        topScopep->addSenTreesp(trigpSenp);
        map[senTreep] = trigpSenp;

        // Add the trigger computation
        const auto& pair = senExprBuilder.build(senTreep);
        funcp->addStmtsp(setTrig(triggerNumber, pair.first));

        // Add initialization time trigger
        if (pair.second || v3Global.opt.xInitialEdge()) {
            initialTrigsp
                = AstNode::addNext(initialTrigsp, setTrig(triggerNumber, new AstConst{flp, 1}));
        }

        // Add a debug statement for this trigger
        std::stringstream ss;
        V3EmitV::verilogForTree(senTreep, ss);
        addDebug(triggerNumber, ss.str());

        //
        ++triggerNumber;
    }
    // Add the init and update statements
    for (AstNodeStmt* const nodep : senExprBuilder.getAndClearInits()) {
        initFuncp->addStmtsp(nodep);
    }
    for (AstNodeStmt* const nodep : senExprBuilder.getAndClearPostUpdates()) {
        funcp->addStmtsp(nodep);
    }
    const auto& preUpdates = senExprBuilder.getAndClearPreUpdates();
    if (!preUpdates.empty()) {
        for (AstNodeStmt* const nodep : vlstd::reverse_view(preUpdates)) {
            UASSERT_OBJ(funcp->stmtsp(), funcp,
                        "No statements in trigger eval function, but there are pre updates");
            funcp->stmtsp()->addHereThisAsNext(nodep);
        }
    }
    const auto& locals = senExprBuilder.getAndClearLocals();
    if (!locals.empty()) {
        UASSERT_OBJ(funcp->stmtsp(), funcp,
                    "No statements in trigger eval function, but there are locals");
        for (AstVar* const nodep : vlstd::reverse_view(locals)) {
            funcp->stmtsp()->addHereThisAsNext(nodep);
        }
    }

    // Add the initialization statements
    if (initialTrigsp) {
        AstVarScope* const tempVscp = scopeTopp->createTemp("__V" + name + "DidInit", 1);
        AstVarRef* const condp = new AstVarRef{flp, tempVscp, VAccess::READ};
        AstIf* const ifp = new AstIf{flp, new AstNot{flp, condp}};
        funcp->addStmtsp(ifp);
        ifp->branchPred(VBranchPred::BP_UNLIKELY);
        ifp->addThensp(setVar(tempVscp, 1));
        ifp->addThensp(initialTrigsp);
    }

    // Add a call to the dumping function if debug is enabled
    {
        AstTextBlock* const blockp = new AstTextBlock{flp};
        funcp->addStmtsp(blockp);
        const auto add = [&](const string& text) { blockp->addText(flp, text, true); };
        add("#ifdef VL_DEBUG\n");
        add("if (VL_UNLIKELY(vlSymsp->_vm_contextp__->debug())) {\n");
        AstCCall* const callp = new AstCCall{flp, dumpp};
        callp->dtypeSetVoid();
        blockp->addNodesp(callp->makeStmt());
        add("}\n");
        add("#endif\n");
    }

    // The debug code might leak signal names, so simply delete it when using --protect-ids
    if (v3Global.opt.protectIds()) dumpp->stmtsp()->unlinkFrBackWithNext()->deleteTree();

    return {vscp, funcp, dumpp, map};
}

//============================================================================
// Helpers to construct an evaluation loop.

AstNodeStmt* buildLoop(AstNetlist* netlistp, const string& name,
                       const std::function<void(AstVarScope*, AstWhile*)>& build)  //
{
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();
    // Create the loop condition variable
    AstVarScope* const condp = scopeTopp->createTemp("__V" + name + "Continue", 1);
    condp->varp()->noReset(true);
    // Initialize the loop condition variable to true
    AstNodeStmt* const resp = setVar(condp, 1);
    // Add the loop
    AstWhile* const loopp = new AstWhile{flp, new AstVarRef{flp, condp, VAccess::READ}};
    resp->addNext(loopp);
    // Clear the loop condition variable in the loop
    loopp->addStmtsp(setVar(condp, 0));
    // Build the body
    build(condp, loopp);
    // Done
    return resp;
};

std::pair<AstVarScope*, AstNodeStmt*> makeEvalLoop(AstNetlist* netlistp, const string& tag,
                                                   const string& name, AstVarScope* trigVscp,
                                                   AstCFunc* trigDumpp,
                                                   std::function<AstNodeStmt*()> computeTriggers,
                                                   std::function<AstNodeStmt*()> makeBody) {
    UASSERT_OBJ(trigVscp->dtypep()->basicp()->isTriggerVec(), trigVscp, "Not TRIGGERVEC");
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    FileLine* const flp = scopeTopp->fileline();

    AstVarScope* const counterp = scopeTopp->createTemp("__V" + tag + "IterCount", 32);
    counterp->varp()->noReset(true);

    AstNodeStmt* nodep = setVar(counterp, 0);
    nodep->addNext(buildLoop(netlistp, tag, [&](AstVarScope* continuep, AstWhile* loopp) {
        // Compute triggers
        loopp->addStmtsp(computeTriggers());
        // Invoke body if triggered
        {
            AstVarRef* const refp = new AstVarRef{flp, trigVscp, VAccess::READ};
            AstCMethodHard* const callp = new AstCMethodHard{flp, refp, "any"};
            callp->dtypeSetBit();
            AstIf* const ifp = new AstIf{flp, callp};
            loopp->addStmtsp(ifp);
            ifp->addThensp(setVar(continuep, 1));

            // If we exceeded the iteration limit, die
            {
                const uint32_t limit = v3Global.opt.convergeLimit();
                AstVarRef* const counterRefp = new AstVarRef{flp, counterp, VAccess::READ};
                AstConst* const constp = new AstConst{flp, AstConst::DTyped{}, counterp->dtypep()};
                constp->num().setLong(limit);
                AstNodeExpr* const condp = new AstGt{flp, counterRefp, constp};
                AstIf* const failp = new AstIf{flp, condp};
                failp->branchPred(VBranchPred::BP_UNLIKELY);
                ifp->addThensp(failp);
                AstTextBlock* const blockp = new AstTextBlock{flp};
                failp->addThensp(blockp);
                FileLine* const locp = netlistp->topModulep()->fileline();
                const string& file = VIdProtect::protect(locp->filename());
                const string& line = cvtToStr(locp->lineno());
                const auto add = [&](const string& text) { blockp->addText(flp, text, true); };
                add("#ifdef VL_DEBUG\n");
                AstCCall* const newcallp = new AstCCall{flp, trigDumpp};
                newcallp->dtypeSetVoid();
                blockp->addNodesp(newcallp->makeStmt());
                add("#endif\n");
                add("VL_FATAL_MT(\"" + file + "\", " + line + ", \"\", ");
                add("\"" + name + " region did not converge.\");\n");
            }

            // Increment iteration count
            {
                AstVarRef* const wrefp = new AstVarRef{flp, counterp, VAccess::WRITE};
                AstVarRef* const rrefp = new AstVarRef{flp, counterp, VAccess::READ};
                AstConst* const onep = new AstConst{flp, AstConst::DTyped{}, counterp->dtypep()};
                onep->num().setLong(1);
                ifp->addThensp(new AstAssign{flp, wrefp, new AstAdd{flp, rrefp, onep}});
            }

            // Add body
            ifp->addThensp(makeBody());
        }
    }));

    return {counterp, nodep};
}

//============================================================================
// Order the combinational logic to create the settle loop

void createSettle(AstNetlist* netlistp, AstCFunc* const initFuncp, SenExprBuilder& senExprBulider,
                  LogicClasses& logicClasses) {
    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval_settle", true);

    // Clone, because ordering is destructive, but we still need them for "_eval"
    LogicByScope comb = logicClasses.m_comb.clone();
    LogicByScope hybrid = logicClasses.m_hybrid.clone();

    // Nothing to do if there is no logic.
    // While this is rare in real designs, it reduces noise in small tests.
    if (comb.empty() && hybrid.empty()) return;

    // We have an extra trigger denoting this is the first iteration of the settle loop
    ExtraTriggers extraTriggers;
    const size_t firstIterationTrigger = extraTriggers.allocate("first iteration");

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&comb, &hybrid});
    const TriggerKit& trig = createTriggers(netlistp, initFuncp, senExprBulider, senTreeps, "stl",
                                            extraTriggers, true);

    // Remap sensitivities (comb has none, so only do the hybrid)
    remapSensitivities(hybrid, trig.m_map);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trig.m_map);

    // First trigger is for pure combinational triggers (first iteration)
    AstSenTree* const inputChanged
        = createTriggerSenTree(netlistp, trig.m_vscp, firstIterationTrigger);

    // Create and the body function
    AstCFunc* const stlFuncp = V3Order::order(
        netlistp, {&comb, &hybrid}, trigToSen, "stl", false, true,
        [=](const AstVarScope*, std::vector<AstSenTree*>& out) { out.push_back(inputChanged); });
    splitCheck(stlFuncp);

    // Create the eval loop
    const auto& pair = makeEvalLoop(
        netlistp, "stl", "Settle", trig.m_vscp, trig.m_dumpp,
        [&]() {  // Trigger
            AstCCall* const callp = new AstCCall{stlFuncp->fileline(), trig.m_funcp};
            callp->dtypeSetVoid();
            return callp->makeStmt();
        },
        [&]() {  // Body
            AstCCall* const callp = new AstCCall{stlFuncp->fileline(), stlFuncp};
            callp->dtypeSetVoid();
            return callp->makeStmt();
        });

    // Add the first iteration trigger to the trigger computation function
    trig.addFirstIterationTriggerAssignment(pair.first, firstIterationTrigger);

    // Add the eval loop to the top function
    funcp->addStmtsp(pair.second);
}

//============================================================================
// Order the replicated combinational logic to create the 'ico' region

AstNode* createInputCombLoop(AstNetlist* netlistp, AstCFunc* const initFuncp,
                             SenExprBuilder& senExprBuilder, LogicByScope& logic) {
    // Nothing to do if no combinational logic is sensitive to top level inputs
    if (logic.empty()) return nullptr;

    // SystemC only: Any top level inputs feeding a combinational logic must be marked,
    // so we can make them sc_sensitive
    if (v3Global.opt.systemC()) {
        logic.foreachLogic([](AstNode* logicp) {
            logicp->foreach([](AstVarRef* refp) {
                if (refp->access().isWriteOnly()) return;
                AstVarScope* const vscp = refp->varScopep();
                if (vscp->scopep()->isTop() && vscp->varp()->isNonOutput()) {
                    vscp->varp()->scSensitive(true);
                }
            });
        });
    }

    // We have some extra trigger denoting external conditions
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();

    ExtraTriggers extraTriggers;
    const size_t firstIterationTrigger = extraTriggers.allocate("first iteration");
    const size_t dpiExportTriggerIndex = dpiExportTriggerVscp
                                             ? extraTriggers.allocate("DPI export trigger")
                                             : std::numeric_limits<unsigned>::max();

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&logic});
    const TriggerKit& trig
        = createTriggers(netlistp, initFuncp, senExprBuilder, senTreeps, "ico", extraTriggers);

    if (dpiExportTriggerVscp) {
        trig.addDpiExportTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }

    // Remap sensitivities
    remapSensitivities(logic, trig.m_map);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trig.m_map);

    // The trigger top level inputs (first iteration)
    AstSenTree* const inputChanged
        = createTriggerSenTree(netlistp, trig.m_vscp, firstIterationTrigger);

    // The DPI Export trigger
    AstSenTree* const dpiExportTriggered
        = dpiExportTriggerVscp ? createTriggerSenTree(netlistp, trig.m_vscp, dpiExportTriggerIndex)
                               : nullptr;

    // Create and Order the body function
    AstCFunc* const icoFuncp
        = V3Order::order(netlistp, {&logic}, trigToSen, "ico", false, false,
                         [=](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
                             AstVar* const varp = vscp->varp();
                             if (varp->isPrimaryInish() || varp->isSigUserRWPublic()) {
                                 out.push_back(inputChanged);
                             }
                             if (varp->isWrittenByDpi()) out.push_back(dpiExportTriggered);
                         });
    splitCheck(icoFuncp);

    // Create the eval loop
    const auto& pair = makeEvalLoop(
        netlistp, "ico", "Input combinational", trig.m_vscp, trig.m_dumpp,
        [&]() {  // Trigger
            AstCCall* const callp = new AstCCall{icoFuncp->fileline(), trig.m_funcp};
            callp->dtypeSetVoid();
            return callp->makeStmt();
        },
        [&]() {  // Body
            AstCCall* const callp = new AstCCall{icoFuncp->fileline(), icoFuncp};
            callp->dtypeSetVoid();
            return callp->makeStmt();
        });

    // Add the first iteration trigger to the trigger computation function
    trig.addFirstIterationTriggerAssignment(pair.first, firstIterationTrigger);

    // Return the eval loop itself
    return pair.second;
}

//============================================================================
// Helpers for 'createEval'

AstStmtExpr* createTriggerClearCall(FileLine* const flp, AstVarScope* const vscp) {  // Trigger
    AstVarRef* const refp = new AstVarRef{flp, vscp, VAccess::WRITE};
    AstCMethodHard* const callp = new AstCMethodHard{flp, refp, "clear"};
    callp->dtypeSetVoid();
    return callp->makeStmt();
}

AstStmtExpr* createTriggerSetCall(FileLine* const flp, AstVarScope* const toVscp,
                                  AstVarScope* const fromVscp) {
    AstVarRef* const lhsp = new AstVarRef{flp, toVscp, VAccess::WRITE};
    AstVarRef* const argp = new AstVarRef{flp, fromVscp, VAccess::READ};
    AstCMethodHard* const callp = new AstCMethodHard{flp, lhsp, "thisOr", argp};
    callp->dtypeSetVoid();
    return callp->makeStmt();
}

AstStmtExpr* createTriggerAndNotCall(FileLine* const flp, AstVarScope* const lhsVscp,
                                     AstVarScope* const aVscp, AstVarScope* const bVscp) {
    AstVarRef* const lhsp = new AstVarRef{flp, lhsVscp, VAccess::WRITE};
    AstVarRef* const opap = new AstVarRef{flp, aVscp, VAccess::READ};
    AstVarRef* const opbp = new AstVarRef{flp, bVscp, VAccess::READ};
    opap->addNext(opbp);
    AstCMethodHard* const callp = new AstCMethodHard{flp, lhsp, "andNot", opap};
    callp->dtypeSetVoid();
    return callp->makeStmt();
}

//============================================================================
// Bolt together parts to create the top level _eval function

void createEval(AstNetlist* netlistp,  //
                AstNode* icoLoop,  //
                const EvalKit& actKit,  //
                AstVarScope* preTrigsp,  //
                const EvalKit& nbaKit,  //
                const EvalKit& obsKit,  //
                const EvalKit& reactKit,  //
                AstCFunc* postponedFuncp,  //
                TimingKit& timingKit  //
) {
    FileLine* const flp = netlistp->fileline();

    AstCFunc* const funcp = makeTopFunction(netlistp, "_eval", false);
    netlistp->evalp(funcp);

    // Start with the ico loop, if any
    if (icoLoop) funcp->addStmtsp(icoLoop);

    // Create the active eval loop
    AstNodeStmt* const activeEvalLoopp
        = makeEvalLoop(
              netlistp, "act", "Active", actKit.m_vscp, actKit.m_dumpp,
              [&]() {  // Trigger
                  AstNodeStmt* resultp = nullptr;

                  // Compute the current triggers
                  {
                      AstCCall* const trigsp = new AstCCall{flp, actKit.m_triggerComputep};
                      trigsp->dtypeSetVoid();
                      resultp = AstNode::addNext(resultp, trigsp->makeStmt());
                  }

                  // Commit trigger awaits from the previous iteration
                  if (AstCCall* const commitp = timingKit.createCommit(netlistp)) {
                      resultp = AstNode::addNext(resultp, commitp->makeStmt());
                  }

                  return resultp;
              },
              [&]() {  // Body
                  // Compute the pre triggers
                  AstNodeStmt* resultp
                      = createTriggerAndNotCall(flp, preTrigsp, actKit.m_vscp, nbaKit.m_vscp);
                  // Latch the active trigger flags under the NBA trigger flags
                  resultp = AstNode::addNext(
                      resultp, createTriggerSetCall(flp, nbaKit.m_vscp, actKit.m_vscp));
                  // Resume triggered timing schedulers
                  if (AstCCall* const resumep = timingKit.createResume(netlistp)) {
                      resultp = AstNode::addNext(resultp, resumep->makeStmt());
                  }
                  // Invoke body function
                  {
                      AstCCall* const callp = new AstCCall{flp, actKit.m_funcp};
                      callp->dtypeSetVoid();
                      resultp = AstNode::addNext(resultp, callp->makeStmt());
                  }

                  return resultp;
              })
              .second;

    // Create the NBA eval loop. This uses the Active eval loop in the trigger section.
    AstNodeStmt* topEvalLoopp
        = makeEvalLoop(
              netlistp, "nba", "NBA", nbaKit.m_vscp, nbaKit.m_dumpp,
              [&]() {  // Trigger
                  // Reset NBA triggers
                  AstNodeStmt* resultp = createTriggerClearCall(flp, nbaKit.m_vscp);
                  // Run the Active eval loop
                  resultp = AstNode::addNext(resultp, activeEvalLoopp);
                  return resultp;
              },
              [&]() {  // Body
                  AstCCall* const callp = new AstCCall{flp, nbaKit.m_funcp};
                  callp->dtypeSetVoid();
                  AstNodeStmt* resultp = callp->makeStmt();
                  // Latch the NBA trigger flags under the following region's trigger flags
                  AstVarScope* const nextVscp = obsKit.m_vscp ? obsKit.m_vscp : reactKit.m_vscp;
                  if (nextVscp) {
                      resultp = AstNode::addNext(
                          resultp, createTriggerSetCall(flp, nextVscp, nbaKit.m_vscp));
                  }
                  return resultp;
              })
              .second;

    if (obsKit.m_funcp) {
        // Create the Observed eval loop. This uses the NBA eval loop in the trigger section.
        topEvalLoopp
            = makeEvalLoop(
                  netlistp, "obs", "Observed", obsKit.m_vscp, obsKit.m_dumpp,
                  [&]() {  // Trigger
                      // Reset Observed triggers
                      AstNodeStmt* resultp = createTriggerClearCall(flp, obsKit.m_vscp);
                      // Run the NBA eval loop
                      resultp = AstNode::addNext(resultp, topEvalLoopp);
                      return resultp;
                  },
                  [&]() {  // Body
                      AstCCall* const callp = new AstCCall{flp, obsKit.m_funcp};
                      callp->dtypeSetVoid();
                      AstNodeStmt* resultp = callp->makeStmt();
                      // Latch the Observed trigger flags under the Reactive trigger flags
                      if (reactKit.m_vscp) {
                          resultp = AstNode::addNext(
                              resultp, createTriggerSetCall(flp, reactKit.m_vscp, obsKit.m_vscp));
                      }
                      return resultp;
                  })
                  .second;
    }

    if (reactKit.m_funcp) {
        // Create the Reactive eval loop. This uses the previous eval loop in the trigger section.
        topEvalLoopp = makeEvalLoop(
                           netlistp, "react", "Reactive", reactKit.m_vscp, reactKit.m_dumpp,
                           [&]() {  // Trigger
                               // Reset Reactive triggers
                               AstNodeStmt* resultp = createTriggerClearCall(flp, reactKit.m_vscp);
                               // Run the previous eval loop
                               resultp = AstNode::addNext(resultp, topEvalLoopp);
                               return resultp;
                           },
                           [&]() {  // Body
                               auto* const callp = new AstCCall{flp, reactKit.m_funcp};
                               callp->dtypeSetVoid();
                               return callp->makeStmt();
                           })
                           .second;
    }
    funcp->addStmtsp(topEvalLoopp);

    // Add the Postponed eval call
    if (postponedFuncp) {
        AstCCall* const callp = new AstCCall{flp, postponedFuncp};
        callp->dtypeSetVoid();
        funcp->addStmtsp(callp->makeStmt());
    }
}

}  // namespace

//============================================================================
// Top level entry-point to scheduling

void schedule(AstNetlist* netlistp) {
    const auto addSizeStat = [](const string& name, const LogicByScope& lbs) {
        uint64_t size = 0;
        lbs.foreachLogic([&](AstNode* nodep) { size += nodep->nodeCount(); });
        V3Stats::addStat("Scheduling, " + name, size);
    };

    // Step 0. Prepare timing-related logic and external domains
    auto timingKit = prepareTiming(netlistp);

    // Step 1. Gather and classify all logic in the design
    LogicClasses logicClasses = gatherLogicClasses(netlistp);

    if (v3Global.opt.stats()) {
        V3Stats::statsStage("sched-gather");
        addSizeStat("size of class: static", logicClasses.m_static);
        addSizeStat("size of class: initial", logicClasses.m_initial);
        addSizeStat("size of class: final", logicClasses.m_final);
    }

    // Step 2. Schedule static, initial and final logic classes in source order
    createStatic(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-static");

    AstCFunc* const initp = createInitial(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-initial");

    createFinal(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-final");

    // Step 3: Break combinational cycles by introducing hybrid logic
    // Note: breakCycles also removes corresponding logic from logicClasses.m_comb;
    logicClasses.m_hybrid = breakCycles(netlistp, logicClasses.m_comb);
    if (v3Global.opt.stats()) {
        addSizeStat("size of class: clocked", logicClasses.m_clocked);
        addSizeStat("size of class: combinational", logicClasses.m_comb);
        addSizeStat("size of class: hybrid", logicClasses.m_hybrid);
        V3Stats::statsStage("sched-break-cycles");
    }

    // We pass around a single SenExprBuilder instance, as we only need one set of 'prev' variables
    // for edge/change detection in sensitivity expressions, which this keeps track of.
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    SenExprBuilder senExprBuilder{scopeTopp};

    // Step 4: Create 'settle' region that restores the combinational invariant
    createSettle(netlistp, initp, senExprBuilder, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-settle");

    // Step 5: Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
    // All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
    // (including combinationally generated signals) are computed within the act region.
    LogicRegions logicRegions
        = partition(logicClasses.m_clocked, logicClasses.m_comb, logicClasses.m_hybrid);
    if (v3Global.opt.stats()) {
        addSizeStat("size of region: Active Pre", logicRegions.m_pre);
        addSizeStat("size of region: Active", logicRegions.m_act);
        addSizeStat("size of region: NBA", logicRegions.m_nba);
        V3Stats::statsStage("sched-partition");
    }

    // Step 6: Replicate combinational logic
    LogicReplicas logicReplicas = replicateLogic(logicRegions);
    if (v3Global.opt.stats()) {
        addSizeStat("size of replicated logic: Input", logicReplicas.m_ico);
        addSizeStat("size of replicated logic: Active", logicReplicas.m_act);
        addSizeStat("size of replicated logic: NBA", logicReplicas.m_nba);
        V3Stats::statsStage("sched-replicate");
    }

    // Step 7: Create input combinational logic loop
    AstNode* const icoLoopp
        = createInputCombLoop(netlistp, initp, senExprBuilder, logicReplicas.m_ico);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-ico");

    // Step 8: Create the pre/act/nba triggers
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();

    // We may have an extra trigger for variable updated in DPI exports
    ExtraTriggers extraTriggers;
    const size_t dpiExportTriggerIndex = dpiExportTriggerVscp
                                             ? extraTriggers.allocate("DPI export trigger")
                                             : std::numeric_limits<unsigned>::max();

    const auto& senTreeps = getSenTreesUsedBy({&logicRegions.m_pre,  //
                                               &logicRegions.m_act,  //
                                               &logicRegions.m_nba,  //
                                               &logicClasses.m_observed,  //
                                               &logicClasses.m_reactive,  //
                                               &timingKit.m_lbs});
    const TriggerKit& actTrig
        = createTriggers(netlistp, initp, senExprBuilder, senTreeps, "act", extraTriggers);

    // Add post updates from the timing kit
    if (timingKit.m_postUpdates) actTrig.m_funcp->addStmtsp(timingKit.m_postUpdates);

    if (dpiExportTriggerVscp) {
        actTrig.addDpiExportTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }

    AstVarScope* const actTrigVscp = actTrig.m_vscp;
    AstVarScope* const preTrigVscp = scopeTopp->createTempLike("__VpreTriggered", actTrigVscp);

    const auto cloneMapWithNewTriggerReferences
        = [=](std::unordered_map<const AstSenTree*, AstSenTree*> map, AstVarScope* vscp) {
              // Copy map
              auto newMap{map};
              // Replace references in each mapped value with a reference to the given vscp
              for (auto& pair : newMap) {
                  pair.second = pair.second->cloneTree(false);
                  pair.second->foreach([&](AstVarRef* refp) {
                      UASSERT_OBJ(refp->varScopep() == actTrigVscp, refp, "Unexpected reference");
                      UASSERT_OBJ(refp->access() == VAccess::READ, refp, "Should be read ref");
                      refp->replaceWith(new AstVarRef{refp->fileline(), vscp, VAccess::READ});
                      VL_DO_DANGLING(refp->deleteTree(), refp);
                  });
                  topScopep->addSenTreesp(pair.second);
              }
              return newMap;
          };

    const auto& actTrigMap = actTrig.m_map;
    const auto preTrigMap = cloneMapWithNewTriggerReferences(actTrigMap, preTrigVscp);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-triggers");

    // Note: Experiments so far show that running the Act (or Ico) regions on
    // multiple threads is always a net loss, so only use multi-threading for
    // NBA for now. This can be revised if evidence is available that it would
    // be beneficial

    // Step 9: Create the 'act' region evaluation function

    // Remap sensitivities of the input logic to the triggers
    remapSensitivities(logicRegions.m_pre, preTrigMap);
    remapSensitivities(logicRegions.m_act, actTrigMap);
    remapSensitivities(logicReplicas.m_act, actTrigMap);
    remapSensitivities(timingKit.m_lbs, actTrigMap);
    const auto& actTimingDomains = timingKit.remapDomains(actTrigMap);

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSenAct;
    invertAndMergeSenTreeMap(trigToSenAct, preTrigMap);
    invertAndMergeSenTreeMap(trigToSenAct, actTrigMap);

    // The DPI Export trigger AstSenTree
    AstSenTree* const dpiExportTriggeredAct
        = dpiExportTriggerVscp
              ? createTriggerSenTree(netlistp, actTrig.m_vscp, dpiExportTriggerIndex)
              : nullptr;

    AstCFunc* const actFuncp = V3Order::order(
        netlistp, {&logicRegions.m_pre, &logicRegions.m_act, &logicReplicas.m_act}, trigToSenAct,
        "act", false, false, [&](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
            auto it = actTimingDomains.find(vscp);
            if (it != actTimingDomains.end()) out = it->second;
            if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggeredAct);
        });
    splitCheck(actFuncp);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-act");

    const EvalKit& actKit = {actTrig.m_vscp, actTrig.m_funcp, actTrig.m_dumpp, actFuncp};

    // Orders a region's logic and creates the region eval function
    const auto order = [&](const std::string& name,
                           const std::vector<V3Sched::LogicByScope*>& logic) -> EvalKit {
        AstVarScope* const trigVscp
            = scopeTopp->createTempLike("__V" + name + "Triggered", actTrigVscp);
        const auto trigMap = cloneMapWithNewTriggerReferences(actTrigMap, trigVscp);
        // Remap sensitivities of the input logic to the triggers
        for (LogicByScope* lbs : logic) remapSensitivities(*lbs, trigMap);

        // Create the inverse map from trigger ref AstSenTree to original AstSenTree
        std::unordered_map<const AstSenItem*, const AstSenTree*> trigToSen;
        invertAndMergeSenTreeMap(trigToSen, trigMap);

        AstSenTree* const dpiExportTriggered
            = dpiExportTriggerVscp
                  ? createTriggerSenTree(netlistp, trigVscp, dpiExportTriggerIndex)
                  : nullptr;

        const auto& timingDomains = timingKit.remapDomains(trigMap);
        AstCFunc* const funcp = V3Order::order(
            netlistp, logic, trigToSen, name, name == "nba" && v3Global.opt.mtasks(), false,
            [&](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
                auto it = timingDomains.find(vscp);
                if (it != timingDomains.end()) out = it->second;
                if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggered);
            });

        // Create the trigger dumping function, which is the same as act trigger
        // dumping function, but referencing this region's trigger vector.
        AstCFunc* const dumpp = actTrig.m_dumpp->cloneTree(false);
        actTrig.m_dumpp->addNextHere(dumpp);
        dumpp->name("_dump_triggers__" + name);
        dumpp->foreach([&](AstVarRef* refp) {
            UASSERT_OBJ(refp->access().isReadOnly(), refp, "Should only read state");
            if (refp->varScopep() == actTrig.m_vscp) {
                refp->replaceWith(new AstVarRef{refp->fileline(), trigVscp, VAccess::READ});
                VL_DO_DANGLING(refp->deleteTree(), refp);
            }
        });
        dumpp->foreach([&](AstText* textp) {  //
            textp->text(VString::replaceWord(textp->text(), "act", name));
        });

        return {trigVscp, nullptr, dumpp, funcp};
    };

    // Step 10: Create the 'nba' region evaluation function
    const EvalKit& nbaKit = order("nba", {&logicRegions.m_nba, &logicReplicas.m_nba});
    splitCheck(nbaKit.m_funcp);
    netlistp->evalNbap(nbaKit.m_funcp);  // Remember for V3LifePost
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-nba");

    // Orders a region's logic and creates the region eval function (only if there is any logic in
    // the region)
    const auto orderIfNonEmpty = [&](const std::string& name, LogicByScope& lbs) -> EvalKit {
        if (lbs.empty()) return {};
        const auto& kit = order(name, {&lbs});
        if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-" + name);
        return kit;
    };

    // Step 11: Create the 'obs' region evaluation function
    const EvalKit& obsKit = orderIfNonEmpty("obs", logicClasses.m_observed);

    // Step 12: Create the 're' region evaluation function
    const EvalKit& reactKit = orderIfNonEmpty("react", logicClasses.m_reactive);

    // Step 13: Create the 'postponed' region evaluation function
    auto* const postponedFuncp = createPostponed(netlistp, logicClasses);

    // Step 14: Bolt it all together to create the '_eval' function
    createEval(netlistp, icoLoopp, actKit, preTrigVscp, nbaKit, obsKit, reactKit, postponedFuncp,
               timingKit);

    transformForks(netlistp);

    splitCheck(initp);

    netlistp->dpiExportTriggerp(nullptr);

    V3Global::dumpCheckGlobalTree("sched", 0, dumpTreeLevel() >= 3);
}

}  // namespace V3Sched
