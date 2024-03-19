//===--- SILGenTopLevel.cpp - Top-level Code Emission ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGenTopLevel.h"
#include "SILGenFunction.h"
#include "Scope.h"
#include "swift/AST/DiagnosticsSIL.h"

#define DEBUG_TYPE "silgen"

using namespace swift;
using namespace Lowering;

static FuncDecl *synthesizeExit(ASTContext &ctx, ModuleDecl *moduleDecl) {
  // Synthesize an exit function with this interface.
  // @_extern(c)
  // func exit(_: Int32) -> Never
  ParameterList *params =
      ParameterList::createWithoutLoc(ParamDecl::createImplicit(
          ctx, Identifier(), Identifier(), ctx.getInt32Type(), moduleDecl));
  FuncDecl *exitFuncDecl = FuncDecl::createImplicit(
      ctx, StaticSpellingKind::None,
      DeclName(ctx, DeclBaseName(ctx.getIdentifier("exit")), params), {},
      /*async*/ false, /*throws*/ false, /*thrownType*/ Type(), {}, params,
      ctx.getNeverType(), moduleDecl);
  exitFuncDecl->getAttrs().add(new (ctx) ExternAttr(
      std::nullopt, std::nullopt, ExternKind::C, /*implicit*/ true));
  return exitFuncDecl;
}

