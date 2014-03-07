//===-- Frontend.cpp - frontend utility methods ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains utility methods for parsing and performing semantic
// on modules.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/Frontend.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Module.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace swift;

void swift::CompilerInstance::createSILModule() {
  assert(getMainModule());
  TheSILModule = SILModule::createEmptyModule(getMainModule());
}

void swift::CompilerInstance::setTargetConfigurations(IRGenOptions &IRGenOpts,
                             LangOptions &LangOpts) {
  
  llvm::Triple triple = llvm::Triple(IRGenOpts.Triple);
  
  // Set the "os" target configuration.
  if (triple.isMacOSX()) {
    LangOpts.TargetConfigOptions["os"] = "OSX";
  } else if (triple.isiOS()) {
    LangOpts.TargetConfigOptions["os"] = "iOS";
  } else {
    assert(false && "Unsupported target OS");
  }
  
  // Set the "arch" target configuration.
  switch (triple.getArch()) {
  case llvm::Triple::ArchType::arm:
    LangOpts.TargetConfigOptions["arch"] = "arm";
    break;
  case llvm::Triple::ArchType::x86:
    LangOpts.TargetConfigOptions["arch"] = "i386";
    break;
  case llvm::Triple::ArchType::x86_64:
    LangOpts.TargetConfigOptions["arch"] = "x86_64";
    break;
  default:
    // FIXME: Use `case llvm::Triple::arm64` when underlying LLVM is new enough
    if (StringRef("arm64") == llvm::Triple::getArchTypeName(triple.getArch()))
      LangOpts.TargetConfigOptions["arch"] = "arm64";
      break;
    llvm_unreachable("Unsupported target architecture");
  }
}

