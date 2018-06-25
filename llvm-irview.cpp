#include <fstream>
#include <iostream>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/IR/AssemblyAnnotationWriter.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SourceMgr.h>

using namespace std;
using namespace llvm;

std::string repeat(std::string str, const std::size_t n) {
  if (n == 0) {
    str.clear();
    str.shrink_to_fit();
    return str;
  } else if (n == 1 || str.empty()) {
    return str;
  }
  const auto period = str.size();
  if (period == 1) {
    str.append(n - 1, str.front());
    return str;
  }
  str.reserve(period * n);
  std::size_t m{2};
  for (; m < n; m *= 2)
    str += str;
  str.append(str.c_str(), (n - (m / 2)) * period);
  return str;
}

std::string realpath(std::string path) {
  char *real_path = realpath(path.c_str(), NULL);
  if (real_path == NULL)
    return path;
  std::string std_real_path = std::string(real_path);
  free(real_path);
  return std_real_path;
}

std::string shortest(std::vector<string> paths) {
  string candidate = paths[0];
  for (size_t i = 1; i < paths.size(); i++) {
    if (candidate.length() > paths[i].length())
      candidate = paths[i];
  }
  return candidate;
}

std::string short_path(std::string path, std::string root) {
  std::vector<string> short_paths({path});

  if (path.find(root) == 0)
    short_paths.push_back(path.substr(root.length() + 1));

  string real_path = realpath(path);
  string real_root = realpath(root);
  if (real_path.find(real_root) == 0)
    short_paths.push_back(real_path.substr(real_root.length() + 1));

  return shortest(short_paths);
}

std::string short_path(std::string path) {
  std::vector<string> short_paths({path});

  char *load_path = getenv("JULIA_LOAD_PATH");
  if (load_path != NULL)
    short_paths.push_back(short_path(path, string(load_path)));

  return shortest(short_paths);
}

// helper class for tracking inlining context while printing debug info
class DILineInfoPrinter {
  std::vector<DILineInfo> context;

public:
  void emit_finish(raw_ostream &Out);
  void emit_lineinfo(formatted_raw_ostream &Out, std::vector<DILineInfo> &DI);

  template <class T> void emit_lineinfo(std::string &Out, T &DI) {
    raw_string_ostream OS(Out);
    emit_lineinfo(OS, DI);
  }

  void emit_lineinfo(formatted_raw_ostream &Out, DILineInfo &DI) {
    std::vector<DILineInfo> DIvec(1);
    DIvec[0] = DI;
    emit_lineinfo(Out, DIvec);
  }

  void emit_lineinfo(formatted_raw_ostream &Out, DIInliningInfo &DI) {
    size_t nframes = DI.getNumberOfFrames();
    std::vector<DILineInfo> DIvec(nframes);
    for (size_t i = 0; i < DI.getNumberOfFrames(); i++) {
      DIvec[i] = DI.getFrame(i);
    }
    emit_lineinfo(Out, DIvec);
  }

  void emit_finish(std::string &Out) {
    raw_string_ostream OS(Out);
    emit_finish(OS);
  }
};

void DILineInfoPrinter::emit_finish(raw_ostream &Out) { context.clear(); }

std::string read_line(std::string path, size_t line)
{
  string buf;
  ifstream file(path);
  if (!file) {
    // files in Julia's base directory are not fully specified;
    // rely on an environment variable to look them up
    char *base_dir = getenv("JULIA_BASE_DIR");
    if (base_dir != NULL) {
      file = ifstream(string(base_dir) + '/' + path);
    }
  }
  if (!file) 
    return "";
  size_t i;
  for (i = 1; getline(file, buf) && i != line; i++) {}
  if (i != line)
    return "";
  return buf;
}

std::string & ltrim(std::string & str)
{
  auto it2 =  std::find_if( str.begin() , str.end() , [](char ch){ return !std::isspace<char>(ch , std::locale::classic() ) ; } );
  str.erase( str.begin() , it2);
  return str;   
}

