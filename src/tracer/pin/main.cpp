//! \file
/*
**  Copyright (C) - Triton
**
**  This program is under the terms of the LGPLv3 License.
*/

#include <csignal>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <pin.H>

/* libTriton */
#include <api.hpp>
#include <pythonBindings.hpp>

/* Pintool */
#include "bindings.hpp"
#include "context.hpp"
#include "snapshot.hpp"
#include "trigger.hpp"
#include "utils.hpp"



/*! \page Tracer_page Tracer
    \brief [**internal**] All information about how to plug a tracer.
\tableofcontents
\section Tracer_description Description
<hr>

The new design of the Triton library (since the `v0.3`), allows you to plug any kind of tracers. E.g: Pin,
Valgrind and even a database.

<p align="center"><img src="http://triton.quarkslab.com/files/triton_v03_architecture.png"/></p>

To use the `libTriton`, your tracer must provide two kinds of information at each program point:

- The current opcode executed.
- A state context (register and memory).

Based on these two information, Triton will translate the control flow into the \ref py_smt2lib_page representation. As an example, let assume that you have dumped
a trace into a database with all registers state and memory access - these information may come from Valgrind, Pin, Qemu or whatever. The following Python code
uses the Triton's API to build the semantics of each instruction stored in the database.

~~~~~~~~~~~~~{.py}
#!/usr/bin/env python2
## -*- coding: utf-8 -*-

import  sys
import  struct

from triton  import *
from datbase import import Manager

unpack_size = {1: 'B', 2: 'H', 4: 'I', 8: 'Q', 16: 'QQ'}

if __name__ == '__main__':

    # Set the arch
    setArchitecture(ARCH.X86_64)

    # Connect to the database
    db = Manager().connect()

    # inst_id is the instruction id into the database.
    inst_id = 1

    while True:

        # Get opcode (from database)
        opcode = db.get_opcode_from_inst_id(inst_id)
        if opcode is None:
            break

        # Get concrete register value (from database)
        regs = db.get_registers_from_inst_id(inst_id)

        # Build the Triton instruction
        inst = Instruction()

        # Setup opcodes
        inst.setOpcodes(opcode)

        # Setup Address
        inst.setAddress(regs['rip'])

        # Update concrete register state
        inst.updateContext(Register(REG.RAX,    regs['rax']))
        inst.updateContext(Register(REG.RBX,    regs['rbx']))
        inst.updateContext(Register(REG.RCX,    regs['rcx']))
        inst.updateContext(Register(REG.RDX,    regs['rdx']))
        inst.updateContext(Register(REG.RDI,    regs['rdi']))
        inst.updateContext(Register(REG.RSI,    regs['rsi']))
        inst.updateContext(Register(REG.RBP,    regs['rbp']))
        inst.updateContext(Register(REG.RSP,    regs['rsp']))
        inst.updateContext(Register(REG.RIP,    regs['rip']))
        inst.updateContext(Register(REG.R8,     regs['r8']))
        inst.updateContext(Register(REG.R9,     regs['r9']))
        inst.updateContext(Register(REG.R10,    regs['r10']))
        inst.updateContext(Register(REG.R11,    regs['r11']))
        inst.updateContext(Register(REG.R12,    regs['r12']))
        inst.updateContext(Register(REG.R13,    regs['r13']))
        inst.updateContext(Register(REG.R14,    regs['r14']))
        inst.updateContext(Register(REG.R15,    regs['r15']))
        inst.updateContext(Register(REG.RFLAGS, regs['rflags']))

        # Update concrete memory access
        accesses = db.get_memory_access_from_inst_id(inst_id)

        # Read before write
        for access in accesses:
            if access['kind'] == 'R':
                address = access['addr']
                data    = access['data']
                value   = struct.unpack(unpack_size[len(data)], data)[0]
                inst.updateContext(Memory(address, len(data), value))

        # Write after read
        for access in accesses:
            if access['kind'] == 'W':
                address = access['addr']
                data    = access['data']
                value   = struct.unpack(unpack_size[len(data)], data)[0]
                inst.updateContext(Memory(address, len(data), value))

        # Process everything (build IR, spread taint, perform simplification, ...)
        processing(inst)

        # At this point, all engines inside the Triton library were been synchronized with the concrete state.
        # Display instruction
        print inst

        # Display symbolic expressions
        for expr in inst.getSymbolicExpressions():
            print '\t', expr

        # Next instruction (from the database)
        inst_id += 1

    sys.exit(0)
~~~~~~~~~~~~~

The database connection is a pure example to show you how to interact with the Triton API. As Triton is written in `C++`, you can directly
create your Triton instruction inside a DBI engine (like Pin or Valgrind). According to your tracer, you can refer to the [Python](http://triton.quarkslab.com/documentation/doxygen/py_triton_page.html)
or the [C++](http://triton.quarkslab.com/documentation/doxygen/classtriton_1_1API.html) API. Note that this project is shippied with a pintool as tracer - Checkout the following page for more information
\ref pintool_py_api.

*/