void SILGenModule::emitEntryPoint(SourceFile *SF, SILFunction *TopLevel) {

  auto EntryRef = SILDeclRef::getMainFileEntryPoint(SF);
  bool isAsyncTopLevel = false;
  if (SF->isAsyncContext()) {
    isAsyncTopLevel = true;
    auto asyncEntryRef = SILDeclRef::getAsyncMainFileEntryPoint(SF);
    auto *asyncTopLevel = getFunction(asyncEntryRef, ForDefinition);
    SILGenFunction(*this, *TopLevel, SF)
        .emitAsyncMainThreadStart(asyncEntryRef);
    TopLevel = asyncTopLevel;
    EntryRef = asyncEntryRef;
  }

  TopLevel->createProfiler(EntryRef);

  SILGenFunction TopLevelSGF(*this, *TopLevel, SF,
                             /* IsEmittingTopLevelCode */ true);
  TopLevelSGF.MagicFunctionName = SwiftModule->getName();
  auto moduleCleanupLoc = CleanupLocation::getModuleCleanupLocation();

  TopLevelSGF.prepareEpilog(SF, std::nullopt,
                            getASTContext().getErrorExistentialType(),
                            moduleCleanupLoc);

  auto prologueLoc = RegularLocation::getModuleLocation();
  prologueLoc.markAsPrologue();
  if (SF->isAsyncContext()) {
    // emitAsyncMainThreadStart will create argc and argv.
    // Just set the main actor as the expected executor; we should
    // already be running on it.
    SILValue executor = TopLevelSGF.emitMainExecutor(prologueLoc);
    TopLevelSGF.ExpectedExecutor = TopLevelSGF.B.createOptionalSome(
        prologueLoc, executor, SILType::getOptionalType(executor->getType()));
  } else {
    // Create the argc and argv arguments.
    auto entry = TopLevelSGF.B.getInsertionBB();
    auto context = TopLevelSGF.getTypeExpansionContext();
    auto paramTypeIter =
        TopLevelSGF.F.getConventions().getParameterSILTypes(context).begin();

    entry->createFunctionArgument(*paramTypeIter);
    entry->createFunctionArgument(*std::next(paramTypeIter));
  }

  {
    Scope S(TopLevelSGF.Cleanups, moduleCleanupLoc);
    SILGenTopLevel(TopLevelSGF).visitSourceFile(SF);
  }

  // Unregister the top-level function emitter.
  TopLevelSGF.stopEmittingTopLevelCode();

  // Write out the epilog.
  auto moduleLoc = RegularLocation::getModuleLocation();
  moduleLoc.markAutoGenerated();
  auto returnInfo = TopLevelSGF.emitEpilogBB(moduleLoc);
  auto returnLoc = returnInfo.second;
  returnLoc.markAutoGenerated();

  SILFunction *exitFunc = nullptr;

  SILType returnType;
  if (isAsyncTopLevel) {
    FuncDecl *exitFuncDecl = getExit();
    if (!exitFuncDecl) {
      // If it doesn't exist, we can conjure one up instead of crashing
      exitFuncDecl = synthesizeExit(getASTContext(), TopLevel->getModule().getSwiftModule());
    }
    exitFunc = getFunction(
        SILDeclRef(exitFuncDecl, SILDeclRef::Kind::Func, /*isForeign*/ true),
        NotForDefinition);
    SILFunctionType &funcType =
        *exitFunc->getLoweredType().getAs<SILFunctionType>();
    returnType = SILType::getPrimitiveObjectType(
        funcType.getParameters().front().getInterfaceType());
  } else {
    returnType = TopLevelSGF.F.getConventions().getSingleSILResultType(
        TopLevelSGF.getTypeExpansionContext());
  }

  auto emitTopLevelReturnValue = [&](unsigned value) -> SILValue {
    // Create an integer literal for the value.
    auto litType = SILType::getBuiltinIntegerType(32, getASTContext());
    SILValue retValue =
        TopLevelSGF.B.createIntegerLiteral(moduleLoc, litType, value);

    // Wrap that in a struct if necessary.
    if (litType != returnType) {
      retValue = TopLevelSGF.B.createStruct(moduleLoc, returnType, retValue);
    }
    return retValue;
  };

  // Fallthrough should signal a normal exit by returning 0.
  SILValue returnValue;
  if (TopLevelSGF.B.hasValidInsertionPoint())
    returnValue = emitTopLevelReturnValue(0);

  // Handle the implicit rethrow block.
  auto rethrowBB = TopLevelSGF.ThrowDest.getBlock();
  TopLevelSGF.ThrowDest = JumpDest::invalid();

  // If the rethrow block wasn't actually used, just remove it.
  if (rethrowBB->pred_empty()) {
    TopLevelSGF.eraseBasicBlock(rethrowBB);

    // Otherwise, we need to produce a unified return block.
  } else {
    auto returnBB = TopLevelSGF.createBasicBlock();
    if (TopLevelSGF.B.hasValidInsertionPoint())
      TopLevelSGF.B.createBranch(returnLoc, returnBB, returnValue);
    returnValue = returnBB->createPhiArgument(returnType, OwnershipKind::Owned);
    TopLevelSGF.B.emitBlock(returnBB);

    // Emit the rethrow block.
    SILGenSavedInsertionPoint savedIP(TopLevelSGF, rethrowBB,
                                      FunctionSection::Postmatter);

    // Log the error.
    SILValue error = rethrowBB->getArgument(0);
    TopLevelSGF.B.createBuiltin(moduleLoc,
                                getASTContext().getIdentifier("errorInMain"),
                                Types.getEmptyTupleType(), {}, {error});

    // Then end the lifetime of the error.
    //
    // We do this to appease the ownership verifier. We do not care about
    // actually destroying the value since we are going to immediately exit,
    // so this saves us a slight bit of code-size since end_lifetime is
    // stripped out after ownership is removed.
    TopLevelSGF.B.createEndLifetime(moduleLoc, error);

    // Signal an abnormal exit by returning 1.
    TopLevelSGF.Cleanups.emitCleanupsForReturn(CleanupLocation(moduleLoc),
                                               IsForUnwind);
    TopLevelSGF.B.createBranch(returnLoc, returnBB, emitTopLevelReturnValue(1));
  }

  // Return.
  if (TopLevelSGF.B.hasValidInsertionPoint()) {

    if (isAsyncTopLevel) {
      SILValue exitCall = TopLevelSGF.B.createFunctionRef(moduleLoc, exitFunc);
      TopLevelSGF.B.createApply(moduleLoc, exitCall, {}, {returnValue});
      TopLevelSGF.B.createUnreachable(moduleLoc);
    } else {
      TopLevelSGF.B.createReturn(returnLoc, returnValue);
    }
  }

  // Okay, we're done emitting the top-level function; destroy the
  // emitter and verify the result.
  SILFunction &toplevel = TopLevelSGF.getFunction();

  LLVM_DEBUG(llvm::dbgs() << "lowered toplevel sil:\n";
             toplevel.print(llvm::dbgs()));
  toplevel.verifyIncompleteOSSA();
  emitLazyConformancesForFunction(&toplevel);
}

