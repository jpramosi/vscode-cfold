// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "pipeline.hh"

#include "config.hh"
#include "include_complete.hh"
#include "log.hh"
#include "lsp.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "project.hh"
#include "query.hh"
#include "sema_manager.hh"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Threading.h>

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#ifndef _WIN32 @_0_
#include <unistd.h>
#endif @_0_
using namespace llvm;
namespace chrono = std::chrono;

namespace ccls { @_1_
namespace { @_2_
struct PublishDiagnosticParam { @_3_
  DocumentUri uri;
  std::vector<Diagnostic> diagnostics;
}; @_3_
REFLECT_STRUCT(PublishDiagnosticParam, uri, diagnostics);
} // namespace @_2_

void VFS::clear() { @_4_
  std::lock_guard lock(mutex);
  state.clear();
} @_4_

int VFS::loaded(const std::string &path) { @_5_
  std::lock_guard lock(mutex);
  return state[path].loaded;
} @_5_

bool VFS::stamp(const std::string &path, int64_t ts, int step) { @_6_
  std::lock_guard<std::mutex> lock(mutex);
  State &st = state[path];
  if (st.timestamp < ts || (st.timestamp == ts && st.step < step)) { @_7_
    st.timestamp = ts;
    st.step = step;
    return true;
  } else @_7_
    return false;
} @_6_

struct MessageHandler;
void standaloneInitialize(MessageHandler &, const std::string &root);

namespace pipeline { @_8_

std::atomic<bool> g_quit;
std::atomic<int64_t> loaded_ts{0}, pending_index_requests{0}, request_id{0};
int64_t tick = 0;

namespace { @_9_

struct IndexRequest { @_10_
  std::string path;
  std::vector<const char *> args;
  IndexMode mode;
  bool must_exist = false;
  RequestId id;
  int64_t ts = tick++;
}; @_10_

std::mutex thread_mtx;
std::condition_variable no_active_threads;
int active_threads;

MultiQueueWaiter *main_waiter;
MultiQueueWaiter *indexer_waiter;
MultiQueueWaiter *stdout_waiter;
ThreadedQueue<InMessage> *on_request;
ThreadedQueue<IndexRequest> *index_request;
ThreadedQueue<IndexUpdate> *on_indexed;
ThreadedQueue<std::string> *for_stdout;

struct InMemoryIndexFile { @_11_
  std::string content;
  IndexFile index;
}; @_11_
std::shared_mutex g_index_mutex;
std::unordered_map<std::string, InMemoryIndexFile> g_index;

bool cacheInvalid(VFS *vfs, IndexFile *prev, const std::string &path,
                  const std::vector<const char *> &args,
                  const std::optional<std::string> &from) { @_12_
  { @_13_
    std::lock_guard<std::mutex> lock(vfs->mutex);
    if (prev->mtime < vfs->state[path].timestamp) { @_14_
      LOG_V(1) << "timestamp changed for " << path
               << (from ? " (via " + *from + ")" : std::string());
      return true;
    } @_14_
  } @_13_

  // For inferred files, allow -o a a.cc -> -o b b.cc
  StringRef stem = sys::path::stem(path);
  int changed = -1, size = std::min(prev->args.size(), args.size());
  for (int i = 0; i < size; i++)
    if (strcmp(prev->args[i], args[i]) && sys::path::stem(args[i]) != stem) { @_15_
      changed = i;
      break;
    } @_15_
  if (changed < 0 && prev->args.size() != args.size())
    changed = size;
  if (changed >= 0)
    LOG_V(1) << "args changed for " << path
             << (from ? " (via " + *from + ")" : std::string()) << "; old: "
             << (changed < prev->args.size() ? prev->args[changed] : "")
             << "; new: " << (changed < size ? args[changed] : "");
  return changed >= 0;
}; @_12_

std::string appendSerializationFormat(const std::string &base) { @_16_
  switch (g_config->cache.format) { @_17_
  case SerializeFormat::Binary:
    return base + ".blob";
  case SerializeFormat::Json:
    return base + ".json";
  } @_17_
} @_16_

std::string getCachePath(std::string src) { @_18_
  if (g_config->cache.hierarchicalPath) { @_19_
    std::string ret = src[0] == '/' ? src.substr(1) : src;
#ifdef _WIN32 @_20_
    std::replace(ret.begin(), ret.end(), ':', '@');
#endif @_20_
    return g_config->cache.directory + ret;
  } @_19_
  for (auto &[root, _] : g_config->workspaceFolders)
    if (StringRef(src).startswith(root)) { @_21_
      auto len = root.size();
      return g_config->cache.directory +
             escapeFileName(root.substr(0, len - 1)) + '/' +
             escapeFileName(src.substr(len));
    } @_21_
  return g_config->cache.directory + '@' +
         escapeFileName(g_config->fallbackFolder.substr(
             0, g_config->fallbackFolder.size() - 1)) +
         '/' + escapeFileName(src);
} @_18_

std::unique_ptr<IndexFile> rawCacheLoad(const std::string &path) { @_22_
  if (g_config->cache.retainInMemory) { @_23_
    std::shared_lock lock(g_index_mutex);
    auto it = g_index.find(path);
    if (it != g_index.end())
      return std::make_unique<IndexFile>(it->second.index);
    if (g_config->cache.directory.empty())
      return nullptr;
  } @_23_

  std::string cache_path = getCachePath(path);
  std::optional<std::string> file_content = readContent(cache_path);
  std::optional<std::string> serialized_indexed_content =
      readContent(appendSerializationFormat(cache_path));
  if (!file_content || !serialized_indexed_content)
    return nullptr;

  return ccls::deserialize(g_config->cache.format, path,
                           *serialized_indexed_content, *file_content,
                           IndexFile::kMajorVersion);
} @_22_

std::mutex &getFileMutex(const std::string &path) { @_24_
  const int n_MUTEXES = 256;
  static std::mutex mutexes[n_MUTEXES];
  return mutexes[std::hash<std::string>()(path) % n_MUTEXES];
} @_24_

bool indexer_Parse(SemaManager *completion, WorkingFiles *wfiles,
                   Project *project, VFS *vfs, const GroupMatch &matcher) { @_25_
  std::optional<IndexRequest> opt_request = index_request->tryPopFront();
  if (!opt_request)
    return false;
  auto &request = *opt_request;
  bool loud = request.mode != IndexMode::OnChange;
  struct RAII { @_26_
    ~RAII() { pending_index_requests--; }
  } raii; @_26_

  // Dummy one to trigger refresh semantic highlight.
  if (request.path.empty()) { @_27_
    IndexUpdate dummy;
    dummy.refresh = true;
    on_indexed->pushBack(std::move(dummy), false);
    return false;
  } @_27_

  if (!matcher.matches(request.path)) { @_28_
    LOG_IF_S(INFO, loud) << "skip " << request.path;
    return false;
  } @_28_

  Project::Entry entry =
      project->findEntry(request.path, true, request.must_exist);
  if (request.must_exist && entry.filename.empty())
    return true;
  if (request.args.size())
    entry.args = request.args;
  std::string path_to_index = entry.filename;
  std::unique_ptr<IndexFile> prev;

  bool deleted = request.mode == IndexMode::Delete,
       no_linkage = g_config->index.initialNoLinkage ||
                    request.mode != IndexMode::Background;
  int reparse = 0;
  if (deleted)
    reparse = 2;
  else if (!(g_config->index.onChange && wfiles->getFile(path_to_index))) { @_29_
    std::optional<int64_t> write_time = lastWriteTime(path_to_index);
    if (!write_time) { @_30_
      deleted = true;
    } else {
      if (vfs->stamp(path_to_index, *write_time, no_linkage ? 2 : 0))
        reparse = 1;
      if (request.path != path_to_index) { @_31_
        std::optional<int64_t> mtime1 = lastWriteTime(request.path);
        if (!mtime1)
          deleted = true;
        else if (vfs->stamp(request.path, *mtime1, no_linkage ? 2 : 0))
          reparse = 2;
      } @_31_
    } @_30_
  } @_29_

  if (g_config->index.onChange) { @_32_
    reparse = 2;
    std::lock_guard lock(vfs->mutex);
    vfs->state[path_to_index].step = 0;
    if (request.path != path_to_index)
      vfs->state[request.path].step = 0;
  } @_32_
  bool track = g_config->index.trackDependency > 1 ||
               (g_config->index.trackDependency == 1 && request.ts < loaded_ts);
  if (!reparse && !track)
    return true;

  if (reparse < 2)
    do { @_33_
      std::unique_lock lock(getFileMutex(path_to_index));
      prev = rawCacheLoad(path_to_index);
      if (!prev || prev->no_linkage < no_linkage ||
          cacheInvalid(vfs, prev.get(), path_to_index, entry.args,
                       std::nullopt))
        break;
      if (track)
        for (const auto &dep : prev->dependencies) { @_34_
          if (auto mtime1 = lastWriteTime(dep.first.val().str())) { @_35_
            if (dep.second < *mtime1) { @_36_
              reparse = 2;
              LOG_V(1) << "timestamp changed for " << path_to_index << " via "
                       << dep.first.val().str();
              break;
            } @_36_
          } else {
            reparse = 2;
            LOG_V(1) << "timestamp changed for " << path_to_index << " via "
                     << dep.first.val().str();
            break;
          } @_35_
        } @_34_
      if (reparse == 0)
        return true;
      if (reparse == 2)
        break;

      if (vfs->loaded(path_to_index))
        return true;
      LOG_S(INFO) << "load cache for " << path_to_index;
      auto dependencies = prev->dependencies;
      IndexUpdate update = IndexUpdate::createDelta(nullptr, prev.get());
      on_indexed->pushBack(std::move(update),
                           request.mode != IndexMode::Background);
      { @_37_
        std::lock_guard lock1(vfs->mutex);
        VFS::State &st = vfs->state[path_to_index];
        st.loaded++;
        if (prev->no_linkage)
          st.step = 2;
      } @_37_
      lock.unlock();

      for (const auto &dep : dependencies) { @_38_
        std::string path = dep.first.val().str();
        if (!vfs->stamp(path, dep.second, 1))
          continue;
        std::lock_guard lock1(getFileMutex(path));
        prev = rawCacheLoad(path);
        if (!prev)
          continue;
        { @_39_
          std::lock_guard lock2(vfs->mutex);
          VFS::State &st = vfs->state[path];
          if (st.loaded)
            continue;
          st.loaded++;
          st.timestamp = prev->mtime;
          if (prev->no_linkage)
            st.step = 3;
        } @_39_
        IndexUpdate update = IndexUpdate::createDelta(nullptr, prev.get());
        on_indexed->pushBack(std::move(update),
                             request.mode != IndexMode::Background);
        if (entry.id >= 0) { @_40_
          std::lock_guard lock2(project->mtx);
          project->root2folder[entry.root].path2entry_index[path] = entry.id;
        } @_40_
      } @_38_
      return true;
    } while (0); @_33_

  if (loud) { @_41_
    std::string line;
    if (LOG_V_ENABLED(1)) { @_42_
      line = "\n ";
      for (auto &arg : entry.args)
        (line += ' ') += arg;
    } @_42_
    LOG_S(INFO) << (deleted ? "delete " : "parse ") << path_to_index << line;
  } @_41_

  std::vector<std::unique_ptr<IndexFile>> indexes;
  if (deleted) { @_43_
    indexes.push_back(std::make_unique<IndexFile>(request.path, "", false));
    if (request.path != path_to_index)
      indexes.push_back(std::make_unique<IndexFile>(path_to_index, "", false));
  } else {
    std::vector<std::pair<std::string, std::string>> remapped;
    if (g_config->index.onChange) { @_44_
      std::string content = wfiles->getContent(path_to_index);
      if (content.size())
        remapped.emplace_back(path_to_index, content);
    } @_44_
    bool ok;
    indexes = idx::index(completion, wfiles, vfs, entry.directory,
                         path_to_index, entry.args, remapped, no_linkage, ok);

    if (!ok) { @_45_
      if (request.id.valid()) { @_46_
        ResponseError err;
        err.code = ErrorCode::InternalError;
        err.message = "failed to index " + path_to_index;
        pipeline::replyError(request.id, err);
      } @_46_
      return true;
    } @_45_
  } @_43_

  for (std::unique_ptr<IndexFile> &curr : indexes) { @_47_
    std::string path = curr->path;
    if (!matcher.matches(path)) { @_48_
      LOG_IF_S(INFO, loud) << "skip index for " << path;
      continue;
    } @_48_

    if (!deleted)
      LOG_IF_S(INFO, loud) << "store index for " << path
                           << " (delta: " << !!prev << ")";
    { @_49_
      std::lock_guard lock(getFileMutex(path));
      int loaded = vfs->loaded(path), retain = g_config->cache.retainInMemory;
      if (loaded)
        prev = rawCacheLoad(path);
      else
        prev.reset();
      if (retain > 0 && retain <= loaded + 1) { @_50_
        std::lock_guard lock(g_index_mutex);
        auto it = g_index.insert_or_assign(
            path, InMemoryIndexFile{curr->file_contents, *curr});
        std::string().swap(it.first->second.index.file_contents);
      } @_50_
      if (g_config->cache.directory.size()) { @_51_
        std::string cache_path = getCachePath(path);
        if (deleted) { @_52_
          (void)sys::fs::remove(cache_path);
          (void)sys::fs::remove(appendSerializationFormat(cache_path));
        } else {
          if (g_config->cache.hierarchicalPath)
            sys::fs::create_directories(
                sys::path::parent_path(cache_path, sys::path::Style::posix),
                true);
          writeToFile(cache_path, curr->file_contents);
          writeToFile(appendSerializationFormat(cache_path),
                      serialize(g_config->cache.format, *curr));
        } @_52_
      } @_51_
      on_indexed->pushBack(IndexUpdate::createDelta(prev.get(), curr.get()),
                           request.mode != IndexMode::Background);
      { @_53_
        std::lock_guard lock1(vfs->mutex);
        vfs->state[path].loaded++;
      } @_53_
      if (entry.id >= 0) { @_54_
        std::lock_guard lock(project->mtx);
        auto &folder = project->root2folder[entry.root];
        for (auto &dep : curr->dependencies)
          folder.path2entry_index[dep.first.val().str()] = entry.id;
      } @_54_
    } @_49_
  } @_47_

  return true;
} @_25_

void quit(SemaManager &manager) { @_55_
  g_quit.store(true, std::memory_order_relaxed);
  manager.quit();

  { std::lock_guard lock(index_request->mutex_); }
  indexer_waiter->cv.notify_all();
  { std::lock_guard lock(for_stdout->mutex_); }
  stdout_waiter->cv.notify_one();
  std::unique_lock lock(thread_mtx);
  no_active_threads.wait(lock, [] { return !active_threads; });
} @_55_

} // namespace @_9_

void threadEnter() { @_56_
  std::lock_guard lock(thread_mtx);
  active_threads++;
} @_56_

void threadLeave() { @_57_
  std::lock_guard lock(thread_mtx);
  if (!--active_threads)
    no_active_threads.notify_one();
} @_57_

void init() { @_58_
  main_waiter = new MultiQueueWaiter;
  on_request = new ThreadedQueue<InMessage>(main_waiter);
  on_indexed = new ThreadedQueue<IndexUpdate>(main_waiter);

  indexer_waiter = new MultiQueueWaiter;
  index_request = new ThreadedQueue<IndexRequest>(indexer_waiter);

  stdout_waiter = new MultiQueueWaiter;
  for_stdout = new ThreadedQueue<std::string>(stdout_waiter);
} @_58_

void indexer_Main(SemaManager *manager, VFS *vfs, Project *project,
                  WorkingFiles *wfiles) { @_59_
  GroupMatch matcher(g_config->index.whitelist, g_config->index.blacklist);
  while (true)
    if (!indexer_Parse(manager, wfiles, project, vfs, matcher))
      if (indexer_waiter->wait(g_quit, index_request))
        break;
} @_59_

void main_OnIndexed(DB *db, WorkingFiles *wfiles, IndexUpdate *update) { @_60_
  if (update->refresh) { @_61_
    LOG_S(INFO)
        << "loaded project. Refresh semantic highlight for all working file.";
    std::lock_guard lock(wfiles->mutex);
    for (auto &[f, wf] : wfiles->files) { @_62_
      std::string path = lowerPathIfInsensitive(f);
      if (db->name2file_id.find(path) == db->name2file_id.end())
        continue;
      QueryFile &file = db->files[db->name2file_id[path]];
      emitSemanticHighlight(db, wf.get(), file);
    } @_62_
    return;
  } @_61_

  db->applyIndexUpdate(update);

  // Update indexed content, skipped ranges, and semantic highlighting.
  if (update->files_def_update) { @_63_
    auto &def_u = *update->files_def_update;
    if (WorkingFile *wfile = wfiles->getFile(def_u.first.path)) { @_64_
      // FIXME With index.onChange: true, use buffer_content only for
      // request.path
      wfile->setIndexContent(g_config->index.onChange ? wfile->buffer_content
                                                      : def_u.second);
      QueryFile &file = db->files[update->file_id];
      emitSkippedRanges(wfile, file);
      emitSemanticHighlight(db, wfile, file);
    } @_64_
  } @_63_
} @_60_

void launchStdin() { @_65_
  threadEnter();
  std::thread([]() { @_66_
    set_thread_name("stdin");
    std::string str;
    const std::string_view kContentLength("Content-Length: ");
    bool received_exit = false;
    while (true) { @_67_
      int len = 0;
      str.clear();
      while (true) { @_68_
        int c = getchar();
        if (c == EOF)
          goto quit;
        if (c == '\n') { @_69_
          if (str.empty())
            break;
          if (!str.compare(0, kContentLength.size(), kContentLength))
            len = atoi(str.c_str() + kContentLength.size());
          str.clear();
        } else if (c != '\r') {
          str += c;
        } @_69_
      } @_68_

      str.resize(len);
      for (int i = 0; i < len; ++i) { @_70_
        int c = getchar();
        if (c == EOF)
          goto quit;
        str[i] = c;
      } @_70_

      auto message = std::make_unique<char[]>(len);
      std::copy(str.begin(), str.end(), message.get());
      auto document = std::make_unique<rapidjson::Document>();
      document->Parse(message.get(), len);
      assert(!document->HasParseError());

      JsonReader reader{document.get()};
      if (!reader.m->HasMember("jsonrpc") ||
          std::string((*reader.m)["jsonrpc"].GetString()) != "2.0")
        break;
      RequestId id;
      std::string method;
      reflectMember(reader, "id", id);
      reflectMember(reader, "method", method);
      if (id.valid())
        LOG_V(2) << "receive RequestMessage: " << id.value << " " << method;
      else
        LOG_V(2) << "receive NotificationMessage " << method;
      if (method.empty())
        continue;
      received_exit = method == "exit";
      // g_config is not available before "initialize". Use 0 in that case.
      on_request->pushBack(
          {id, std::move(method), std::move(message), std::move(document), @_71_
           chrono::steady_clock::now() +
               chrono::milliseconds(g_config ? g_config->request.timeout : 0)}); @_71_

      if (received_exit)
        break;
    } @_67_

  quit:
    if (!received_exit) { @_72_
      const std::string_view str("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
      auto message = std::make_unique<char[]>(str.size());
      std::copy(str.begin(), str.end(), message.get());
      auto document = std::make_unique<rapidjson::Document>();
      document->Parse(message.get(), str.size());
      on_request->pushBack({RequestId(), std::string("exit"), @_73_
                            std::move(message), std::move(document),
                            chrono::steady_clock::now()}); @_73_
    } @_72_
    threadLeave();
  }).detach(); @_66_
} @_65_

void launchStdout() { @_74_
  threadEnter();
  std::thread([]() { @_75_
    set_thread_name("stdout");

    while (true) { @_76_
      std::vector<std::string> messages = for_stdout->dequeueAll();
      for (auto &s : messages) { @_77_
        llvm::outs() << "Content-Length: " << s.size() << "\r\n\r\n" << s;
        llvm::outs().flush();
      } @_77_
      if (stdout_waiter->wait(g_quit, for_stdout))
        break;
    } @_76_
    threadLeave();
  }).detach(); @_75_
} @_74_

void mainLoop() { @_78_
  Project project;
  WorkingFiles wfiles;
  VFS vfs;

  SemaManager manager(
      &project, &wfiles,
      [](const std::string &path, std::vector<Diagnostic> diagnostics) { @_79_
        PublishDiagnosticParam params;
        params.uri = DocumentUri::fromPath(path);
        params.diagnostics = std::move(diagnostics);
        notify("textDocument/publishDiagnostics", params);
      }, @_79_
      [](const RequestId &id) { @_80_
        if (id.valid()) { @_81_
          ResponseError err;
          err.code = ErrorCode::InternalError;
          err.message = "drop older completion request";
          replyError(id, err);
        } @_81_
      }); @_80_

  IncludeComplete include_complete(&project);
  DB db;

  // Setup shared references.
  MessageHandler handler;
  handler.db = &db;
  handler.project = &project;
  handler.vfs = &vfs;
  handler.wfiles = &wfiles;
  handler.manager = &manager;
  handler.include_complete = &include_complete;

  bool has_indexed = false;
  std::deque<InMessage> backlog;
  StringMap<std::deque<InMessage *>> path2backlog;
  while (true) { @_82_
    if (backlog.size()) { @_83_
      auto now = chrono::steady_clock::now();
      handler.overdue = true;
      while (backlog.size()) { @_84_
        if (backlog[0].backlog_path.size()) { @_85_
          if (now < backlog[0].deadline)
            break;
          handler.run(backlog[0]);
          path2backlog[backlog[0].backlog_path].pop_front();
        } @_85_
        backlog.pop_front();
      } @_84_
      handler.overdue = false;
    } @_83_

    std::vector<InMessage> messages = on_request->dequeueAll();
    bool did_work = messages.size();
    for (InMessage &message : messages)
      try { @_86_
        handler.run(message);
      } catch (NotIndexed &ex) {
        backlog.push_back(std::move(message));
        backlog.back().backlog_path = ex.path;
        path2backlog[ex.path].push_back(&backlog.back());
      } @_86_

    bool indexed = false;
    for (int i = 20; i--;) { @_87_
      std::optional<IndexUpdate> update = on_indexed->tryPopFront();
      if (!update)
        break;
      did_work = true;
      indexed = true;
      main_OnIndexed(&db, &wfiles, &*update);
      if (update->files_def_update) { @_88_
        auto it = path2backlog.find(update->files_def_update->first.path);
        if (it != path2backlog.end()) { @_89_
          for (auto &message : it->second) { @_90_
            handler.run(*message);
            message->backlog_path.clear();
          } @_90_
          path2backlog.erase(it);
        } @_89_
      } @_88_
    } @_87_

    if (did_work) { @_91_
      has_indexed |= indexed;
      if (g_quit.load(std::memory_order_relaxed))
        break;
    } else {
      if (has_indexed) { @_92_
        freeUnusedMemory();
        has_indexed = false;
      } @_92_
      if (backlog.empty())
        main_waiter->wait(g_quit, on_indexed, on_request);
      else
        main_waiter->waitUntil(backlog[0].deadline, on_indexed, on_request);
    } @_91_
  } @_82_

  quit(manager);
} @_78_

void standalone(const std::string &root) { @_93_
  Project project;
  WorkingFiles wfiles;
  VFS vfs;
  SemaManager manager(
      nullptr, nullptr,
      [](const std::string &, const std::vector<Diagnostic> &) {},
      [](const RequestId &id) {});
  IncludeComplete complete(&project);

  MessageHandler handler;
  handler.project = &project;
  handler.wfiles = &wfiles;
  handler.vfs = &vfs;
  handler.manager = &manager;
  handler.include_complete = &complete;

  standaloneInitialize(handler, root);
  bool tty = sys::Process::StandardOutIsDisplayed();

  if (tty) { @_94_
    int entries = 0;
    for (auto &[_, folder] : project.root2folder)
      entries += folder.entries.size();
    printf("entries: %5d\n", entries);
  } @_94_
  while (1) { @_95_
    (void)on_indexed->dequeueAll();
    int pending = pending_index_requests;
    if (tty) { @_96_
      printf("\rpending: %5d", pending);
      fflush(stdout);
    } @_96_
    if (!pending)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } @_95_
  if (tty)
    puts("");
  quit(manager);
} @_93_

void index(const std::string &path, const std::vector<const char *> &args,
           IndexMode mode, bool must_exist, RequestId id) { @_97_
  pending_index_requests++;
  index_request->pushBack({path, args, mode, must_exist, std::move(id)},
                          mode != IndexMode::Background);
} @_97_

void removeCache(const std::string &path) { @_98_
  if (g_config->cache.directory.size()) { @_99_
    std::lock_guard lock(g_index_mutex);
    g_index.erase(path);
  } @_99_
} @_98_

std::optional<std::string> loadIndexedContent(const std::string &path) { @_100_
  if (g_config->cache.directory.empty()) { @_101_
    std::shared_lock lock(g_index_mutex);
    auto it = g_index.find(path);
    if (it == g_index.end())
      return {};
    return it->second.content;
  } @_101_
  return readContent(getCachePath(path));
} @_100_

void notifyOrRequest(const char *method, bool request,
                     const std::function<void(JsonWriter &)> &fn) { @_102_
  rapidjson::StringBuffer output;
  rapidjson::Writer<rapidjson::StringBuffer> w(output);
  w.StartObject();
  w.Key("jsonrpc");
  w.String("2.0");
  w.Key("method");
  w.String(method);
  if (request) { @_103_
    w.Key("id");
    w.Int64(request_id.fetch_add(1, std::memory_order_relaxed));
  } @_103_
  w.Key("params");
  JsonWriter writer(&w);
  fn(writer);
  w.EndObject();
  LOG_V(2) << (request ? "RequestMessage: " : "NotificationMessage: ")
           << method;
  for_stdout->pushBack(output.GetString());
} @_102_

static void reply(const RequestId &id, const char *key,
                  const std::function<void(JsonWriter &)> &fn) { @_104_
  rapidjson::StringBuffer output;
  rapidjson::Writer<rapidjson::StringBuffer> w(output);
  w.StartObject();
  w.Key("jsonrpc");
  w.String("2.0");
  w.Key("id");
  switch (id.type) { @_105_
  case RequestId::kNone:
    w.Null();
    break;
  case RequestId::kInt:
    w.Int64(atoll(id.value.c_str()));
    break;
  case RequestId::kString:
    w.String(id.value.c_str(), id.value.size());
    break;
  } @_105_
  w.Key(key);
  JsonWriter writer(&w);
  fn(writer);
  w.EndObject();
  if (id.valid())
    LOG_V(2) << "respond to RequestMessage: " << id.value;
  for_stdout->pushBack(output.GetString());
} @_104_

void reply(const RequestId &id, const std::function<void(JsonWriter &)> &fn) { @_106_
  reply(id, "result", fn);
} @_106_

void replyError(const RequestId &id,
                const std::function<void(JsonWriter &)> &fn) { @_107_
  reply(id, "error", fn);
} @_107_
} // namespace pipeline @_8_
} // namespace ccls @_1_