namespace tracer {
  namespace pintool {

    //! Pin options: -script
    KNOB<std::string> KnobPythonModule(KNOB_MODE_WRITEONCE, "pintool", "script", "", "Python script");

    //! Lock / Unlock InsertCall
    Trigger analysisTrigger = Trigger();

    //! Snapshot engine
    Snapshot snapshot = Snapshot();



    /* Switch lock */
    static void toggleWrapper(bool flag) {
      PIN_LockClient();
      tracer::pintool::analysisTrigger.update(flag);
      PIN_UnlockClient();
    }


    /* Callback before instruction processing */
    static void callbackBefore(triton::arch::Instruction* tritonInst, triton::uint8* addr, triton::uint32 size, CONTEXT* ctx, THREADID threadId) {

      /* Some configurations must be applied before processing */
      tracer::pintool::callbacks::preProcessing(tritonInst, threadId);

      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Setup Triton information */
      tritonInst->partialReset();
      tritonInst->setOpcodes(addr, size);
      tritonInst->setAddress(reinterpret_cast<triton::__uint>(addr));
      tritonInst->setThreadId(reinterpret_cast<triton::uint32>(threadId));

      /* Setup the concrete context */
      tracer::pintool::setupContextRegister(tritonInst, ctx);

      /* Disassemble the instruction */
      triton::api.disassembly(*tritonInst);

      /* Trust operands */
      for (auto op = tritonInst->operands.begin(); op != tritonInst->operands.end(); op++)
        op->setTrust(true);

      /* Execute the Python callback before the IR processing */
      if (tracer::pintool::context::mustBeExecuted == false)
        tracer::pintool::callbacks::beforeIRProc(tritonInst);
      else
        tracer::pintool::context::mustBeExecuted = false;

      /* Check if we must execute a new context */
      if (tracer::pintool::context::mustBeExecuted == true) {
        tritonInst->reset();
        tracer::pintool::context::executeContext();
      }

      /* Process the IR and taint */
      triton::api.buildSemantics(*tritonInst);

      /* Execute the Python callback */
      if (tracer::pintool::context::mustBeExecuted == false)
        tracer::pintool::callbacks::before(tritonInst);

      /* Check if we must restore the snapshot */
      if (tracer::pintool::snapshot.mustBeRestored() == true) {
        tritonInst->reset();
        tracer::pintool::snapshot.restoreSnapshot(ctx);
      }

      /* Some configurations must be applied after processing */
      tracer::pintool::callbacks::postProcessing(tritonInst, threadId);

      /* Untrust operands */
      for (auto op = tritonInst->operands.begin(); op != tritonInst->operands.end(); op++)
        op->setTrust(false);

      /* Mutex */
      PIN_UnlockClient();
    }


    /* Callback after instruction processing */
    static void callbackAfter(triton::arch::Instruction* tritonInst, CONTEXT* ctx, THREADID threadId) {

      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::after(tritonInst);

      /* Some configurations must be applied after processing */
      tracer::pintool::callbacks::postProcessing(tritonInst, threadId);

      /* Clear Instruction information because of the Pin's cache */
      tritonInst->reset();

      /* Check if we must execute a new context */
      if (tracer::pintool::context::mustBeExecuted == true)
        tracer::pintool::context::executeContext();

      /* Check if we must restore the snapshot */
      if (tracer::pintool::snapshot.mustBeRestored() == true)
        tracer::pintool::snapshot.restoreSnapshot(ctx);

      /* Mutex */
      PIN_UnlockClient();
    }


    /* Save the memory access into the Triton instruction */
    static void saveMemoryAccess(triton::arch::Instruction* tritonInst, triton::__uint addr, triton::uint32 size) {
      triton::uint128 value = tracer::pintool::context::getCurrentMemoryValue(addr, size);
      tritonInst->updateContext(triton::arch::MemoryOperand(addr, size, value));
    }