/// Generate code for calling the given main function.
void SILGenFunction::emitCallToMain(FuncDecl *mainFunc) {
  // This function is effectively emitting SIL for:
  //   return try await TheType.$main();
  auto loc = SILLocation(mainFunc);
  auto *entryBlock = B.getInsertionBB();

  SILDeclRef mainFunctionDeclRef(mainFunc, SILDeclRef::Kind::Func);
  SILFunction *mainFunction =
      SGM.getFunction(mainFunctionDeclRef, NotForDefinition);

  NominalTypeDecl *mainType =
      mainFunc->getDeclContext()->getSelfNominalTypeDecl();
  auto metatype = B.createMetatype(mainType, getLoweredType(mainType->getInterfaceType()));

  auto mainFunctionRef = B.createFunctionRef(loc, mainFunction);

  auto builtinInt32Type = SILType::getBuiltinIntegerType(
      32, getASTContext());

  // Set up the exit block, which will either return the exit value
  // (for synchronous main()) or call exit() with the return value (for
  // asynchronous main()).
  auto *exitBlock = createBasicBlock();
  SILValue exitCode =
      exitBlock->createPhiArgument(builtinInt32Type, OwnershipKind::None);
  B.setInsertionPoint(exitBlock);

  if (!mainFunc->hasAsync()) {
    auto returnType = F.getConventions().getSingleSILResultType(
        B.getTypeExpansionContext());
    if (exitCode->getType() != returnType)
      exitCode = B.createStruct(loc, returnType, exitCode);
    B.createReturn(loc, exitCode);
  } else {
    FuncDecl *exitFuncDecl = SGM.getExit();
    if (!exitFuncDecl) {
      // If it doesn't exist, we can conjure one up instead of crashing
      exitFuncDecl = synthesizeExit(getASTContext(), mainFunc->getModuleContext());
    }
    SILFunction *exitSILFunc = SGM.getFunction(
        SILDeclRef(exitFuncDecl, SILDeclRef::Kind::Func, /*isForeign*/ true),
        NotForDefinition);

    SILFunctionType &funcType =
        *exitSILFunc->getLoweredType().getAs<SILFunctionType>();
    SILType retType = SILType::getPrimitiveObjectType(
        funcType.getParameters().front().getInterfaceType());
    exitCode = B.createStruct(loc, retType, exitCode);
    SILValue exitCall = B.createFunctionRef(loc, exitSILFunc);
    B.createApply(loc, exitCall, {}, {exitCode});
    B.createUnreachable(loc);
  }

  // Form a call to the main function.
  CanSILFunctionType mainFnType = mainFunction->getConventions().funcTy;
  ASTContext &ctx = getASTContext();
  if (mainFnType->hasErrorResult()) {
    auto *successBlock = createBasicBlock();
    B.setInsertionPoint(successBlock);
    successBlock->createPhiArgument(SGM.Types.getEmptyTupleType(),
                                    OwnershipKind::None);
    SILValue zeroReturnValue =
        B.createIntegerLiteral(loc, builtinInt32Type, 0);
    B.createBranch(loc, exitBlock, {zeroReturnValue});

    SILResultInfo errorResult = mainFnType->getErrorResult();
    SILType errorType = errorResult.getSILStorageInterfaceType();

    auto *failureBlock = createBasicBlock();
    B.setInsertionPoint(failureBlock);
    SILValue error;
    if (IndirectErrorResult) {
      error = IndirectErrorResult;
    } else {
      error = failureBlock->createPhiArgument(
          errorType, OwnershipKind::Owned);
    }

    // Log the error.
    if (errorType.getASTType()->isErrorExistentialType()) {
      // Load the indirect error, if needed.
      if (IndirectErrorResult) {
        const TypeLowering &errorExistentialTL = getTypeLowering(errorType);

        error = emitLoad(
           loc, IndirectErrorResult, errorExistentialTL, SGFContext(),
          IsTake).forward(*this);
      }

      // Call the errorInMain entrypoint, which takes an existential
      // error.
      B.createBuiltin(loc, ctx.getIdentifier("errorInMain"),
                      SGM.Types.getEmptyTupleType(), {}, {error});
    } else {
      // Call the _errorInMainTyped entrypoint, which handles
      // arbitrary error types.
      SILValue tmpBuffer;

      FuncDecl *entrypoint = ctx.getErrorInMainTyped();
      auto genericSig = entrypoint->getGenericSignature();
      SubstitutionMap subMap = SubstitutionMap::get(
          genericSig, [&](SubstitutableType *dependentType) {
            return errorType.getASTType();
          }, LookUpConformanceInModule(getModule().getSwiftModule()));

      // Generic errors are passed indirectly.
      if (!error->getType().isAddress()) {
        auto *tmp = B.createAllocStack(loc, error->getType().getObjectType(),
                                       std::nullopt);
        emitSemanticStore(
            loc, error, tmp,
            getTypeLowering(tmp->getType()), IsInitialization);
        tmpBuffer = tmp;
        error = tmp;
      }

      emitApplyOfLibraryIntrinsic(
          loc, entrypoint, subMap,
          { ManagedValue::forForwardedRValue(*this, error) },
          SGFContext());
    }
    B.createUnreachable(loc);

    B.setInsertionPoint(entryBlock);
    B.createTryApply(loc, mainFunctionRef, SubstitutionMap(),
                     {metatype}, successBlock, failureBlock);
  } else {
    B.setInsertionPoint(entryBlock);
    B.createApply(loc, mainFunctionRef, SubstitutionMap(), {metatype});
    SILValue returnValue =
        B.createIntegerLiteral(loc, builtinInt32Type, 0);
    B.createBranch(loc, exitBlock, {returnValue});
  }
}