bool swift::CompilerInstance::setup(const CompilerInvocation &Invok) {
  Invocation = Invok;

  // Honor -Xllvm.
  if (!Invok.getFrontendOptions().LLVMArgs.empty()) {
    llvm::SmallVector<const char *, 4> Args;
    Args.push_back("swift (LLVM option parsing)");
    for (unsigned i = 0, e = Invok.getFrontendOptions().LLVMArgs.size(); i != e;
         ++i)
      Args.push_back(Invok.getFrontendOptions().LLVMArgs[i].c_str());
    Args.push_back(nullptr);
    llvm::cl::ParseCommandLineOptions(Args.size()-1, Args.data());
  }
  
  // Initialize the target build configuration settings ("os" and "arch").
  setTargetConfigurations(Invocation.getIRGenOptions(),
                          Invocation.getLangOptions());

  Context.reset(new ASTContext(Invocation.getLangOptions(),
                               Invocation.getSearchPathOptions(),
                               SourceMgr, Diagnostics));

  if (Invocation.getFrontendOptions().EnableSourceImport) {
    bool immediate = Invocation.getFrontendOptions().actionIsImmediate();
    Context->addModuleLoader(SourceLoader::create(*Context, !immediate));
  }
  
  SML = SerializedModuleLoader::create(*Context);
  Context->addModuleLoader(SML);

  // Wire up the Clang importer. If the user has specified an SDK, use it.
  // Otherwise, we just keep it around as our interface to Clang's ABI
  // knowledge.
  auto ImporterCtor = swift::getClangImporterCtor();
  if (ImporterCtor) {
    auto clangImporter =
        ImporterCtor(*Context, Invocation.getTargetTriple(),
                     Invocation.getClangImporterOptions());
    if (!clangImporter) {
      Diagnostics.diagnose(SourceLoc(), diag::error_clang_importer_create_fail);
      return true;
    }

    Context->addModuleLoader(clangImporter, /*isClang*/true);
  } else if (!Invocation.getSDKPath().empty()) {
    Diagnostics.diagnose(SourceLoc(),
                         diag::error_clang_importer_not_linked_in);
    return true;
  }

  assert(Lexer::isIdentifier(Invocation.getModuleName()));

  auto CodeCompletePoint = Invocation.getCodeCompletionPoint();
  if (CodeCompletePoint.first) {
    auto MemBuf = CodeCompletePoint.first;
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    unsigned CodeCompletionBufferID = SourceMgr.addMemBufferCopy(MemBuf);
    BufferIDs.push_back(CodeCompletionBufferID);
    SourceMgr.setCodeCompletionPoint(CodeCompletionBufferID,
                                     CodeCompletePoint.second);
  }

  bool MainMode = (Invocation.getInputKind() == SourceFileKind::Main);
  bool SILMode = (Invocation.getInputKind() == SourceFileKind::SIL);

  const Optional<SelectedInput> &PrimaryInput =
    Invocation.getFrontendOptions().PrimaryInput;

  // Add the memory buffers first, these will be associated with a filename
  // and they can replace the contents of an input filename.
  for (unsigned i = 0, e = Invocation.getInputBuffers().size(); i != e; ++i) {
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    unsigned BufferID =
        SourceMgr.addMemBufferCopy(Invocation.getInputBuffers()[i]);
    BufferIDs.push_back(BufferID);

    if (SILMode)
      MainBufferID = BufferID;

    if (PrimaryInput && PrimaryInput->isBuffer() && PrimaryInput->Index == i)
      PrimaryBufferID = BufferID;
  }

  for (unsigned i = 0, e = Invocation.getInputFilenames().size(); i != e; ++i) {
    auto &File = Invocation.getInputFilenames()[i];

    // FIXME: Working with filenames is fragile, maybe use the real path
    // or have some kind of FileManager.
    using namespace llvm::sys::path;
    {
      Optional<unsigned> ExistingBufferID =
        SourceMgr.getIDForBufferIdentifier(File);
      if (ExistingBufferID.hasValue()) {
        if (SILMode || (MainMode && filename(File) == "main.swift"))
          MainBufferID = ExistingBufferID.getValue();

        if (PrimaryInput && PrimaryInput->isFilename() &&
            PrimaryInput->Index == i)
          PrimaryBufferID = ExistingBufferID.getValue();

        continue; // replaced by a memory buffer.
      }
    }

    // Open the input file.
    std::unique_ptr<llvm::MemoryBuffer> InputFile;
    if (llvm::error_code Err =
          llvm::MemoryBuffer::getFileOrSTDIN(File, InputFile)) {
      Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file,
                           File, Err.message());
      return true;
    }

    unsigned BufferID = SourceMgr.addNewSourceBuffer(std::move(InputFile));

    // Transfer ownership of the MemoryBuffer to the SourceMgr.
    BufferIDs.push_back(BufferID);

    if (SILMode || (MainMode && filename(File) == "main.swift"))
      MainBufferID = BufferID;

    if (PrimaryInput && PrimaryInput->isFilename() && PrimaryInput->Index == i)
      PrimaryBufferID = BufferID;
  }

  if (MainMode && MainBufferID == NO_SUCH_BUFFER && BufferIDs.size() == 1)
    MainBufferID = BufferIDs.front();

  return false;
}