    /* Callback to save bytes for the snapshot engine */
    static void callbackSnapshot(triton::__uint mem, triton::uint32 writeSize) {
      if (!tracer::pintool::analysisTrigger.getState())
      /* Analysis locked */
        return;

      /* If the snapshot is not enable we don't save the memory */
      if (tracer::pintool::snapshot.isLocked())
        return;

      /* Mutex */
      PIN_LockClient();

      for (triton::uint32 i = 0; i < writeSize ; i++)
        tracer::pintool::snapshot.addModification(mem+i, *(reinterpret_cast<triton::uint8*>(mem+i)));

      /* Mutex */
      PIN_UnlockClient();
    }


    /* Callback at a routine entry */
    static void callbackRoutineEntry(CONTEXT* ctx, THREADID threadId, PyObject* callback) {
      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex lock */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::routine(threadId, callback);

      /* Mutex unlock */
      PIN_UnlockClient();
    }


    /* Callback at a routine exit */
    static void callbackRoutineExit(CONTEXT* ctx, THREADID threadId, PyObject* callback) {
      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex lock */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::routine(threadId, callback);

      /* Mutex unlock */
      PIN_UnlockClient();
    }


    /* Callback at the end of the execution */
    static void callbackFini(int, VOID *) {
      /* Execute the Python callback */
      tracer::pintool::callbacks::fini();
    }


    /* Callback at a syscall entry */
    static void callbackSyscallEntry(unsigned int threadId, CONTEXT* ctx, SYSCALL_STANDARD std, void* v) {
      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::syscallEntry(threadId, std);

      /* Mutex */
      PIN_UnlockClient();
    }


    /* Callback at the syscall exit */
    static void callbackSyscallExit(unsigned int threadId, CONTEXT* ctx, SYSCALL_STANDARD std, void* v) {
      if (!tracer::pintool::analysisTrigger.getState() || threadId != tracer::pintool::options::targetThreadId)
      /* Analysis locked */
        return;

      /* Mutex */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::syscallExit(threadId, std);

      /* Mutex */
      PIN_UnlockClient();
    }


    /*
     * Callback when an image is loaded.
     * This callback must be called even outside the range analysis.
     */
    static void callbackImageLoad(IMG img) {
      /* Mutex */
      PIN_LockClient();

      /* Collect image's informations */
      std::string imagePath     = IMG_Name(img);
      triton::__uint imageBase  = IMG_LowAddress(img);
      triton::__uint imageSize  = (IMG_HighAddress(img) + 1) - imageBase;

      /* Execute the Python callback */
      tracer::pintool::callbacks::imageLoad(imagePath, imageBase, imageSize);

      /* Mutex */
      PIN_UnlockClient();
    }


    /* Callback when a signals occurs */
    static bool callbackSignals(unsigned int threadId, int sig, CONTEXT* ctx, bool hasHandler, const EXCEPTION_INFO* pExceptInfo, void* v) {
      /* Mutex */
      PIN_LockClient();

      /* Update CTX */
      tracer::pintool::context::lastContext = ctx;

      /* Execute the Python callback */
      tracer::pintool::callbacks::signals(threadId, sig);

      /* Mutex */
      PIN_UnlockClient();

      /*
       * We must exit. If you don't want to exit,
       * you must use the restoreSnapshot() function.
       */
      exit(0);

      return true;
    }