void SILGenModule::emitEntryPoint(SourceFile *SF) {
  assert(!M.lookUpFunction(getASTContext().getEntryPointFunctionName()) &&
         "already emitted toplevel?!");

  auto mainEntryRef = SILDeclRef::getMainFileEntryPoint(SF);
  SILFunction *TopLevel = getFunction(mainEntryRef, ForDefinition);
  TopLevel->setBare(IsBare);
  emitEntryPoint(SF, TopLevel);
}

void SILGenFunction::emitMarkFunctionEscapeForTopLevelCodeGlobals(
    SILLocation Loc, CaptureInfo CaptureInfo) {

  llvm::SmallVector<SILValue, 4> Captures;

  for (auto Capture : CaptureInfo.getCaptures()) {
    // Decls captured by value don't escape.
    auto It = VarLocs.find(Capture.getDecl());
    if (It == VarLocs.end() || !It->getSecond().value->getType().isAddress())
      continue;

    Captures.push_back(It->second.value);
  }

  if (!Captures.empty())
    B.createMarkFunctionEscape(Loc, Captures);
}

/// Emit a `mark_function_escape_instruction` into `SGF` if `AFD` captures an
/// uninitialized global variable
static void emitMarkFunctionEscape(SILGenFunction &SGF,
                                   AbstractFunctionDecl *AFD) {
  if (AFD->getDeclContext()->isLocalContext())
    return;
  auto CaptureInfo = AFD->getCaptureInfo();
  SGF.emitMarkFunctionEscapeForTopLevelCodeGlobals(AFD, std::move(CaptureInfo));
}

SILGenTopLevel::SILGenTopLevel(SILGenFunction &SGF) : SGF(SGF) {}

void SILGenTopLevel::visitSourceFile(SourceFile *SF) {

  for (auto *D : SF->getTopLevelDecls()) {
    D->visitAuxiliaryDecls([&](Decl *AuxiliaryDecl) { visit(AuxiliaryDecl); });
    visit(D);
  }

  if (auto *SynthesizedFile = SF->getSynthesizedFile()) {
    for (auto *D : SynthesizedFile->getTopLevelDecls()) {
      assert(isa<ExtensionDecl>(D) || isa<ProtocolDecl>(D));
      visit(D);
    }
  }

  for (Decl *D : SF->getHoistedDecls()) {
    visit(D);
  }

  for (TypeDecl *TD : SF->getLocalTypeDecls()) {
    if (TD->getDeclContext()->getInnermostSkippedFunctionContext())
      continue;
    visit(TD);
  }
}

void SILGenTopLevel::visitNominalTypeDecl(NominalTypeDecl *NTD) {
  TypeVisitor(SGF).emit(NTD);
}

