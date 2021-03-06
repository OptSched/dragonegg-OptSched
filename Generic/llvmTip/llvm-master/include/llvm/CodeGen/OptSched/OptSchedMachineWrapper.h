/*******************************************************************************
Description:  A wrapper that converts an LLVM target to an OptSched
MachineModel. By default machine models are read from ini files however
MachineModelGenerator classes may supplement or override the information
contained in those ini files.
*******************************************************************************/

#ifndef OPTSCHED_MACHINE_MODEL_WRAPPER_H
#define OPTSCHED_MACHINE_MODEL_WRAPPER_H

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/OptSched/basic/machine_model.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <map>

namespace opt_sched {

class MachineModelGenerator;

// A wrapper for the OptSched MachineModel
class LLVMMachineModel : public MachineModel {
public:
  // Use a config file to initialize the machine model.
  LLVMMachineModel(const string configFile);
  // Convert information about the target machine into the
  // optimal scheduler machine model
  void convertMachineModel(const llvm::ScheduleDAGInstrs &dag,
                           const llvm::RegisterClassInfo *regClassInfo);
  // Pointer to register info for target
  const llvm::TargetRegisterInfo *registerInfo;
  MachineModelGenerator* getMMGen() { return MMGen.get(); }
  ~LLVMMachineModel() = default;

private:
  // Should a machine model be generated.
  bool shouldGenerateMM;
  // The machine model generator class.
  std::unique_ptr<MachineModelGenerator> MMGen;
};

// Generate a machine model for a specific chip.
class MachineModelGenerator {
public:
  // Generate instruction scheduling type for all instructions in the current
  // DAG that do not already have assigned instruction types.
  virtual void generateInstrType(const llvm::MachineInstr *instr) = 0;
  virtual ~MachineModelGenerator() = default;
};

// Generate a machine model for the Cortex A7. This will only generate
// instruction types. Things like issue type and issue rate must be specified
// correctly in the machine_model.cfg file. Check
// OptSchedCfg/arch/ARM_cortex_a7_machine_model.cfg for a template.
class CortexA7MMGenerator : public MachineModelGenerator {
public:
  CortexA7MMGenerator(const llvm::ScheduleDAGInstrs *dag, MachineModel *mm);
  // Generate instruction scheduling type for all instructions in the current
  // DAG by using LLVM itineraries.
  void generateInstrType(const llvm::MachineInstr *instr);
  virtual ~CortexA7MMGenerator() = default;

private:
  // Functional Units
  enum FU : unsigned {
    Pipe0 = 1,   // 00000001
    Pipe1 = 2,   // 00000010
    LSPipe = 4,  // 00000100
    NPipe = 8,   // 00001000
    NLSPipe = 16 // 00010000
  };
  const llvm::ScheduleDAGInstrs *dag;
  MachineModel *mm;
  const llvm::InstrItineraryData *iid;

  // Returns true if a machine instruction should be considered fully pipelined
  // in the machine model.
  bool isMIPipelined(const llvm::MachineInstr *inst, unsigned idx) const;
  // Find the issue type for an instruction.
  IssueType generateIssueType(const llvm::InstrStage *E) const;
};

} // end namespace opt_sched

#endif
