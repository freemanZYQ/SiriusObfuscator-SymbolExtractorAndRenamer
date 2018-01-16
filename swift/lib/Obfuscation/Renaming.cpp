#include "swift/Obfuscation/Renaming.h"
#include "swift/Obfuscation/CompilerInfrastructure.h"
#include "swift/Obfuscation/SourceFileWalker.h"
#include "swift/Obfuscation/Utils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "swift/IDE/Utils.h"

#include <memory>

namespace swift {
namespace obfuscation {

using SmallPath = llvm::SmallString<256>;
  
llvm::Expected<SmallPath>
computeObfuscatedPath(const StringRef Filename,
                      const StringRef OriginalProjectPath,
                      const StringRef ObfuscatedProjectPath) {
  SmallPath Path(Filename);
  llvm::sys::path::replace_path_prefix(Path, OriginalProjectPath, ObfuscatedProjectPath);
  return Path;
}

llvm::Error copyProject(const StringRef OriginalPath,
                        const StringRef ObfuscatedPath) {
  
  std::error_code ErrorCode;
  for (llvm::sys::fs::recursive_directory_iterator Iterator(OriginalPath, ErrorCode), End;
       Iterator != End && !ErrorCode;
       Iterator.increment(ErrorCode)) {
    if (llvm::sys::fs::is_directory(Iterator->path())) {
      continue;
    }
    
    auto PathOrError = computeObfuscatedPath(StringRef(Iterator->path()),
                                             OriginalPath,
                                             ObfuscatedPath);
    if (auto Error = PathOrError.takeError()) {
      return Error;
    }
    
    auto Path = PathOrError.get();
    auto DirectoryPath = Path;
    llvm::sys::path::remove_filename(DirectoryPath);
    if (auto Error = llvm::sys::fs::create_directories(DirectoryPath)) {
      return stringError("Cannot create directory in " + Path.str().str(), Error);
    }
    
    if (auto Error = llvm::sys::fs::copy_file(Iterator->path(), Path)) {
      return stringError("Cannot copy file from " + Iterator->path() + " to " + Path.str().str(), Error);
    }
  }
  
  if (ErrorCode) {
    return stringError("Error while traversing the project directory " + OriginalPath.str(), ErrorCode);
  }
  
  return llvm::Error::success();
}

static bool shouldRename(const SymbolRenaming &Symbol,
                         const SymbolWithRange &SymbolWithRange,
                         const std::string &ModuleName) {
  return SymbolWithRange.Symbol.Identifier == Symbol.Identifier
      && SymbolWithRange.Symbol.Name == Symbol.OriginalName
      && SymbolWithRange.Symbol.Module == ModuleName;
}
  
llvm::Expected<bool> performActualRenaming(SourceFile &Current,
                                           const std::string &ModuleName,
                                           const RenamesJson &RenamesJson,
                                           swift::SourceManager &SourceManager,
                                           unsigned int BufferId,
                                           StringRef Path) {
  bool performedRenaming = false;
  auto SymbolsWithRanges = walkAndCollectSymbols(Current);
  
  std::unique_ptr<llvm::raw_fd_ostream> DescriptorStream(nullptr);
  std::unique_ptr<swift::ide::SourceEditOutputConsumer> Editor(nullptr);
  
  //TODO: would be way better to have a map here instead of iterating through symbols
  for (const auto &SymbolWithRange : SymbolsWithRanges) {
    for (const auto &Symbol : RenamesJson.Symbols) {
      
      if (shouldRename(Symbol, SymbolWithRange, ModuleName)) {
        if (Editor == nullptr) {
          std::error_code Error;
          DescriptorStream = llvm::make_unique<llvm::raw_fd_ostream>(Path, Error, llvm::sys::fs::F_None);
          if (DescriptorStream->has_error() || Error) {
            return stringError("Cannot open output file: " + Path.str(), Error);
          }
          Editor = llvm::make_unique<swift::ide::SourceEditOutputConsumer>(SourceManager,
                                                                           BufferId,
                                                                           *DescriptorStream);
        }
        Editor->ide::SourceEditConsumer::accept(SourceManager,
                                                SymbolWithRange.Range,
                                                StringRef(Symbol.ObfuscatedName));
        performedRenaming = true;
        break;
      }
    }
  }
  return performedRenaming;
}
  
llvm::Expected<FilesList>
performRenaming(std::string MainExecutablePath,
                const FilesJson &FilesJson,
                const RenamesJson &RenamesJson,
                std::string ObfuscatedProjectPath) {
  
  CompilerInstance CI;
  if (auto Error = setupCompilerInstance(CI, FilesJson, MainExecutablePath)) {
    return std::move(Error);
  }
  
  if (auto Error = copyProject(FilesJson.Project.RootPath, ObfuscatedProjectPath)) {
    return std::move(Error);
  }
  
  FilesList Files;
  for (auto* Unit : CI.getMainModule()->getFiles()) {
    if (auto* Current = dyn_cast<SourceFile>(Unit)) {

      auto PathOrError = computeObfuscatedPath(Current->getFilename(),
                                               FilesJson.Project.RootPath,
                                               ObfuscatedProjectPath);
      if (auto Error = PathOrError.takeError()) {
        return std::move(Error);
      }
      
      auto Path = PathOrError.get().str();
      auto &SourceManager = Current->getASTContext().SourceMgr;
      auto BufferId = Current->getBufferID().getValue();
      
      if (performActualRenaming(*Current,
                                FilesJson.Module.Name,
                                RenamesJson,
                                SourceManager,
                                BufferId,
                                Path)) {
        auto Filename = llvm::sys::path::filename(Path).str();
        Files.push_back(std::pair<std::string, std::string>(Filename, Path));
      }
    }
  }
  
  return Files;
}

} //namespace obfuscation
} //namespace swift