void SILGenTopLevel::visitExtensionDecl(ExtensionDecl *ED) {
  ExtensionVisitor(SGF).emit(ED);
}

void SILGenTopLevel::visitAbstractFunctionDecl(AbstractFunctionDecl *AFD) {
  emitMarkFunctionEscape(SGF, AFD);
}

void SILGenTopLevel::visitAbstractStorageDecl(AbstractStorageDecl *ASD) {
  SGF.SGM.visitEmittedAccessors(ASD,
      [this](AccessorDecl *Accessor) { visitAbstractFunctionDecl(Accessor); });
}

void SILGenTopLevel::visitTopLevelCodeDecl(TopLevelCodeDecl *TD) {

  SGF.emitProfilerIncrement(TD->getBody());

  DebugScope DS(SGF, CleanupLocation(TD));

  for (auto &ESD : TD->getBody()->getElements()) {
    if (!SGF.B.hasValidInsertionPoint()) {
      if (auto *S = ESD.dyn_cast<Stmt *>()) {
        if (S->isImplicit())
          continue;
      } else if (auto *E = ESD.dyn_cast<Expr *>()) {
        if (E->isImplicit())
          continue;
      }

      SGF.SGM.diagnose(ESD.getStartLoc(), diag::unreachable_code);
      // There's no point in trying to emit anything else.
      return;
    }

    if (auto *S = ESD.dyn_cast<Stmt *>()) {
      SGF.emitStmt(S);
    } else if (auto *E = ESD.dyn_cast<Expr *>()) {
      SGF.emitIgnoredExpr(E);
    } else {
      SGF.visit(ESD.get<Decl *>());
    }
  }
}

SILGenTopLevel::TypeVisitor::TypeVisitor(SILGenFunction &SGF) : SGF(SGF) {}

void SILGenTopLevel::TypeVisitor::emit(IterableDeclContext *Ctx) {
  for (auto *Member : Ctx->getABIMembers()) {
    visit(Member);
  }
}

void SILGenTopLevel::TypeVisitor::visit(Decl *D) {
  if (SGF.SGM.shouldSkipDecl(D))
    return;

  TypeMemberVisitor::visit(D);
}

void SILGenTopLevel::TypeVisitor::visitPatternBindingDecl(
    PatternBindingDecl *PD) {
  for (auto i : range(PD->getNumPatternEntries())) {
    if (!PD->getExecutableInit(i) || PD->isStatic())
      continue;
    auto *Var = PD->getAnchoringVarDecl(i);
    if (Var->getDeclContext()->isLocalContext())
      continue;
    auto CaptureInfo = PD->getCaptureInfo(i);

    // If this is a stored property initializer inside a type at global scope,
    // it may close over a global variable. If we're emitting top-level code,
    // then emit a "mark_function_escape" that lists the captured global
    // variables so that definite initialization can reason about this
    // escape point.
    SGF.emitMarkFunctionEscapeForTopLevelCodeGlobals(Var,
                                                     std::move(CaptureInfo));
  }
}

void SILGenTopLevel::TypeVisitor::visitNominalTypeDecl(NominalTypeDecl *NTD) {
  TypeVisitor(SGF).emit(NTD);
}

void SILGenTopLevel::TypeVisitor::visitAbstractFunctionDecl(
    AbstractFunctionDecl *AFD) {
  emitMarkFunctionEscape(SGF, AFD);
}

void SILGenTopLevel::TypeVisitor::visitAbstractStorageDecl(
    AbstractStorageDecl *ASD) {
  SGF.SGM.visitEmittedAccessors(ASD,
      [this](AccessorDecl *Accessor) { visitAbstractFunctionDecl(Accessor); });
}

SILGenTopLevel::ExtensionVisitor::ExtensionVisitor(SILGenFunction &SGF)
    : TypeVisitor(SGF) {}

void SILGenTopLevel::ExtensionVisitor::visitPatternBindingDecl(
    PatternBindingDecl *PD) {
  auto *Ctx = PD->getDeclContext();
  if (isa<ExtensionDecl>(Ctx) &&
      cast<ExtensionDecl>(Ctx)->isObjCImplementation()) {
    TypeVisitor::visitPatternBindingDecl(PD);
  }
}
