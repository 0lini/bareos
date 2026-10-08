/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/**
 * @file
 * Walker thread / backup thread split of the backup pass.
 *
 * The walker thread runs FindFiles() on the job's FindFilesPacket.  Instead of
 * saving each entry it takes a snapshot of the packet and puts it into a
 * bounded queue, and it adds the entry to the discovered totals.
 *
 * The backup thread (the thread calling FindFilesWithWalker()) takes the
 * entries out of the queue, loads them into its own FindFilesPacket and calls
 * the save callback.  It also does the work FindOneFile() normally does right
 * after the callback returns and which depends on the result of the save:
 *  - hard link bookkeeping (the walker cannot know the FileIndex of the first
 *    link yet, so all hard link decisions are made here),
 *  - restoring the access times (the walker restores them before the file
 *    was read, so it has to be done again after reading).
 *
 * Plugins are only ever called from the backup thread.  As plugins can
 * modify the fileset and the accurate data, the walker waits for command
 * plugin entries and for files handled by option plugins to be processed
 * before it continues.
 */

#include "include/bareos.h"
#include "include/filetypes.h"
#include "include/jcr.h"
#include "filed/backup_walker.h"
#include "filed/filed_jcr_impl.h"
#include "findlib/find.h"
#include "findlib/find_one.h"
#include "lib/thread_specific_data.h"

#include <atomic>
#include <future>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace filedaemon {

namespace {

// Snapshot of everything SaveFile() needs from the FindFilesPacket.
struct QueuedEntry {
  enum class Kind
  {
    kFile,
    kPluginCommand
  };
  Kind kind{Kind::kFile};
  bool top_level{false};
  bool hardlinked{false};
  std::uint64_t accounted_bytes{0};
  // set if the walker waits for the result of this entry
  std::optional<std::promise<int>> completion{};

  std::string fname{};
  std::string link_or_dir{};
  std::string top_fname{};
  struct stat statp{};
  int type{FT_UNSET};
  int ff_errno{0};
  int32_t delta_seq{0};
  bool accurate_found{false};
  char flags[FOPTS_BYTES]{};
  uint32_t Compress_algo{0};
  int Compress_level{0};
  int StripPath{0};
  s_sz_matching* size_match{nullptr};
  char VerifyOpts[MAX_OPTS]{};
  uint64_t accurate_opts{0};
  uint64_t base_job_opts{0};
  char* plugin{nullptr};
  bool opt_plugin{false};
  bool volhas_attrlist{false};
  HfsPlusInfo hfsinfo{};

  std::size_t Cost() const
  {
    return sizeof(*this) + fname.capacity() + link_or_dir.capacity()
           + top_fname.capacity();
  }
};

QueuedEntry SnapshotFile(const FindFilesPacket* ff, bool top_level)
{
  QueuedEntry e;
  e.kind = QueuedEntry::Kind::kFile;
  e.top_level = top_level;
  e.hardlinked = ff->linked != nullptr;
  e.fname = ff->fname;
  e.link_or_dir = ff->link_or_dir ? ff->link_or_dir : ff->fname;
  if (ff->top_fname) { e.top_fname = ff->top_fname; }
  e.statp = ff->statp;
  e.type = ff->type;
  e.ff_errno = ff->ff_errno;
  e.delta_seq = ff->delta_seq;
  e.accurate_found = ff->accurate_found;
  memcpy(e.flags, ff->flags, sizeof(e.flags));
  e.Compress_algo = ff->Compress_algo;
  e.Compress_level = ff->Compress_level;
  e.StripPath = ff->StripPath;
  e.size_match = ff->size_match;
  memcpy(e.VerifyOpts, ff->VerifyOpts, sizeof(e.VerifyOpts));
  e.accurate_opts = ff->accurate_opts;
  e.base_job_opts = ff->base_job_opts;
  e.plugin = ff->plugin;
  e.opt_plugin = ff->opt_plugin;
  e.volhas_attrlist = ff->volhas_attrlist;
  e.hfsinfo = ff->hfsinfo;
  return e;
}

// Load a snapshot into the backup thread's packet.  The strings stay owned
// by the entry, so the packet is only valid as long as the entry lives.
void LoadEntry(QueuedEntry& e, FindFilesPacket* ff)
{
  ff->fname = e.fname.data();
  ff->link_or_dir = e.link_or_dir.data();
  ff->top_fname = e.top_fname.data();
  ff->statp = e.statp;
  ff->type = e.type;
  ff->ff_errno = e.ff_errno;
  ff->delta_seq = e.delta_seq;
  ff->accurate_found = e.accurate_found;
  memcpy(ff->flags, e.flags, sizeof(ff->flags));
  ff->Compress_algo = e.Compress_algo;
  ff->Compress_level = e.Compress_level;
  ff->StripPath = e.StripPath;
  ff->size_match = e.size_match;
  memcpy(ff->VerifyOpts, e.VerifyOpts, sizeof(ff->VerifyOpts));
  ff->accurate_opts = e.accurate_opts;
  ff->base_job_opts = e.base_job_opts;
  ff->plugin = e.plugin;
  ff->opt_plugin = e.opt_plugin;
  ff->volhas_attrlist = e.volhas_attrlist;
  ff->hfsinfo = e.hfsinfo;

  ff->cmd_plugin = false;
  ff->no_read = false;
  ff->FileIndex = 0;
  ff->LinkFI = 0;
  ff->linked = nullptr;
  ff->digest = nullptr;
  ff->digest_len = 0;
  ff->digest_stream = 0;
}

void UnloadEntry(FindFilesPacket* ff)
{
  ff->fname = nullptr;
  ff->link_or_dir = nullptr;
  ff->top_fname = nullptr;
  ff->linked = nullptr;
  ff->digest = nullptr;
}

}  // namespace

