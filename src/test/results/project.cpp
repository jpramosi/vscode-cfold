// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "project.hh"

#include "clang_tu.hh" // llvm::vfs
#include "filesystem.hh"
#include "log.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "utils.hh"
#include "working_files.hh"

#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Types.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/GlobPattern.h>
#include <llvm/Support/LineIterator.h>
#include <llvm/Support/Program.h>

#include <rapidjson/writer.h>

#ifdef _WIN32 @_0_
#include <Windows.h> @_0_
#else @_1_
#include <unistd.h>
#endif @_1_

#include <array>
#include <limits.h>
#include <unordered_set>
#include <vector>

using namespace clang;
using namespace llvm;

namespace ccls { @_2_
std::pair<LanguageId, bool> lookupExtension(std::string_view filename) { @_3_
  using namespace clang::driver;
  auto i = types::lookupTypeForExtension(
      sys::path::extension({filename.data(), filename.size()}).substr(1));
  bool header = i == types::TY_CHeader || i == types::TY_CXXHeader ||
                i == types::TY_ObjCXXHeader;
  bool objc = types::isObjC(i);
  LanguageId ret;
  if (types::isCXX(i))
    ret = types::isCuda(i) ? LanguageId::Cuda
                           : objc ? LanguageId::ObjCpp : LanguageId::Cpp;
  else if (objc)
    ret = LanguageId::ObjC;
  else if (i == types::TY_C || i == types::TY_CHeader)
    ret = LanguageId::C;
  else
    ret = LanguageId::Unknown;
  return {ret, header};
} @_3_

namespace { @_4_

enum OptionClass { @_5_
  EqOrJoinOrSep,
  EqOrSep,
  JoinOrSep,
  Separate,
}; @_5_

struct ProjectProcessor { @_6_
  Project::Folder &folder;
  std::unordered_set<size_t> command_set;
  StringSet<> exclude_args;
  std::vector<GlobPattern> exclude_globs;

  ProjectProcessor(Project::Folder &folder) : folder(folder) { @_7_
    for (auto &arg : g_config->clang.excludeArgs)
      if (arg.find_first_of("?*[") == std::string::npos)
        exclude_args.insert(arg);
      else if (Expected<GlobPattern> glob_or_err = GlobPattern::create(arg))
        exclude_globs.push_back(std::move(*glob_or_err));
      else
        LOG_S(WARNING) << toString(glob_or_err.takeError());
  } @_7_

  bool excludesArg(StringRef arg, int &i) { @_8_
    if (arg.startswith("-M")) { @_9_
      if (arg == "-MF" || arg == "-MT" || arg == "-MQ")
        i++;
      return true;
    } @_9_
    if (arg == "-Xclang") { @_10_
      i++;
      return true;
    } @_10_
    return exclude_args.count(arg) ||
           any_of(exclude_globs,
                  [&](const GlobPattern &glob) { return glob.match(arg); });
  } @_8_

  // Expand %c %cpp ... in .ccls
  void process(Project::Entry &entry) { @_11_
    std::vector<const char *> args(entry.args.begin(),
                                   entry.args.begin() + entry.compdb_size);
    auto [lang, header] = lookupExtension(entry.filename);
    for (int i = entry.compdb_size; i < entry.args.size(); i++) { @_12_
      const char *arg = entry.args[i];
      StringRef a(arg);
      if (a[0] == '%') { @_13_
        bool ok = false;
        for (;;) { @_14_
          if (a.consume_front("%c "))
            ok |= lang == LanguageId::C;
          else if (a.consume_front("%h "))
            ok |= lang == LanguageId::C && header;
          else if (a.consume_front("%cpp "))
            ok |= lang == LanguageId::Cpp;
          else if (a.consume_front("%cu "))
            ok |= lang == LanguageId::Cuda;
          else if (a.consume_front("%hpp "))
            ok |= lang == LanguageId::Cpp && header;
          else if (a.consume_front("%objective-c "))
            ok |= lang == LanguageId::ObjC;
          else if (a.consume_front("%objective-cpp "))
            ok |= lang == LanguageId::ObjCpp;
          else
            break;
        } @_14_
        if (ok)
          args.push_back(a.data());
      } else if (!excludesArg(a, i)) {
        args.push_back(arg);
      } @_13_
    } @_12_
    entry.args = args;
    getSearchDirs(entry);
  } @_11_