void CompilerInstance::performParse() {
  const SourceFileKind Kind = Invocation.getInputKind();
  Identifier ID = Context->getIdentifier(Invocation.getModuleName());
  MainModule = Module::create(ID, *Context);
  Context->LoadedModules[ID.str()] = MainModule;

  if (Kind == SourceFileKind::SIL) {
    assert(BufferIDs.size() == 1);
    assert(MainBufferID != NO_SUCH_BUFFER);
    createSILModule();
  }

  if (Kind == SourceFileKind::REPL) {
    auto *SingleInputFile =
      new (*Context) SourceFile(*MainModule, Kind, {},
                                Invocation.getParseStdlib());
    MainModule->addFile(*SingleInputFile);
    return;
  }

  std::unique_ptr<DelayedParsingCallbacks> DelayedCB;
  if (Invocation.isCodeCompletion()) {
    DelayedCB.reset(
        new CodeCompleteDelayedCallbacks(SourceMgr.getCodeCompletionLoc()));
  } else if (Invocation.isDelayedFunctionBodyParsing()) {
    DelayedCB.reset(new AlwaysDelayedCallbacks);
  }

  PersistentParserState PersistentState;

  // Make sure the main file is the first file in the module. This may only be
  // a source file, or it may be a SIL file, which requires pumping the parser.
  // We parse it last, though, to make sure that it can use decls from other
  // files in the module.
  if (MainBufferID != NO_SUCH_BUFFER) {
    assert(Kind == SourceFileKind::Main || Kind == SourceFileKind::SIL);

    if (Kind == SourceFileKind::Main)
      SourceMgr.setHashbangBufferID(MainBufferID);

    auto *SingleInputFile =
      new (*Context) SourceFile(*MainModule, Kind, MainBufferID,
                                Invocation.getParseStdlib());
    MainModule->addFile(*SingleInputFile);

    if (MainBufferID == PrimaryBufferID)
      PrimarySourceFile = SingleInputFile;
  }

  bool hadLoadError = false;

  // Parse all the library files first.
  for (auto BufferID : BufferIDs) {
    if (BufferID == MainBufferID)
      continue;

    auto Buffer = SourceMgr->getMemoryBuffer(BufferID);
    if (SerializedModuleLoader::isSerializedAST(Buffer->getBuffer())) {
      std::unique_ptr<llvm::MemoryBuffer> Input(
        llvm::MemoryBuffer::getMemBuffer(Buffer->getBuffer(),
                                         Buffer->getBufferIdentifier(),
                                         false));
      if (!SML->loadAST(*MainModule, SourceLoc(), std::move(Input)))
        hadLoadError = true;
      continue;
    }

    auto *NextInput = new (*Context) SourceFile(*MainModule,
                                                SourceFileKind::Library,
                                                BufferID,
                                                Invocation.getParseStdlib());
    MainModule->addFile(*NextInput);

    if (BufferID == PrimaryBufferID)
      PrimarySourceFile = NextInput;

    bool Done;
    parseIntoSourceFile(*NextInput, BufferID, &Done, nullptr,
                        &PersistentState, DelayedCB.get());
    assert(Done && "Parser returned early?");
    (void) Done;

    performNameBinding(*NextInput);
  }

  if (hadLoadError)
    return;
  
  // Parse the main file last.
  if (MainBufferID != NO_SUCH_BUFFER) {
    SourceFile &MainFile = MainModule->getMainSourceFile(Kind);
    SILParserState SILContext(TheSILModule.get());
    unsigned CurTUElem = 0;
    bool Done;
    do {
      // Pump the parser multiple times if necessary.  It will return early
      // after parsing any top level code in a main module, or in SIL mode when
      // there are chunks of swift decls (e.g. imports and types) interspersed
      // with 'sil' definitions.
      parseIntoSourceFile(MainFile, MainFile.getBufferID().getValue(), &Done,
                          TheSILModule ? &SILContext : nullptr,
                          &PersistentState, DelayedCB.get());
      if (!Invocation.getParseOnly() && (PrimaryBufferID == NO_SUCH_BUFFER ||
                                         MainBufferID == PrimaryBufferID))
        performTypeChecking(MainFile, PersistentState.getTopLevelContext(),
                            CurTUElem);
      CurTUElem = MainFile.Decls.size();
    } while (!Done);
  }

  if (!Invocation.getParseOnly()) {
    // Type-check each top-level input besides the main source file.
    for (auto File : MainModule->getFiles())
      if (auto SF = dyn_cast<SourceFile>(File))
        if (PrimaryBufferID == NO_SUCH_BUFFER ||
            (SF->getBufferID().hasValue() &&
             SF->getBufferID().getValue() == PrimaryBufferID))
          performTypeChecking(*SF, PersistentState.getTopLevelContext());

    // Even if there were no source files, we should still record known
    // protocols.
    if (Context->getStdlibModule())
      Context->recordKnownProtocols(Context->getStdlibModule());
  }

  if (DelayedCB) {
    performDelayedParsing(MainModule, PersistentState,
                          Invocation.getCodeCompletionFactory());
  }
}