class BackupWalker {
 public:
  BackupWalker(JobControlRecord* jcr,
               FindFilesPacket* ff,
               FileCallback* save_file,
               FileCallback* plugin_save,
               std::size_t max_queue_bytes)
      : jcr_{jcr}
      , ff_{ff}
      , save_file_{save_file}
      , plugin_save_{plugin_save}
      , queue_{max_queue_bytes}
  {
  }

  int Run();

 private:
  static BackupWalker* From(JobControlRecord* jcr)
  { return jcr->fd_impl->backup_walker; }
  // walker thread
  void WalkerMain();
  static int EnqueueFile(JobControlRecord* jcr,
                         FindFilesPacket* ff,
                         bool top_level);
  static int EnqueuePluginCommand(JobControlRecord* jcr,
                                  FindFilesPacket* ff,
                                  bool top_level);
  std::uint64_t AccountBytes(const FindFilesPacket* ff);
  int Enqueue(QueuedEntry entry, bool wait_for_result);

  // backup thread
  int ConsumeAll(FindFilesPacket* work);
  int Process(QueuedEntry& e, FindFilesPacket* work);

  JobControlRecord* jcr_;
  FindFilesPacket* ff_;
  FileCallback* save_file_;
  FileCallback* plugin_save_;
  BoundedWorkQueue<QueuedEntry> queue_;
  std::atomic<bool> stopped_{false};
  int walk_result_{0};  // only valid after the walker was joined

  // hard links whose bytes were already accounted for (walker only)
  std::unordered_set<Hardlink> counted_links_;
};

std::uint64_t BackupWalker::AccountBytes(const FindFilesPacket* ff)
{
  // only regular files with data are read
  if (ff->type != FT_REG || !S_ISREG(ff->statp.st_mode)) { return 0; }
  if (ff->linked) {
    // the data of a hard linked file is only saved once
    auto [_, inserted]
        = counted_links_.insert(Hardlink{ff->statp.st_dev, ff->statp.st_ino});
    if (!inserted) { return 0; }
  }
  return static_cast<std::uint64_t>(ff->statp.st_size);
}

int BackupWalker::Enqueue(QueuedEntry entry, bool wait_for_result)
{
  std::future<int> result;
  if (wait_for_result) {
    entry.completion.emplace();
    result = entry.completion->get_future();
  }

  const std::size_t cost = entry.Cost();
  if (!queue_.Push(std::move(entry), cost)) { return 0; }
  if (!wait_for_result) { return 1; }

  try {
    return result.get();
  } catch (const std::future_error&) {
    // the entry was dropped because the backup thread stopped
    return 0;
  }
}

int BackupWalker::EnqueueFile(JobControlRecord* jcr,
                              FindFilesPacket* ff,
                              bool top_level)
{
  BackupWalker* self = From(jcr);
  if (jcr->IsJobCanceled() || self->stopped_) { return 0; }

  // SaveFile() ignores this, the directory is saved with its FT_DIREND
  if (ff->type == FT_DIRBEGIN) { return 1; }

  QueuedEntry entry = SnapshotFile(ff, top_level);
  entry.accounted_bytes = self->AccountBytes(ff);
  jcr->fd_impl->progress.Discovered(entry.accounted_bytes);

  // option plugins may change the fileset or the accurate data
  return self->Enqueue(std::move(entry), ff->opt_plugin);
}