  void getSearchDirs(Project::Entry &entry) { @_15_
#if LLVM_VERSION_MAJOR < 8 @_16_
    const std::string base_name = sys::path::filename(entry.filename);
    size_t hash = std::hash<std::string>{}(entry.directory);
    bool OPT_o = false;
    for (auto &arg : entry.args) { @_17_
      bool last_o = OPT_o;
      OPT_o = false;
      if (arg[0] == '-') { @_18_
        OPT_o = arg[1] == 'o' && arg[2] == '\0';
        if (OPT_o || arg[1] == 'D' || arg[1] == 'W')
          continue;
      } else if (last_o) {
        continue;
      } else if (sys::path::filename(arg) == base_name) {
        LanguageId lang = lookupExtension(arg).first;
        if (lang != LanguageId::Unknown) { @_19_
          hash_combine(hash, (size_t)lang);
          continue;
        } @_19_
      } @_18_
      hash_combine(hash, std::hash<std::string_view>{}(arg));
    } @_17_
    if (!command_set.insert(hash).second)
      return;
    auto args = entry.args;
    args.push_back("-fsyntax-only");
    for (const std::string &arg : g_config->clang.extraArgs)
      args.push_back(intern(arg));
    args.push_back(intern("-working-directory=" + entry.directory));
    args.push_back(intern("-resource-dir=" + g_config->clang.resourceDir));

    // a weird C++ deduction guide heap-use-after-free causes libclang to crash.
    IgnoringDiagConsumer DiagC;
    IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts(new DiagnosticOptions());
    DiagnosticsEngine Diags(
        IntrusiveRefCntPtr<DiagnosticIDs>(new DiagnosticIDs()), &*DiagOpts,
        &DiagC, false);

    driver::Driver Driver(args[0], llvm::sys::getDefaultTargetTriple(), Diags);
    auto TargetAndMode =
        driver::ToolChain::getTargetAndModeFromProgramName(args[0]);
    if (!TargetAndMode.TargetPrefix.empty()) { @_20_
      const char *arr[] = {"-target", TargetAndMode.TargetPrefix.c_str()};
      args.insert(args.begin() + 1, std::begin(arr), std::end(arr));
      Driver.setTargetAndMode(TargetAndMode);
    } @_20_
    Driver.setCheckInputsExist(false);

    std::unique_ptr<driver::Compilation> C(Driver.BuildCompilation(args));
    const driver::JobList &Jobs = C->getJobs();
    if (Jobs.size() != 1)
      return;
    const auto &CCArgs = Jobs.begin()->getArguments();

    auto CI = std::make_unique<CompilerInvocation>();
    CompilerInvocation::CreateFromArgs(*CI, CCArgs.data(),
                                       CCArgs.data() + CCArgs.size(), Diags);
    CI->getFrontendOpts().DisableFree = false;
    CI->getCodeGenOpts().DisableFree = false;

    HeaderSearchOptions &HeaderOpts = CI->getHeaderSearchOpts();
    for (auto &E : HeaderOpts.UserEntries) { @_21_
      std::string path =
          normalizePath(resolveIfRelative(entry.directory, E.Path));
      ensureEndsInSlash(path);
      switch (E.Group) { @_22_
      default:
        folder.search_dir2kind[path] |= 2;
        break;
      case frontend::Quoted:
        folder.search_dir2kind[path] |= 1;
        break;
      case frontend::Angled:
        folder.search_dir2kind[path] |= 3;
        break;
      } @_22_
    } @_21_
#endif @_16_
  } @_15_
}; @_6_

std::vector<const char *>
readCompilerArgumentsFromFile(const std::string &path) { @_23_
  auto mbOrErr = MemoryBuffer::getFile(path);
  if (!mbOrErr)
    return {};
  std::vector<const char *> args;
  for (line_iterator i(*mbOrErr.get(), true, '#'), e; i != e; ++i) { @_24_
    std::string line(*i);
    doPathMapping(line);
    args.push_back(intern(line));
  } @_24_
  return args;
} @_23_

bool appendToCDB(const std::vector<const char *> &args) { @_25_
  return args.size() && StringRef("%compile_commands.json") == args[0];
} @_25_

std::vector<const char *> getFallback(const std::string &path) { @_26_
  std::vector<const char *> argv{"clang"};
  if (sys::path::extension(path) == ".h")
    argv.push_back("-xobjective-c++-header");
  argv.push_back(intern(path));
  return argv;
} @_26_

void loadDirectoryListing(ProjectProcessor &proc, const std::string &root,
                          const StringSet<> &seen) { @_27_
  Project::Folder &folder = proc.folder;
  std::vector<std::string> files;

  auto getDotCcls = [&root, &folder](std::string cur) { @_28_
    while (!(cur = sys::path::parent_path(cur)).empty()) { @_29_
      auto it = folder.dot_ccls.find(cur);
      if (it != folder.dot_ccls.end())
        return it->second;
      std::string normalized = normalizePath(cur);
      // Break if outside of the project root.
      if (normalized.size() <= root.size() ||
          normalized.compare(0, root.size(), root) != 0)
        break;
    } @_29_
    return folder.dot_ccls[root];
  }; @_28_

  getFilesInFolder(root, true /*recursive*/, true /*add_folder_to_path*/, @_30_ @_30_ @_31_ @_31_
                   [&folder, &files, &seen](const std::string &path) { @_32_
                     std::pair<LanguageId, bool> lang = lookupExtension(path);
                     if (lang.first != LanguageId::Unknown && !lang.second) { @_33_
                       if (!seen.count(path))
                         files.push_back(path);
                     } else if (sys::path::filename(path) == ".ccls") {
                       std::vector<const char *> args =
                           readCompilerArgumentsFromFile(path);
                       folder.dot_ccls.emplace(
                           sys::path::parent_path(path).str() + '/', args);
                       std::string l;
                       for (size_t i = 0; i < args.size(); i++) { @_34_
                         if (i)
                           l += ' ';
                         l += args[i];
                       } @_34_
                       LOG_S(INFO) << "use " << path << ": " << l;
                     } @_33_
                   }); @_32_

  // If the first line of .ccls is %compile_commands.json, append extra flags.
  for (auto &e : folder.entries)
    if (const auto &args = getDotCcls(e.filename); appendToCDB(args)) { @_35_
      if (args.size())
        e.args.insert(e.args.end(), args.begin() + 1, args.end());
      proc.process(e);
    } @_35_
  // Set flags for files not in compile_commands.json
  for (const std::string &file : files)
    if (const auto &args = getDotCcls(file); !appendToCDB(args)) { @_36_
      Project::Entry e;
      e.root = e.directory = root;
      e.filename = file;
      if (args.empty()) { @_37_
        e.args = getFallback(e.filename);
      } else {
        e.args = args;
        e.args.push_back(intern(e.filename));
      } @_37_
      proc.process(e);
      folder.entries.push_back(e);
    } @_36_
} @_27_

// Computes a score based on how well |a| and |b| match. This is used for
// argument guessing.
int computeGuessScore(std::string_view a, std::string_view b) { @_38_
  int score = 0;
  unsigned h = 0;
  llvm::SmallDenseMap<unsigned, int> m;
  for (uint8_t c : a)
    if (c == '/') { @_39_
      score -= 9;
      if (h)
        m[h]++;
      h = 0;
    } else {
      h = h * 33 + c;
    } @_39_
  h = 0;
  for (uint8_t c : b)
    if (c == '/') { @_40_
      score -= 9;
      auto it = m.find(h);
      if (it != m.end() && it->second > 0) { @_41_
        it->second--;
        score += 31;
      } @_41_
      h = 0;
    } else {
      h = h * 33 + c;
    } @_40_

  uint8_t c;
  int d[127] = {};
  for (int i = a.size(); i-- && (c = a[i]) != '/';)
    if (c < 127)
      d[c]++;
  for (int i = b.size(); i-- && (c = b[i]) != '/';)
    if (c < 127 && d[c])
      d[c]--, score++;
  return score;
} @_38_

} // namespace @_4_

void Project::loadDirectory(const std::string &root, Project::Folder &folder) { @_42_
  SmallString<256> cdbDir, path, stdinPath;
  std::string err_msg;
  folder.entries.clear();
  if (g_config->compilationDatabaseCommand.empty()) { @_43_
    cdbDir = root;
    if (g_config->compilationDatabaseDirectory.size()) { @_44_
      if (sys::path::is_absolute(g_config->compilationDatabaseDirectory))
        cdbDir = g_config->compilationDatabaseDirectory;
      else
        sys::path::append(cdbDir, g_config->compilationDatabaseDirectory);
    } @_44_
    sys::path::append(path, cdbDir, "compile_commands.json");
  } else {
    // If `compilationDatabaseCommand` is specified, execute it to get the
    // compdb.
#ifdef _WIN32 @_45_
    char tmpdir[L_tmpnam];
    tmpnam_s(tmpdir, L_tmpnam);
    cdbDir = tmpdir;
    if (sys::fs::create_directory(tmpdir, false))
      return; @_45_
#else @_46_
    char tmpdir[] = "/tmp/ccls-compdb-XXXXXX";
    if (!mkdtemp(tmpdir))
      return;
    cdbDir = tmpdir;
#endif @_46_
    sys::path::append(path, cdbDir, "compile_commands.json");
    sys::path::append(stdinPath, cdbDir, "stdin");
    { @_47_
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
      JsonWriter json_writer(&writer);
      reflect(json_writer, *g_config);
      std::string input = sb.GetString();
      FILE *fout = fopen(stdinPath.c_str(), "wb");
      fwrite(input.c_str(), input.size(), 1, fout);
      fclose(fout);
    } @_47_
    std::array<Optional<StringRef>, 3> redir{StringRef(stdinPath), @_48_
                                             StringRef(path), StringRef()}; @_48_
    std::vector<StringRef> args{g_config->compilationDatabaseCommand, root};
    if (sys::ExecuteAndWait(args[0], args, llvm::None, redir, 0, 0, &err_msg) <
        0) { @_49_
      LOG_S(ERROR) << "failed to execute " << args[0].str() << " "
                   << args[1].str() << ": " << err_msg;
      return;
    } @_49_
  } @_43_

  std::unique_ptr<tooling::CompilationDatabase> cdb =
      tooling::CompilationDatabase::loadFromDirectory(cdbDir, err_msg);
  if (!g_config->compilationDatabaseCommand.empty()) { @_50_
#ifdef _WIN32 @_51_
    DeleteFileA(stdinPath.c_str());
    DeleteFileA(path.c_str());
    RemoveDirectoryA(cdbDir.c_str()); @_51_
#else @_52_
    unlink(stdinPath.c_str());
    unlink(path.c_str());
    rmdir(cdbDir.c_str());
#endif @_52_
  } @_50_

  ProjectProcessor proc(folder);
  StringSet<> seen;
  std::vector<Project::Entry> result;
  if (!cdb) { @_53_
    if (g_config->compilationDatabaseCommand.size() || sys::fs::exists(path))
      LOG_S(ERROR) << "failed to load " << path.c_str();
  } else {
    LOG_S(INFO) << "loaded " << path.c_str();
    for (tooling::CompileCommand &cmd : cdb->getAllCompileCommands()) { @_54_
      static bool once;
      Project::Entry entry;
      entry.root = root;
      doPathMapping(entry.root);

      // If workspace folder is real/ but entries use symlink/, convert to
      // real/.
      entry.directory = realPath(cmd.Directory);
      normalizeFolder(entry.directory);
      doPathMapping(entry.directory);
      entry.filename =
          realPath(resolveIfRelative(entry.directory, cmd.Filename));
      normalizeFolder(entry.filename);
      doPathMapping(entry.filename);

      std::vector<std::string> args = std::move(cmd.CommandLine);
      entry.args.reserve(args.size());
      for (int i = 0; i < args.size(); i++) { @_55_
        doPathMapping(args[i]);
        if (!proc.excludesArg(args[i], i))
          entry.args.push_back(intern(args[i]));
      } @_55_
      entry.compdb_size = entry.args.size();

      // Work around relative --sysroot= as it isn't affected by
      // -working-directory=. chdir is thread hostile but this function runs
      // before indexers do actual work and it works when there is only one
      // workspace folder.
      if (!once) { @_56_
        once = true;
        llvm::vfs::getRealFileSystem()->setCurrentWorkingDirectory(
            entry.directory);
      } @_56_
      proc.getSearchDirs(entry);

      if (seen.insert(entry.filename).second)
        folder.entries.push_back(entry);
    } @_54_
  } @_53_

  // Use directory listing if .ccls exists or compile_commands.json does not
  // exist.
  path.clear();
  sys::path::append(path, root, ".ccls");
  LOG_S(INFO) << "root: " << root;
  if (sys::fs::exists(path)) { @_57_
    LOG_S(INFO) << "Found: " << path.c_str();
    loadDirectoryListing(proc, root, seen);
  } @_57_
} @_42_

void Project::load(const std::string &root) { @_58_
  assert(root.back() == '/');
  std::lock_guard lock(mtx);
  Folder &folder = root2folder[root];

  loadDirectory(root, folder);
  for (auto &[path, kind] : folder.search_dir2kind)
    LOG_S(INFO) << "search directory: " << path << ' ' << " \"< "[kind];

  // Setup project entries.
  folder.path2entry_index.reserve(folder.entries.size());
  for (size_t i = 0; i < folder.entries.size(); ++i) { @_59_
    folder.entries[i].id = i;
    folder.path2entry_index[folder.entries[i].filename] = i;
    LOG_S(INFO) << "add file: " << folder.entries[i].filename;
  } @_59_
} @_58_

Project::Entry Project::findEntry(const std::string &path, bool can_redirect,
                                  bool must_exist) { @_60_
  std::string best_dot_ccls_root;
  Project::Folder *best_dot_ccls_folder = nullptr;
  std::string best_dot_ccls_dir;
  const std::vector<const char *> *best_dot_ccls_args = nullptr;

  bool match = false, exact_match = false;
  const Entry *best = nullptr;
  Project::Folder *best_compdb_folder = nullptr;

  Project::Entry ret;
  std::lock_guard lock(mtx);

  for (auto &[root, folder] : root2folder)
    if (StringRef(path).startswith(root)) { @_61_
      // Find the best-fit .ccls
      for (auto &[dir, args] : folder.dot_ccls)
        if (StringRef(path).startswith(dir) &&
            dir.length() > best_dot_ccls_dir.length()) { @_62_
          best_dot_ccls_root = root;
          best_dot_ccls_folder = &folder;
          best_dot_ccls_dir = dir;
          best_dot_ccls_args = &args;
        } @_62_

      if (!match) { @_63_
        auto it = folder.path2entry_index.find(path);
        if (it != folder.path2entry_index.end()) { @_64_
          Project::Entry &entry = folder.entries[it->second];
          exact_match = entry.filename == path;
          if ((match = exact_match || can_redirect) || entry.compdb_size) { @_65_
            // best->compdb_size is >0 for a compdb entry, 0 for a .ccls entry.
            best_compdb_folder = &folder;
            best = &entry;
          } @_65_
        } @_64_
      } @_63_
    } @_61_

  bool append = false;
  if (best_dot_ccls_args && !(append = appendToCDB(*best_dot_ccls_args)) &&
      !exact_match) { @_66_
    // If the first line is not %compile_commands.json, override the compdb
    // match if it is not an exact match.
    ret.root = ret.directory = best_dot_ccls_root;
    ret.filename = path;
    if (best_dot_ccls_args->empty()) { @_67_
      ret.args = getFallback(path);
    } else {
      ret.args = *best_dot_ccls_args;
      ret.args.push_back(intern(path));
    } @_67_
  } else {
    // If the first line is %compile_commands.json, find the matching compdb
    // entry and append .ccls args.
    if (must_exist && !match && !(best_dot_ccls_args && !append))
      return ret;
    if (!best) { @_68_
      // Infer args from a similar path.
      int best_score = INT_MIN;
      auto [lang, header] = lookupExtension(path);
      for (auto &[root, folder] : root2folder)
        if (StringRef(path).startswith(root))
          for (const Entry &e : folder.entries)
            if (e.compdb_size) { @_69_
              int score = computeGuessScore(path, e.filename);
              // Decrease score if .c is matched against .hh
              auto [lang1, header1] = lookupExtension(e.filename);
              if (lang != lang1 && !(lang == LanguageId::C && header))
                score -= 30;
              if (score > best_score) { @_70_
                best_score = score;
                best_compdb_folder = &folder;
                best = &e;
              } @_70_
            } @_69_
      ret.is_inferred = true;
    } @_68_
    if (!best) { @_71_
      ret.root = ret.directory = g_config->fallbackFolder;
      ret.args = getFallback(path);
    } else {
      // The entry may have different filename but it doesn't matter when
      // building CompilerInvocation. The main filename is specified
      // separately.
      ret.root = best->root;
      ret.directory = best->directory;
      ret.args = best->args;
      if (best->compdb_size) // delete trailing .ccls options if exist
        ret.args.resize(best->compdb_size);
      else
        best_dot_ccls_args = nullptr;
    } @_71_
    ret.filename = path;
  } @_66_

  if (best_dot_ccls_args && append && best_dot_ccls_args->size())
    ret.args.insert(ret.args.end(), best_dot_ccls_args->begin() + 1,
                    best_dot_ccls_args->end());
  if (best_compdb_folder)
    ProjectProcessor(*best_compdb_folder).process(ret);
  else if (best_dot_ccls_folder)
    ProjectProcessor(*best_dot_ccls_folder).process(ret);
  for (const std::string &arg : g_config->clang.extraArgs)
    ret.args.push_back(intern(arg));
  ret.args.push_back(intern("-working-directory=" + ret.directory));
  return ret;
} @_60_

void Project::index(WorkingFiles *wfiles, const RequestId &id) { @_72_
  auto &gi = g_config->index;
  GroupMatch match(gi.whitelist, gi.blacklist),
      match_i(gi.initialWhitelist, gi.initialBlacklist);
  std::vector<const char *> args, extra_args;
  for (const std::string &arg : g_config->clang.extraArgs)
    extra_args.push_back(intern(arg));
  { @_73_
    std::lock_guard lock(mtx);
    for (auto &[root, folder] : root2folder) { @_74_
      int i = 0;
      for (const Project::Entry &entry : folder.entries) { @_75_
        std::string reason;
        if (match.matches(entry.filename, &reason) &&
            match_i.matches(entry.filename, &reason)) { @_76_
          bool interactive = wfiles->getFile(entry.filename) != nullptr;
          args = entry.args;
          args.insert(args.end(), extra_args.begin(), extra_args.end());
          args.push_back(intern("-working-directory=" + entry.directory));
          pipeline::index(entry.filename, args,
                          interactive ? IndexMode::Normal
                                      : IndexMode::Background,
                          false, id);
        } else {
          LOG_V(1) << "[" << i << "/" << folder.entries.size()
                   << "]: " << reason << "; skip " << entry.filename;
        } @_76_
        i++;
      } @_75_
    } @_74_
  } @_73_

  pipeline::loaded_ts = pipeline::tick;
  // Dummy request to indicate that project is loaded and
  // trigger refreshing semantic highlight for all working files.
  pipeline::index("", {}, IndexMode::Background, false);
} @_72_

void Project::indexRelated(const std::string &path) { @_77_
  auto &gi = g_config->index;
  GroupMatch match(gi.whitelist, gi.blacklist);
  StringRef stem = sys::path::stem(path);
  std::vector<const char *> args, extra_args;
  for (const std::string &arg : g_config->clang.extraArgs)
    extra_args.push_back(intern(arg));
  std::lock_guard lock(mtx);
  for (auto &[root, folder] : root2folder)
    if (StringRef(path).startswith(root)) { @_78_
      for (const Project::Entry &entry : folder.entries) { @_79_
        std::string reason;
        args = entry.args;
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        args.push_back(intern("-working-directory=" + entry.directory));
        if (sys::path::stem(entry.filename) == stem && entry.filename != path &&
            match.matches(entry.filename, &reason))
          pipeline::index(entry.filename, args, IndexMode::Background, true);
      } @_79_
      break;
    } @_78_
} @_77_
} // namespace ccls @_2_