void DILineInfoPrinter::emit_lineinfo(formatted_raw_ostream &Out,
                                      std::vector<DILineInfo> &DI) {
  // just skip over lines with no debug info at all
  size_t nframes = DI.size();
  if (nframes == 0)
    return;

  size_t nctx = context.size();
  if (nctx > nframes)
    context.resize(nframes);

  // check if we need to update just the line number info
  bool update_line_only = false;
  for (size_t i = 0; i < nctx && i < nframes; i++) {
    const DILineInfo &CtxLine = context.at(i);
    const DILineInfo &FrameLine = DI.at(nframes - 1 - i);
    if (CtxLine != FrameLine) {
      if (CtxLine.FileName == FrameLine.FileName &&
          CtxLine.FunctionName == FrameLine.FunctionName) {
        update_line_only = true;
      }
      context.resize(i);
      break;
    }
  }
  if (update_line_only) {
    DILineInfo frame = DI.at(nframes - 1 - context.size());
    if (frame.Line != UINT_MAX && frame.Line != 0)
      Out << repeat("│ ", context.size()) << "├ ; "
          << short_path(frame.FileName) << ":" << frame.Line;

      std::string line = read_line(frame.FileName, frame.Line);
      if (!line.empty()) {
        Out.PadToColumn(92 + 2*(context.size()));
        Out << ltrim(line);
      }

      Out << '\n';
    context.push_back(frame);
  }

  for (size_t i = context.size(); i < nframes; i++) {
    DILineInfo frame = DI.at(nframes - 1 - i);
    context.push_back(frame);
    // strip trailing ; from function name
    std::string functionname =
        frame.FunctionName.substr(0, frame.FunctionName.size() - 1);
    Out << repeat("│ ", i) << "┌ "
        << "; " << functionname << " at " << short_path(frame.FileName);
    if (frame.Line != UINT_MAX && frame.Line != 0) {
      Out << ":" << frame.Line;
      std::string line = read_line(frame.FileName, frame.Line);
      if (!line.empty()) {
        Out.PadToColumn(92 + 2*i);
        Out << ltrim(line);
      }
    }
    Out << "\n";
  }
}

// adaptor class for printing line numbers before llvm IR lines
class LineNumberAnnotatedWriter : public AssemblyAnnotationWriter {
  DILocation *InstrLoc = nullptr;
  DILineInfoPrinter LinePrinter;
  DenseMap<const Instruction *, DILocation *> DebugLoc;
  DenseMap<const Function *, DISubprogram *> Subprogram;

public:
  LineNumberAnnotatedWriter() {}
  virtual void emitFunctionAnnot(const Function *, formatted_raw_ostream &);
  virtual void emitInstructionAnnot(const Instruction *,
                                    formatted_raw_ostream &);
  virtual void emitBasicBlockEndAnnot(const BasicBlock *,
                                      formatted_raw_ostream &);
  // virtual void printInfoComment(const Value &, formatted_raw_ostream &) {}

  void addSubprogram(const Function *F, DISubprogram *SP) {
    Subprogram[F] = SP;
  }

  void addDebugLoc(const Instruction *I, DILocation *Loc) { DebugLoc[I] = Loc; }
};

void LineNumberAnnotatedWriter::emitFunctionAnnot(const Function *F,
                                                  formatted_raw_ostream &Out) {
  InstrLoc = nullptr;
  DISubprogram *FuncLoc = F->getSubprogram();
  if (!FuncLoc) {
    auto SP = Subprogram.find(F);
    if (SP != Subprogram.end())
      FuncLoc = SP->second;
  }
  if (!FuncLoc)
    return;
  std::vector<DILineInfo> DIvec(1);
  DILineInfo &DI = DIvec.back();
  DI.FunctionName = FuncLoc->getName();
  DI.FileName = FuncLoc->getFilename();
  DI.Line = FuncLoc->getLine();
  LinePrinter.emit_lineinfo(Out, DIvec);
}

