#include "llvm/CodeGen/OptSched/basic/graph_trans.h"
#include "llvm/CodeGen/OptSched/generic/bit_vector.h"
#include "llvm/CodeGen/OptSched/generic/logger.h"
#include "llvm/CodeGen/OptSched/basic/register.h"
#include <list>

namespace opt_sched {

GraphTrans::GraphTrans(DataDepGraph* dataDepGraph) {
  assert(dataDepGraph != NULL);

  SetDataDepGraph(dataDepGraph);
  SetNumNodesInGraph(dataDepGraph->GetInstCnt());
}

std::unique_ptr<GraphTrans> GraphTrans::CreateGraphTrans(TRANS_TYPE type, DataDepGraph* dataDepGraph) {
  switch(type) {
    // Create equivalence detection graph transformation.
    case TT_EQDECT:
      return std::unique_ptr<GraphTrans> (new EquivDectTrans(dataDepGraph));
    case TT_RPONSP:
      return std::unique_ptr<GraphTrans> (new RPOnlyNodeSupTrans(dataDepGraph));
  }
}

bool GraphTrans::AreNodesIndep_(SchedInstruction* inst1, SchedInstruction* inst2) {
 // The nodes are independent if there is no path from srcInst to dstInst.
 if (!inst1->IsRcrsvPrdcsr(inst2) && !inst1->IsRcrsvScsr(inst2)) {
  #ifdef IS_DEBUG_GRAPH_TRANS
  Logger::Info("Nodes %d and %d are independent", inst1->GetNum(), inst2->GetNum());
  #endif
  return true;
 }
 else
   return false;
}

FUNC_RESULT EquivDectTrans::ApplyTrans() {
  InstCount numNodes = GetNumNodesInGraph();
  DataDepGraph* graph = GetDataDepGraph();
  #ifdef IS_DEBUG_GRAPH_TRANS
  Logger::Info("Applying Equiv Dect trans");
  #endif

  std::list<InstCount> nodes;
  // Initialize list of nodes.
  for (InstCount n = 0; n < numNodes; n++)
    nodes.push_back(n);

  std::list<InstCount>::iterator start, next;
  start = next = nodes.begin();
  // After we add an edge between two equivalent instructions
  // it will invalidate the equal predecessor condition for
  // future nodes. Therefore we should wait until we have checked all
  // potentially equivalent nodes before adding edges between them.
  std::vector<std::pair<InstCount, InstCount>> edgesToAdd;
  while (start != nodes.end()) {
    edgesToAdd.clear();
    next++;

    while (next != nodes.end()) {
      SchedInstruction* srcInst = graph->GetInstByIndx(*start);
      SchedInstruction* dstInst = graph->GetInstByIndx(*next);
      #ifdef IS_DEBUG_GRAPH_TRANS
      Logger::Info("Checking nodes %d:%d", *start, *next);
      #endif

      if (NodesAreEquiv_(srcInst, dstInst)) {
        #ifdef IS_DEBUG_GRAPH_TRANS
        Logger::Info("Nodes %d and %d are equivalent", *start, *next);
        #endif
        edgesToAdd.push_back(std::make_pair(*start, *next));
        nodes.erase(start);
        start = next;
      }
      next++;
    }
    // Add edges, we have found all nodes that are equivalent to the original "start"
    for (InstCount i = 0; i < edgesToAdd.size(); i++) {
      SchedInstruction* from = graph->GetInstByIndx(edgesToAdd[i].first);
      SchedInstruction* to = graph->GetInstByIndx(edgesToAdd[i].second);
      #ifdef IS_DEBUG_GRAPH_TRANS
      Logger::Info("Creating edge from %d to %d", from->GetNum(), to->GetNum());
      #endif
      graph->CreateEdge(from, to, 0, DEP_OTHER);
    }
    start++;
    next = start;
  }
  return RES_SUCCESS;
}

bool EquivDectTrans::NodesAreEquiv_(SchedInstruction* srcInst, SchedInstruction* dstInst) {
  if (srcInst->GetIssueType() != dstInst->GetIssueType())
    return false;

	if (!srcInst->IsScsrEquvlnt(dstInst) || !srcInst->IsPrdcsrEquvlnt(dstInst))
      return false;

  // All tests passed return true
  return true;
}

FUNC_RESULT RPOnlyNodeSupTrans::ApplyTrans() {
  InstCount numNodes = GetNumNodesInGraph();
  DataDepGraph* graph = GetDataDepGraph();
  #ifdef IS_DEBUG_GRAPH_TRANS
  Logger::Info("Applying RP-only node sup trans");
  #endif

  for (int i = 0; i < numNodes; i++) {
    SchedInstruction* nodeA = graph->GetInstByIndx(i);
    for (int j = i+1; j < numNodes; j++) {
      if (i == j)
        continue;
      SchedInstruction* nodeB = graph->GetInstByIndx(j);

      #ifdef IS_DEBUG_GRAPH_TRANS
      Logger::Info("Checking nodes %d:%d", i, j);
      #endif
			if (NodeIsSuperior_(nodeA, nodeB)) {
        #ifdef IS_DEBUG_GRAPH_TRANS
        Logger::Info("Node %d is superior to node %d", i, j);
        #endif
				graph->CreateEdge(nodeA, nodeB, 0, DEP_OTHER);
			}
      else if (NodeIsSuperior_(nodeB, nodeA)) {
        #ifdef IS_DEBUG_GRAPH_TRANS
        Logger::Info("Node %d is superior to node %d", j, i);
        #endif
				graph->CreateEdge(nodeB, nodeA, 0, DEP_OTHER);
      }
    }
  }
  return RES_SUCCESS;
}

bool RPOnlyNodeSupTrans::NodeIsSuperior_(SchedInstruction* nodeA, SchedInstruction* nodeB) {
  DataDepGraph* graph = GetDataDepGraph();

  if (nodeA->GetIssueType() != nodeB->GetIssueType()) {
   #ifdef IS_DEBUG_GRAPH_TRANS
   Logger::Info("Node %d is not of the same issue type as node %d", nodeA->GetNum(), nodeB->GetNum());
   #endif
   return false;
  }

  if (!AreNodesIndep_(nodeA, nodeB)) {
    #ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Node %d is not independent from node %d", nodeA->GetNum(), nodeB->GetNum());
    #endif
    return false;
  }

  // The predecessor list of A must be a sub-list of the predecessor list of B.
  BitVector* predsA = nodeA->GetRcrsvNghbrBitVector(DIR_BKWRD);
  BitVector* predsB = nodeB->GetRcrsvNghbrBitVector(DIR_BKWRD);
  if (!predsA->IsSubVector(predsB)) {
    #ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Pred list of node %d is not a sub-list of the pred list of node %d", nodeA->GetNum(), nodeB->GetNum());
    #endif
    return false;
  }

  // The successor list of B must be a sub-list of the successor list of A.
  BitVector* succsA = nodeA->GetRcrsvNghbrBitVector(DIR_FRWRD);
  BitVector* succsB = nodeB->GetRcrsvNghbrBitVector(DIR_FRWRD);
  if (!succsB->IsSubVector(succsA)) {
    #ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Succ list of node %d is not a sub-list of the succ list of node %d", nodeB->GetNum(), nodeA->GetNum());
    #endif
    return false;
  }

  // For every virtual register that belongs to the Use set of B but does not belong to the Use set of A
  // there must be at least one instruction C that is distint from A nad B and belongs to the 
  // recurisve sucessor lits of both A and B.
  
  // First find registers that belong to the Use Set of B but not to the Use Set of A.
  // TODO (austin) modify wrapper code so it is easier to identify physical registers.
  Register** usesA;
  Register** usesB;
  Register** usesC;
  int useCntA = nodeA->GetUses(usesA);
  int useCntB = nodeB->GetUses(usesB);
  std::list<Register*> usesOnlyB;

  for (int i = 0; i < useCntB; i++) {
    Register* useB = usesB[i];
    // Flag for determining whether useB is used by node A.
    bool usedByA = false;
    for (int j = 0; j < useCntA; j++) {
      Register* useA = usesA[j];
      if (useA == useB) {
        usedByA = true;
        break;
      }
    }
    if (!usedByA) {
      Logger::Info("Found reg used by nodeB but not nodeA");
      usesOnlyB.push_back(useB);
    }
  }

/*
  // Find the intersection of recursive successors of A and B.
  std::unique_ptr<BitVector> succsAandB = succsA->And(succsB);
  if (succsAandB->GetOneCnt() > 0) {
    Logger::Info("Checking condition 1");
    for (int i = 0; i < succsAandB->GetSize(); i++) {
      if (succsAandB->GetBit(i)) {
        SchedInstruction* nodeC = graph->GetInstByIndx(i);
      }
    }
  }
*/
  // We have already made sure the successor list of B is a subset
  // of the successor list of A
  // TODO(austin) Maybe registers should have a unique identifier and be put in a BitVector.
  for (int i = 0; i < succsB->GetSize(); i++) {
    // Check if there are no registers in usesOnlyB or we have found a user for every register.
    if (!usesOnlyB.size())
      break;

    if (succsB->GetBit(i)) {
      SchedInstruction* nodeC = graph->GetInstByIndx(i);
      int useCntC = nodeC->GetUses(usesC);
      std::list<Register*>::iterator reg;

      // Search all uses in C for a register that uses the target registers in usesOnlyB.
			for (int j = 0; j < useCntC; j++) {
        reg = usesOnlyB.begin();

        while (reg != usesOnlyB.end()) {
          if (*reg == usesC[j]) {
            Logger::Info("Found a user \"C\"");
            reg = usesOnlyB.erase(reg);
          }
          else
            reg++;
        }
			}
    }
  }

  if (usesOnlyB.size()) {
    #ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Live range condition 1 failed");
    #endif
    return false;
  }

  // For each register type, the number of registers defined by A is less than or equal to the number of registers defined by B.
  Register** defsA;
  Register** defsB;
  int defCntA = nodeA->GetDefs(defsA);
  int defCntB = nodeB->GetDefs(defsB);
  int regTypes = graph->GetRegTypeCnt();
  Logger::Info("reg types %d", regTypes);
  vector<InstCount> regTypeDefsA(regTypes);
  vector<InstCount> regTypeDefsB(regTypes);

  for (int i = 0; i < defCntA; i++)
    regTypeDefsA[defsA[i]->GetType()]++;

  for (int i = 0; i < defCntB; i++)
    regTypeDefsB[defsB[i]->GetType()]++;

  for (int i = 0; i < regTypes; i++) {
    Logger::Info("Def count A for Type %d is %d and B is %d", i, regTypeDefsA[i], regTypeDefsB[i]);
    if (regTypeDefsA[i] > regTypeDefsB[i]) {
      #ifdef IS_DEBUG_GRAPH_TRANS
      Logger::Info("Live range condition 2 failed");
      #endif
      return false;
    }
  }

  return true;
}

} // end namespace opt_sched