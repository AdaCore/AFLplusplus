/*
   american fuzzy lop++ - LLVM LTO instrumentation pass
   ----------------------------------------------------

   Written by Marc Heuse <mh@mh-sec.de>

   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This library is plugged into LLVM when invoking clang through afl-clang-lto.

 */

#define AFL_LLVM_PASS

#include "config.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>

#include <list>
#include <string>
#include <fstream>
#include <set>

#include "llvm/Config/llvm-config.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Pass.h"
#include "llvm/IR/Constants.h"

#include "afl-llvm-common.h"

#ifndef O_DSYNC
  #define O_DSYNC O_SYNC
#endif

using namespace llvm;

namespace {

class AFLdict2filePass : public ModulePass {

 public:
  static char ID;

  AFLdict2filePass() : ModulePass(ID) {

    if (getenv("AFL_DEBUG")) debug = 1;

  }

  bool runOnModule(Module &M) override;

};

}  // namespace

bool AFLdict2filePass::runOnModule(Module &M) {

  int                              found = 0, i, j, fd;
  char                             line[MAX_AUTO_EXTRA * 8], tmp[8], *ptr;
  DenseMap<Value *, std::string *> valueMap;

  /* Show a banner */
  setvbuf(stdout, NULL, _IONBF, 0);

  if ((isatty(2) && !getenv("AFL_QUIET")) || debug) {

    SAYF(cCYA "afl-llvm-dict2file" VERSION cRST
              " by Marc \"vanHauser\" Heuse <mh@mh-sec.de>\n");

  } else

    be_quiet = 1;

  scanForDangerousFunctions(&M);

  char *dictfile = ptr = getenv("AFL_LLVM_DICT2FILE");

  if (!ptr || *ptr != '/')
    FATAL("AFL_LLVM_DICT2FILE is not set to an absolute path: %s", ptr);

  if ((fd = open(ptr, O_WRONLY | O_APPEND | O_CREAT /*| O_DSYNC*/, 0644)) < 0)
    PFATAL("Could not open/create %s.", ptr);

  /* Instrument all the things! */

  for (auto &F : M) {

    if (isIgnoreFunction(&F)) continue;

    /*  Some implementation notes.
     *
     *  We try to handle 3 cases:
     *  - memcmp("foo", arg, 3) <- literal string
     *  - static char globalvar[] = "foo";
     *    memcmp(globalvar, arg, 3) <- global variable
     *  - char localvar[] = "foo";
     *    memcmp(locallvar, arg, 3) <- local variable
     *
     *  The local variable case is the hardest. We can only detect that
     *  case if there is no reassignment or change in the variable.
     *  And it might not work across llvm version.
     *  What we do is hooking the initializer function for local variables
     *  (llvm.memcpy.p0i8.p0i8.i64) and note the string and the assigned
     *  variable. And if that variable is then used in a compare function
     *  we use that noted string.
     *  This seems not to work for tokens that have a size <= 4 :-(
     *
     *  - if the compared length is smaller than the string length we
     *    save the full string. This is likely better for fuzzing but
     *    might be wrong in a few cases depending on optimizers
     *
     *  - not using StringRef because there is a bug in the llvm 11
     *    checkout I am using which sometimes points to wrong strings
     *
     *  Over and out. Took me a full day. damn. mh/vh
     */

    for (auto &BB : F) {

      for (auto &IN : BB) {

        CallInst *callInst = nullptr;

        if ((callInst = dyn_cast<CallInst>(&IN))) {

          bool   isStrcmp = true;
          bool   isMemcmp = true;
          bool   isStrncmp = true;
          bool   isStrcasecmp = true;
          bool   isStrncasecmp = true;
          bool   isIntMemcpy = true;
          bool   addedNull = false;
          size_t optLen = 0;

          Function *Callee = callInst->getCalledFunction();
          if (!Callee) continue;
          if (callInst->getCallingConv() != llvm::CallingConv::C) continue;
          std::string FuncName = Callee->getName().str();
          isStrcmp &= !FuncName.compare("strcmp");
          isMemcmp &= !FuncName.compare("memcmp");
          isStrncmp &= !FuncName.compare("strncmp");
          isStrcasecmp &= !FuncName.compare("strcasecmp");
          isStrncasecmp &= !FuncName.compare("strncasecmp");
          isIntMemcpy &= !FuncName.compare("llvm.memcpy.p0i8.p0i8.i64");

          if (!isStrcmp && !isMemcmp && !isStrncmp && !isStrcasecmp &&
              !isStrncasecmp && !isIntMemcpy)
            continue;

          /* Verify the strcmp/memcmp/strncmp/strcasecmp/strncasecmp function
           * prototype */
          FunctionType *FT = Callee->getFunctionType();

          isStrcmp &=
              FT->getNumParams() == 2 && FT->getReturnType()->isIntegerTy(32) &&
              FT->getParamType(0) == FT->getParamType(1) &&
              FT->getParamType(0) == IntegerType::getInt8PtrTy(M.getContext());
          isStrcasecmp &=
              FT->getNumParams() == 2 && FT->getReturnType()->isIntegerTy(32) &&
              FT->getParamType(0) == FT->getParamType(1) &&
              FT->getParamType(0) == IntegerType::getInt8PtrTy(M.getContext());
          isMemcmp &= FT->getNumParams() == 3 &&
                      FT->getReturnType()->isIntegerTy(32) &&
                      FT->getParamType(0)->isPointerTy() &&
                      FT->getParamType(1)->isPointerTy() &&
                      FT->getParamType(2)->isIntegerTy();
          isStrncmp &= FT->getNumParams() == 3 &&
                       FT->getReturnType()->isIntegerTy(32) &&
                       FT->getParamType(0) == FT->getParamType(1) &&
                       FT->getParamType(0) ==
                           IntegerType::getInt8PtrTy(M.getContext()) &&
                       FT->getParamType(2)->isIntegerTy();
          isStrncasecmp &= FT->getNumParams() == 3 &&
                           FT->getReturnType()->isIntegerTy(32) &&
                           FT->getParamType(0) == FT->getParamType(1) &&
                           FT->getParamType(0) ==
                               IntegerType::getInt8PtrTy(M.getContext()) &&
                           FT->getParamType(2)->isIntegerTy();

          if (!isStrcmp && !isMemcmp && !isStrncmp && !isStrcasecmp &&
              !isStrncasecmp && !isIntMemcpy)
            continue;

          /* is a str{n,}{case,}cmp/memcmp, check if we have
           * str{case,}cmp(x, "const") or str{case,}cmp("const", x)
           * strn{case,}cmp(x, "const", ..) or strn{case,}cmp("const", x, ..)
           * memcmp(x, "const", ..) or memcmp("const", x, ..) */
          Value *Str1P = callInst->getArgOperand(0),
                *Str2P = callInst->getArgOperand(1);
          std::string Str1, Str2;
          StringRef   TmpStr;
          bool        HasStr1 = getConstantStringInfo(Str1P, TmpStr);
          if (TmpStr.empty()) {

            HasStr1 = false;

          } else {

            HasStr1 = true;
            Str1 = TmpStr.str();

          }

          bool HasStr2 = getConstantStringInfo(Str2P, TmpStr);
          if (TmpStr.empty()) {

            HasStr2 = false;

          } else {

            HasStr2 = true;
            Str2 = TmpStr.str();

          }

          if (debug)
            fprintf(stderr, "F:%s %p(%s)->\"%s\"(%s) %p(%s)->\"%s\"(%s)\n",
                    FuncName.c_str(), Str1P, Str1P->getName().str().c_str(),
                    Str1.c_str(), HasStr1 == true ? "true" : "false", Str2P,
                    Str2P->getName().str().c_str(), Str2.c_str(),
                    HasStr2 == true ? "true" : "false");

          // we handle the 2nd parameter first because of llvm memcpy
          if (!HasStr2) {

            auto *Ptr = dyn_cast<ConstantExpr>(Str2P);
            if (Ptr && Ptr->isGEPWithNoNotionalOverIndexing()) {

              if (auto *Var = dyn_cast<GlobalVariable>(Ptr->getOperand(0))) {

                if (Var->hasInitializer()) {

                  if (auto *Array =
                          dyn_cast<ConstantDataArray>(Var->getInitializer())) {

                    HasStr2 = true;
                    Str2 = Array->getAsString().str();

                  }

                }

              }

            }

          }

          // for the internal memcpy routine we only care for the second
          // parameter and are not reporting anything.
          if (isIntMemcpy == true) {

            if (HasStr2 == true) {

              Value *      op2 = callInst->getArgOperand(2);
              ConstantInt *ilen = dyn_cast<ConstantInt>(op2);
              if (ilen) {

                uint64_t literalLength = Str2.size();
                uint64_t optLength = ilen->getZExtValue();
                if (literalLength + 1 == optLength) {

                  Str2.append("\0", 1);  // add null byte
                  addedNull = true;

                }

              }

              valueMap[Str1P] = new std::string(Str2);

              if (debug)
                fprintf(stderr, "Saved: %s for %p\n", Str2.c_str(), Str1P);
              continue;

            }

            continue;

          }

          // Neither a literal nor a global variable?
          // maybe it is a local variable that we saved
          if (!HasStr2) {

            std::string *strng = valueMap[Str2P];
            if (strng && !strng->empty()) {

              Str2 = *strng;
              HasStr2 = true;
              if (debug)
                fprintf(stderr, "Filled2: %s for %p\n", strng->c_str(), Str2P);

            }

          }

          if (!HasStr1) {

            auto Ptr = dyn_cast<ConstantExpr>(Str1P);

            if (Ptr && Ptr->isGEPWithNoNotionalOverIndexing()) {

              if (auto *Var = dyn_cast<GlobalVariable>(Ptr->getOperand(0))) {

                if (Var->hasInitializer()) {

                  if (auto *Array =
                          dyn_cast<ConstantDataArray>(Var->getInitializer())) {

                    HasStr1 = true;
                    Str1 = Array->getAsString().str();

                  }

                }

              }

            }

          }

          // Neither a literal nor a global variable?
          // maybe it is a local variable that we saved
          if (!HasStr1) {

            std::string *strng = valueMap[Str1P];
            if (strng && !strng->empty()) {

              Str1 = *strng;
              HasStr1 = true;
              if (debug)
                fprintf(stderr, "Filled1: %s for %p\n", strng->c_str(), Str1P);

            }

          }

          /* handle cases of one string is const, one string is variable */
          if (!(HasStr1 ^ HasStr2)) continue;

          std::string thestring;

          if (HasStr1)
            thestring = Str1;
          else
            thestring = Str2;

          optLen = thestring.length();

          if (isMemcmp || isStrncmp || isStrncasecmp) {

            Value *      op2 = callInst->getArgOperand(2);
            ConstantInt *ilen = dyn_cast<ConstantInt>(op2);
            if (ilen) {

              uint64_t literalLength = optLen;
              optLen = ilen->getZExtValue();
              if (literalLength + 1 == optLen) {  // add null byte
                thestring.append("\0", 1);
                addedNull = true;

              }

            }

          }

          // add null byte if this is a string compare function and a null
          // was not already added
          if (!isMemcmp) {

            if (addedNull == false) {

              thestring.append("\0", 1);  // add null byte
              optLen++;

            }

            // ensure we do not have garbage
            size_t offset = thestring.find('\0', 0);
            if (offset + 1 < optLen) optLen = offset + 1;
            thestring = thestring.substr(0, optLen);

          }

          if (!be_quiet) {

            std::string outstring;
            fprintf(stderr, "%s: length %zu/%zu \"", FuncName.c_str(), optLen,
                    thestring.length());
            for (uint8_t i = 0; i < thestring.length(); i++) {

              uint8_t c = thestring[i];
              if (c <= 32 || c >= 127)
                fprintf(stderr, "\\x%02x", c);
              else
                fprintf(stderr, "%c", c);

            }

            fprintf(stderr, "\"\n");

          }

          // we take the longer string, even if the compare was to a
          // shorter part. Note that depending on the optimizer of the
          // compiler this can be wrong, but it is more likely that this
          // is helping the fuzzer
          if (optLen != thestring.length()) optLen = thestring.length();
          if (optLen > MAX_AUTO_EXTRA) optLen = MAX_AUTO_EXTRA;
          if (optLen < MIN_AUTO_EXTRA)  // too short? skip
            continue;

          ptr = (char *)thestring.c_str();
          strcpy(line, "\"");
          j = 1;
          for (i = 0; i < optLen; i++) {

            if (isprint(ptr[i])) {

              line[j++] = ptr[i];

            } else {

              if (i + 1 != optLen || ptr[i] != 0) {

                line[j] = 0;
                sprintf(tmp, "\\x%02x", ptr[i]);
                strcat(line, tmp);

              }

            }

          }

          line[j] = 0;
          strcat(line, "\"\n");
          if (write(fd, line, strlen(line)) <= 0)
            PFATAL("Could not write to dictionary file '%s' (fd=%d)", dictfile, fd);
          fsync(fd);
          found++;

        }

      }

    }

    close(fd);

  }

  /* Say something nice. */

  if (!be_quiet) {

    if (!found)
      OKF("No entries for a dictionary found.");
    else
      OKF("Wrote %d entries to the dictionary file.\n", found);

  }

  return true;

}

char AFLdict2filePass::ID = 0;

static void registerAFLdict2filePass(const PassManagerBuilder &,
                                     legacy::PassManagerBase &PM) {

  PM.add(new AFLdict2filePass());

}

static RegisterPass<AFLdict2filePass> X("afl-dict2file",
                                        "afl++ dict2file instrumentation pass",
                                        false, false);

static RegisterStandardPasses RegisterAFLdict2filePass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLdict2filePass);