    /* Image instrumentation */
    static void IMG_Instrumentation(IMG img, void *v) {
      /* Lock / Unlock the Analysis from a Entry point */
      if (tracer::pintool::options::startAnalysisFromEntry) {
        tracer::pintool::options::startAnalysisFromEntry = false;
        tracer::pintool::options::startAnalysisFromAddr.insert(IMG_Entry(img));
      }

      /* Lock / Unlock the Analysis from a symbol */
      if (tracer::pintool::options::startAnalysisFromSymbol != nullptr){

        RTN targetRTN = RTN_FindByName(img, tracer::pintool::options::startAnalysisFromSymbol);
        if (RTN_Valid(targetRTN)) {
          RTN_Open(targetRTN);

          RTN_InsertCall(targetRTN,
              IPOINT_BEFORE,
              (AFUNPTR) toggleWrapper,
              IARG_BOOL, true,
              IARG_END);

          RTN_InsertCall(targetRTN,
              IPOINT_AFTER,
              (AFUNPTR) toggleWrapper,
              IARG_BOOL, false,
              IARG_END);

          RTN_Close(targetRTN);
        }
      }

      /* Callback on routine entry */
      std::map<const char *, PyObject *>::iterator it;
      for (it = tracer::pintool::options::callbackRoutineEntry.begin(); it != tracer::pintool::options::callbackRoutineEntry.end(); it++) {
        RTN targetRTN = RTN_FindByName(img, it->first);
        if (RTN_Valid(targetRTN)){
          RTN_Open(targetRTN);
          RTN_InsertCall(targetRTN, IPOINT_BEFORE, (AFUNPTR)callbackRoutineEntry, IARG_CONTEXT, IARG_THREAD_ID, IARG_PTR, it->second, IARG_END);
          RTN_Close(targetRTN);
        }
      }

      /* Callback on routine exit */
      for (it = tracer::pintool::options::callbackRoutineExit.begin(); it != tracer::pintool::options::callbackRoutineExit.end(); it++) {
        RTN targetRTN = RTN_FindByName(img, it->first);
        if (RTN_Valid(targetRTN)){
          RTN_Open(targetRTN);
          RTN_InsertCall(targetRTN, IPOINT_AFTER, (AFUNPTR)callbackRoutineExit, IARG_CONTEXT, IARG_THREAD_ID, IARG_PTR, it->second, IARG_END);
          RTN_Close(targetRTN);
        }
      }

      /*
       * Callback when a new image is loaded.
       * This callback must be called even outside the range analysis.
       */
      if (IMG_Valid(img))
        tracer::pintool::callbackImageLoad(img);
    }


    /* Check if the analysis must be unlocked */
    static bool checkUnlockAnalysis(triton::__uint address) {
      if (tracer::pintool::options::targetThreadId != -1)
        return false;

      /* Unlock the analysis at the entry point from symbol */
      if (tracer::pintool::options::startAnalysisFromSymbol != nullptr) {
        if ((RTN_FindNameByAddress(address) == tracer::pintool::options::startAnalysisFromSymbol)) {
          tracer::pintool::options::targetThreadId = PIN_ThreadId();
          tracer::pintool::toggleWrapper(true);
          return true;
        }
      }

      /* Unlock the analysis at the entry point from address */
      else if (tracer::pintool::options::startAnalysisFromAddr.find(address) != tracer::pintool::options::startAnalysisFromAddr.end()) {
          tracer::pintool::options::targetThreadId = PIN_ThreadId();
          tracer::pintool::toggleWrapper(true);
          return true;
      }

      /* Unlock the analysis at the entry point from offset */
      else if (tracer::pintool::options::startAnalysisFromOffset.find(tracer::pintool::getInsOffset(address)) != tracer::pintool::options::startAnalysisFromOffset.end()) {
          tracer::pintool::options::targetThreadId = PIN_ThreadId();
          tracer::pintool::toggleWrapper(true);
          return true;
      }
      return false;
    }


    /* Check if the instruction is blacklisted */
    static bool instructionBlacklisted(triton::__uint address) {
      std::list<const char *>::iterator it;
      for (it = tracer::pintool::options::imageBlacklist.begin(); it != tracer::pintool::options::imageBlacklist.end(); it++) {
        if (strstr(tracer::pintool::getImageName(address).c_str(), *it))
          return true;
      }
      return false;
    }


    /* Check if the instruction is whitelisted */
    static bool instructionWhitelisted(triton::__uint address) {
      std::list<const char *>::iterator it;

      /* If there is no whitelist -> jit everything */
      if (tracer::pintool::options::imageWhitelist.empty())
        return true;

      for (it = tracer::pintool::options::imageWhitelist.begin(); it != tracer::pintool::options::imageWhitelist.end(); it++) {
        if (strstr(tracer::pintool::getImageName(address).c_str(), *it))
          return true;
      }

      return false;
    }