std::vector<DILineInfo> lookupFrames(DILocation *&Loc) {
  std::vector<DILineInfo> DIvec;
  do {
    DIvec.emplace_back();
    DILineInfo &DI = DIvec.back();
    DIScope *scope = Loc->getScope();
    if (scope)
      DI.FunctionName = scope->getName();
    DI.FileName = Loc->getFilename();
    DI.Line = Loc->getLine();
    Loc = Loc->getInlinedAt();
  } while (Loc);
  return DIvec;
}

void LineNumberAnnotatedWriter::emitInstructionAnnot(
    const Instruction *I, formatted_raw_ostream &Out) {
  // look for the DI for the current instruction
  DILocation *NewInstrLoc = I->getDebugLoc();
  if (!NewInstrLoc) {
    // we might have deleted it
    auto Loc = DebugLoc.find(I);
    if (Loc != DebugLoc.end())
      NewInstrLoc = Loc->second;
  }

  // no debug info, do nothing
  if (!NewInstrLoc) {
    // at least try to match the previous indentation
    if (InstrLoc) {
      NewInstrLoc = InstrLoc;
      auto DIvec = lookupFrames(NewInstrLoc);
      size_t nframes = DIvec.size();
      Out << repeat("  ", nframes);
    }
    return;
  }

  // look-up the entire scope
  InstrLoc = NewInstrLoc;
  auto DIvec = lookupFrames(NewInstrLoc);

  size_t nframes = DIvec.size();
  if (NewInstrLoc != InstrLoc)
    LinePrinter.emit_lineinfo(Out, DIvec);
  Out << repeat("│ ", nframes);
}

void LineNumberAnnotatedWriter::emitBasicBlockEndAnnot(
    const BasicBlock *BB, formatted_raw_ostream &Out) {
  if (BB == &BB->getParent()->back())
    LinePrinter.emit_finish(Out);
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    errs() << "Usage: " << argv[0] << " INPUT OUTPUT\n";
    return 1;
  }

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> m = parseIRFile(argv[1], Err, Context);
  if (!m) {
    Err.print(argv[0], errs());
    return 1;

  }

  //
  // Main
  //

  LineNumberAnnotatedWriter AAW;

  // strip metadata from all instructions in all functions in the module
  Instruction *deletelast = nullptr; // only delete after advancing iterator
  for (Function &f2 : m->functions()) {
    AAW.addSubprogram(&f2, f2.getSubprogram());
    for (BasicBlock &f2_bb : f2) {
      for (Instruction &inst : f2_bb) {
        if (deletelast) {
          deletelast->eraseFromParent();
          deletelast = nullptr;
        }
        // remove dbg.declare and dbg.value calls
        if (isa<DbgDeclareInst>(inst) || isa<DbgValueInst>(inst)) {
          deletelast = &inst;
          continue;
        }

        // iterate over all metadata kinds and set to NULL to remove
        SmallVector<std::pair<unsigned, MDNode *>, 4> MDForInst;
        inst.getAllMetadataOtherThanDebugLoc(MDForInst);
        for (const auto &md_iter : MDForInst) {
          inst.setMetadata(md_iter.first, NULL);
        }
        // record debug location before erasing it
        AAW.addDebugLoc(&inst, inst.getDebugLoc());
        inst.setDebugLoc(DebugLoc());
      }
      if (deletelast) {
        deletelast->eraseFromParent();
        deletelast = nullptr;
      }
    }
  }
  for (GlobalObject &g2 : m->global_objects()) {
    g2.clearMetadata();
  }


  //
  // Finish
  //

  string out_fn(argv[2]);
  if (out_fn == "-")
    m->print(outs(), &AAW);
  else {
    // TOWO: raw_ostream with direct file output
    std::string code;
    llvm::raw_string_ostream stream(code);
    m->print(stream, &AAW);

    std::ofstream out(out_fn);
    out << code;
    out.close();
  }

  return 0;
}