int BackupWalker::EnqueuePluginCommand(JobControlRecord* jcr,
                                       FindFilesPacket*,
                                       bool top_level)
{
  BackupWalker* self = From(jcr);
  if (jcr->IsJobCanceled() || self->stopped_) { return 0; }

  /* The plugin works directly on the walker's packet (ff->top_fname and
   * ff->cmd_plugin are set by FindFiles()), so the walker has to wait. */
  QueuedEntry entry;
  entry.kind = QueuedEntry::Kind::kPluginCommand;
  entry.top_level = top_level;
  return self->Enqueue(std::move(entry), true);
}

void BackupWalker::WalkerMain()
{
  SetJcrInThreadSpecificData(jcr_);
  walk_result_ = FindFiles(jcr_, ff_, EnqueueFile,
                           plugin_save_ ? EnqueuePluginCommand : nullptr);
  Dmsg2(100, "walker done result=%d queued=%" PRIuz "\n", walk_result_,
        queue_.size());
  jcr_->fd_impl->progress.WalkFinished();
  queue_.Close();
}

int BackupWalker::Process(QueuedEntry& e, FindFilesPacket* work)
{
  if (e.kind == QueuedEntry::Kind::kPluginCommand) {
    return plugin_save_(jcr_, ff_, e.top_level);
  }

  LoadEntry(e, work);

  if (e.hardlinked) {
    switch (ResolveHardlink(work, work->fname)) {
      case HardlinkState::kFirst:
      case HardlinkState::kAlreadySaved:
        break;
      case HardlinkState::kSameName:
        jcr_->fd_impl->progress.Processed(e.accounted_bytes);
        UnloadEntry(work);
        return 1;
    }
  }

  const bool is_link_saved = work->type == FT_LNKSAVED;
  int rc = save_file_(jcr_, work, e.top_level);
  if (work->linked) { work->linked->FileIndex = work->FileIndex; }

  // The walker restored the times before the file was read, do it again.
  if (BitIsSet(FO_KEEPATIME, e.flags) && !is_link_saved
      && (e.type == FT_REG || e.type == FT_REGE || e.type == FT_DIREND)) {
    RestoreFileTimes(work, e.fname.c_str());
  }

  jcr_->fd_impl->progress.Processed(e.accounted_bytes);
  UnloadEntry(work);
  return rc;
}

int BackupWalker::ConsumeAll(FindFilesPacket* work)
{
  int result = 1;
  while (std::optional entry = queue_.Pop()) {
    const int rc = Process(*entry, work);
    if (entry->completion) { entry->completion->set_value(rc); }

    if (rc == 0) {
      // like FindFiles(): only a failure of a top level entry is an error
      if (entry->top_level) { result = 0; }
      if (jcr_->IsJobCanceled() || jcr_->IsIncomplete()) {
        Dmsg0(100, "backup thread stops, job canceled or incomplete\n");
        result = 0;
        stopped_ = true;
        queue_.Abort();
        break;
      }
    }
  }
  return result;
}

int BackupWalker::Run()
{
  jcr_->fd_impl->progress.Start();

  // The backup thread's own packet.  Hard link data lives in its linkhash.
  FindFilesPacket* work = init_find_files();
  work->fileset = ff_->fileset;
  work->incremental = ff_->incremental;
  work->save_time = ff_->save_time;
  work->CheckFct = ff_->CheckFct;

  jcr_->fd_impl->backup_walker = this;

  std::thread walker;
  try {
    walker = std::thread([this] { WalkerMain(); });
  } catch (const std::system_error& e) {
    Jmsg(jcr_, M_WARNING, 0,
         T_("Cannot start walker thread, backing up without it: %s\n"),
         e.what());
    jcr_->fd_impl->backup_walker = nullptr;
    jcr_->fd_impl->progress.Disable();
    TermFindFiles(work);
    return FindFiles(jcr_, ff_, save_file_, plugin_save_);
  }

  const int consume_result = ConsumeAll(work);
  walker.join();

  jcr_->fd_impl->backup_walker = nullptr;
  TermFindFiles(work);

  Dmsg2(100, "walk result=%d consume result=%d\n", walk_result_,
        consume_result);
  return (walk_result_ && consume_result) ? 1 : 0;
}

int FindFilesWithWalker(JobControlRecord* jcr,
                        FindFilesPacket* ff,
                        FileCallback* save_file,
                        FileCallback* plugin_save,
                        std::size_t max_queue_bytes)
{
  BackupWalker walker{jcr, ff, save_file, plugin_save, max_queue_bytes};
  return walker.Run();
}

}  // namespace filedaemon