    /* Trace instrumentation */
    static void TRACE_Instrumentation(TRACE trace, VOID *v) {

      for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {

          /* Check if the analysis me be unlocked */
          tracer::pintool::checkUnlockAnalysis(INS_Address(ins));

          if (!tracer::pintool::analysisTrigger.getState())
          /* Analysis locked */
            continue;

          if (tracer::pintool::instructionBlacklisted(INS_Address(ins)) == true || tracer::pintool::instructionWhitelisted(INS_Address(ins)) == false)
          /* Insruction blacklisted */
            continue;

          /* Prepare the Triton's instruction */
          triton::arch::Instruction* tritonInst = new triton::arch::Instruction();

          /* Save memory read1 informations */
          if (INS_IsMemoryRead(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)saveMemoryAccess,
              IARG_PTR, tritonInst,
              IARG_MEMORYREAD_EA,
              IARG_MEMORYREAD_SIZE,
              IARG_END);
          }

          /* Save memory read2 informations */
          if (INS_HasMemoryRead2(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)saveMemoryAccess,
              IARG_PTR, tritonInst,
              IARG_MEMORYREAD2_EA,
              IARG_MEMORYREAD_SIZE,
              IARG_END);
          }

          /* Save memory write informations */
          if (INS_IsMemoryWrite(ins)) {
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)saveMemoryAccess,
              IARG_PTR, tritonInst,
              IARG_MEMORYWRITE_EA,
              IARG_MEMORYWRITE_SIZE,
              IARG_END);
          }

          /* Callback before */
          INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)callbackBefore,
            IARG_PTR, tritonInst,
            IARG_INST_PTR,
            IARG_UINT32, INS_Size(ins),
            IARG_CONTEXT,
            IARG_THREAD_ID,
            IARG_END);

          /* Callback after */
          /* Syscall after context must be catcher with CALLBACK.SYSCALL_EXIT */
          if (INS_IsSyscall(ins) == false) {
            IPOINT where = IPOINT_AFTER;
            if (INS_HasFallThrough(ins) == false)
              where = IPOINT_TAKEN_BRANCH;
            INS_InsertCall(ins, where, (AFUNPTR)callbackAfter, IARG_PTR, tritonInst, IARG_CONTEXT, IARG_THREAD_ID, IARG_END);
          }

          /* I/O memory monitoring for snapshot */
          if (INS_OperandCount(ins) > 1 && INS_MemoryOperandIsWritten(ins, 0)) {
            INS_InsertCall(
              ins, IPOINT_BEFORE, (AFUNPTR)callbackSnapshot,
              IARG_MEMORYOP_EA, 0,
              IARG_UINT32, INS_MemoryWriteSize(ins),
              IARG_END);
          }

        }
      }
    }


    /* Usage function */
    static triton::sint32 Usage() {
      std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;
      return -1;
    }


    /* The pintool's entry point */
    int main(int argc, char *argv[]) {
      PIN_InitSymbols();
      PIN_SetSyntaxIntel();
      if(PIN_Init(argc, argv))
          return Usage();

      /* Init the Triton module */
      triton::bindings::python::inittriton();

      /* Image callback */
      IMG_AddInstrumentFunction(IMG_Instrumentation, nullptr);

      /* Instruction callback */
      TRACE_AddInstrumentFunction(TRACE_Instrumentation, nullptr);

      /* End instrumentation callback */
      PIN_AddFiniFunction(callbackFini, nullptr);

      /* Syscall entry callback */
      PIN_AddSyscallEntryFunction(callbackSyscallEntry, nullptr);

      /* Syscall exit callback */
      PIN_AddSyscallExitFunction(callbackSyscallExit, nullptr);

      ///* Signals callback */
      PIN_InterceptSignal(SIGHUP,  callbackSignals, nullptr);
      PIN_InterceptSignal(SIGINT,  callbackSignals, nullptr);
      PIN_InterceptSignal(SIGQUIT, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGILL,  callbackSignals, nullptr);
      PIN_InterceptSignal(SIGABRT, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGFPE,  callbackSignals, nullptr);
      PIN_InterceptSignal(SIGKILL, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGSEGV, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGPIPE, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGALRM, callbackSignals, nullptr);
      PIN_InterceptSignal(SIGTERM, callbackSignals, nullptr);

      /* Exec the Pin's python bindings */
      tracer::pintool::initBindings();
      if (!tracer::pintool::execScript(KnobPythonModule.Value().c_str()))
        throw std::runtime_error("tracer::pintool::main(): Script file can't be found.");

      return 0;
    }

  };
};


/* namespace trampoline */
int main(int argc, char *argv[]) {
  return tracer::pintool::main(argc, argv);
}
