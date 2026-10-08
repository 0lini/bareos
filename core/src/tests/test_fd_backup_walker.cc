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

#include "include/bareos.h"
#include "include/filetypes.h"
#include "include/jcr.h"
#include "filed/backup_progress.h"
#include "filed/backup_walker.h"
#include "filed/dir_cmd.h"
#include "filed/filed_jcr_impl.h"
#include "filed/fileset.h"
#include "findlib/find.h"
#include "gtest/gtest.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace filedaemon;
namespace fs = std::filesystem;

// ---- progress computation (compile time) --------------------------------

static_assert(!ProgressPercent(0, 0).has_value());
static_assert(ProgressPercent(0, 10) == 0u);
static_assert(ProgressPercent(5, 10) == 50u);
static_assert(ProgressPercent(1, 3) == 33u);
static_assert(ProgressPercent(2, 3) == 66u);
static_assert(ProgressPercent(10, 10) == 100u);
static_assert(ProgressPercent(11, 10) == 100u);  // clamped
static_assert(ProgressPercent(UINT64_MAX - 1, UINT64_MAX) == 99u);
static_assert(ProgressPercent(UINT64_MAX / 2, UINT64_MAX) == 50u);
static_assert(ProgressPercent(UINT64_MAX, UINT64_MAX) == 100u);

static_assert(!BackupProgressSnapshot{}.Percent().has_value());
// bytes take precedence over files
static_assert(BackupProgressSnapshot{true, false, 10, 1000, 9, 250}.Percent()
              == 25u);
// no file data at all -> count entries
static_assert(BackupProgressSnapshot{true, true, 4, 0, 1, 0}.Percent() == 25u);
static_assert(
    !BackupProgressSnapshot{true, false, 0, 0, 0, 0}.Percent().has_value());

TEST(BackupProgress, CountsDiscoveredAndProcessed)
{
  BackupProgress progress;
  EXPECT_FALSE(progress.Snapshot().active);

  progress.Start();
  progress.Discovered(100);
  progress.Discovered(300);
  progress.Processed(100);

  auto snap = progress.Snapshot();
  EXPECT_TRUE(snap.active);
  EXPECT_FALSE(snap.walk_finished);
  EXPECT_EQ(snap.files_discovered, 2u);
  EXPECT_EQ(snap.bytes_discovered, 400u);
  EXPECT_EQ(snap.files_processed, 1u);
  EXPECT_EQ(snap.bytes_processed, 100u);
  EXPECT_EQ(snap.Percent(), 25u);

  progress.WalkFinished();
  EXPECT_TRUE(progress.Snapshot().walk_finished);

  progress.Start();  // a new job starts from scratch
  snap = progress.Snapshot();
  EXPECT_EQ(snap.files_discovered, 0u);
  EXPECT_FALSE(snap.walk_finished);

  progress.Disable();
  EXPECT_FALSE(progress.Snapshot().active);
}

TEST(BackupProgress, Format)
{
  EXPECT_EQ(FormatBackupProgress(BackupProgressSnapshot{}, false), "");
  EXPECT_EQ(FormatBackupProgress(BackupProgressSnapshot{}, true), "");

  BackupProgressSnapshot snap{true, false, 2000, 20000, 1000, 5000};
  EXPECT_EQ(FormatBackupProgress(snap, false),
            "    Progress=25% Files=1,000/2,000 Bytes=5,000/20,000"
            " (still scanning)\n");
  EXPECT_EQ(FormatBackupProgress(snap, true),
            " Progress=25\n FilesProcessed=1000\n FilesDiscovered=2000\n"
            " BytesProcessed=5000\n BytesDiscovered=20000\n WalkFinished=0\n");

  snap.walk_finished = true;
  EXPECT_EQ(FormatBackupProgress(snap, false),
            "    Progress=25% Files=1,000/2,000 Bytes=5,000/20,000\n");

  BackupProgressSnapshot empty{true, false, 0, 0, 0, 0};
  EXPECT_EQ(FormatBackupProgress(empty, false),
            "    Progress=n/a Files=0/0 Bytes=0/0 (still scanning)\n");
  EXPECT_EQ(FormatBackupProgress(empty, true),
            " FilesProcessed=0\n FilesDiscovered=0\n BytesProcessed=0\n"
            " BytesDiscovered=0\n WalkFinished=0\n");
}

// ---- job counters shared by walker and backup thread -----------------------

TEST(JobCounters, ConcurrentUpdatesAreNotLost)
{
  JobControlRecord jcr;
  constexpr int kPerThread = 100000;
  auto work = [&] {
    for (int i = 0; i < kPerThread; ++i) {
      jcr.IncrementJobErrors();
      jcr.IncrementJobWarnings();
      jcr.AddReadBytes(3);
    }
  };
  std::thread t1(work), t2(work);
  t1.join();
  t2.join();

  EXPECT_EQ(jcr.JobErrors, 2u * kPerThread);
  EXPECT_EQ(jcr.JobWarnings, 2u * kPerThread);
  EXPECT_EQ(jcr.ReadBytes, 2u * 3u * kPerThread);
}

TEST(JobCounters, IncrementReturnsPreviousValue)
{
  JobControlRecord jcr;
  EXPECT_EQ(jcr.IncrementJobErrors(), 0u);
  EXPECT_EQ(jcr.IncrementJobErrors(), 1u);
  EXPECT_EQ(jcr.JobErrors, 2u);
}

TEST(JobCounters, EnsureJobErrors)
{
  JobControlRecord jcr;
  jcr.EnsureJobErrors();
  EXPECT_EQ(jcr.JobErrors, 1u);
  jcr.EnsureJobErrors();
  EXPECT_EQ(jcr.JobErrors, 1u);  // not incremented again
  jcr.JobErrors = 5;
  jcr.EnsureJobErrors();
  EXPECT_EQ(jcr.JobErrors, 5u);
}

// ---- queue ---------------------------------------------------------------

TEST(BoundedWorkQueue, FifoAndCloseDrains)
{
  BoundedWorkQueue<int> q{100};
  EXPECT_TRUE(q.Push(1, 10));
  EXPECT_TRUE(q.Push(2, 10));
  EXPECT_TRUE(q.Push(3, 10));
  q.Close();
  EXPECT_FALSE(q.Push(4, 10));

  EXPECT_EQ(q.Pop(), 1);
  EXPECT_EQ(q.Pop(), 2);
  EXPECT_EQ(q.Pop(), 3);
  EXPECT_EQ(q.Pop(), std::nullopt);
}

TEST(BoundedWorkQueue, AbortDropsItems)
{
  BoundedWorkQueue<std::shared_ptr<int>> q{100};
  auto item = std::make_shared<int>(1);
  EXPECT_TRUE(q.Push(item, 1));
  EXPECT_EQ(item.use_count(), 2);
  q.Abort();
  EXPECT_EQ(item.use_count(), 1);
  EXPECT_EQ(q.Pop(), std::nullopt);
  EXPECT_FALSE(q.Push(item, 1));
}

TEST(BoundedWorkQueue, OversizedItemFitsIntoEmptyQueue)
{
  BoundedWorkQueue<int> q{10};
  EXPECT_TRUE(q.Push(1, 1000));
  EXPECT_EQ(q.Pop(), 1);
}

TEST(BoundedWorkQueue, PushBlocksWhileFull)
{
  BoundedWorkQueue<int> q{2};
  std::atomic<int> pushed{0};
  std::thread producer([&] {
    for (int i = 0; i < 10; ++i) {
      ASSERT_TRUE(q.Push(i, 1));
      ++pushed;
    }
    q.Close();
  });

  // the producer can never be more than the limit ahead
  std::vector<int> got;
  while (std::optional i = q.Pop()) {
    EXPECT_LE(q.size(), 2u);
    got.push_back(*i);
  }
  producer.join();

  EXPECT_EQ(pushed, 10);
  ASSERT_EQ(got.size(), 10u);
  for (int i = 0; i < 10; ++i) { EXPECT_EQ(got[i], i); }
}

TEST(BoundedWorkQueue, AbortWakesBlockedProducer)
{
  BoundedWorkQueue<int> q{1};
  ASSERT_TRUE(q.Push(0, 1));
  std::thread producer([&] { EXPECT_FALSE(q.Push(1, 1)); });
  q.Abort();
  producer.join();
}

// ---- walker / backup thread ----------------------------------------------

namespace {

struct SavedEntry {
  std::string fname;
  int type;
  int32_t link_fi;
  bool operator==(const SavedEntry&) const = default;
};

std::ostream& operator<<(std::ostream& os, const SavedEntry& e)
{
  return os << "{" << e.fname << ", type=" << e.type << ", LinkFI=" << e.link_fi
            << "}";
}

// state shared with the plain function pointer callbacks
struct Recorder {
  std::vector<SavedEntry> saved;
  std::vector<std::thread::id> threads;
  std::vector<BackupProgressSnapshot> progress;
  JobControlRecord* jcr{};
  int32_t next_file_index{0};
  int fail_after{-1};  // return 0 after this many saves
  bool cancel_on_fail{false};
};
Recorder rec;

int RecordSave(JobControlRecord* jcr, FindFilesPacket* ff, bool)
{
  if (rec.fail_after >= 0
      && static_cast<int>(rec.saved.size()) >= rec.fail_after) {
    if (rec.cancel_on_fail) { jcr->setJobStatus(JS_Canceled); }
    return 0;
  }
  if (ff->type == FT_DIRBEGIN) { return 1; }  // what SaveFile() does
  rec.saved.push_back({ff->fname, ff->type, ff->LinkFI});
  rec.threads.push_back(std::this_thread::get_id());
  rec.progress.push_back(jcr->fd_impl->progress.Snapshot());
  // what EncodeAndSendAttributes() does
  ff->FileIndex = ++rec.next_file_index;
  return 1;
}

int RecordPluginSave(JobControlRecord* jcr, FindFilesPacket* ff, bool)
{
  EXPECT_EQ(ff, jcr->fd_impl->ff);
  EXPECT_TRUE(ff->cmd_plugin);
  rec.saved.push_back({std::string{"plugin:"} + ff->top_fname, -1, 0});
  rec.threads.push_back(std::this_thread::get_id());
  return 1;
}

void WriteFile(const fs::path& p, std::size_t size)
{
  std::ofstream f(p, std::ios::binary);
  f << std::string(size, 'x');
}

class BackupWalkerTest : public ::testing::Test {
 protected:
  void SetUp() override
  {
    root = fs::temp_directory_path()
           / ("bareos_walker_test_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "sub" / "deeper");
    WriteFile(root / "a", 10);
    WriteFile(root / "b", 20);
    WriteFile(root / "empty", 0);
    WriteFile(root / "sub" / "c", 30);
    WriteFile(root / "sub" / "deeper" / "d", 40);
    fs::create_hard_link(root / "a", root / "sub" / "a_link");
    fs::create_hard_link(root / "a", root / "sub" / "deeper" / "a_link2");
    fs::create_symlink("a", root / "symlink");

    rec = Recorder{};
  }

  void TearDown() override { fs::remove_all(root); }

  // a fresh job with a fileset including `root` (and optional plugins)
  struct Job {
    JobControlRecord jcr;
    Job(const fs::path& root, std::vector<std::string> plugins = {})
    {
      jcr.fd_impl = new FiledJcrImpl;
      jcr.fd_impl->ff = init_find_files();
      jcr.setJobStatus(JS_Running);
      InitFileset(&jcr);
      auto* incexe = new_include(jcr.fd_impl->ff->fileset);
      AddFileToFileset(&jcr, root.c_str(), true, jcr.fd_impl->ff->fileset);
      for (auto& p : plugins) {
        incexe->plugin_list.append(new_dlistString(p.c_str()));
      }
    }
    ~Job()
    {
      CleanupFileset(&jcr);
      TermFindFiles(jcr.fd_impl->ff);
      delete jcr.fd_impl;
      jcr.fd_impl = nullptr;
    }
  };

  // run the classic single threaded FindFiles()
  std::vector<SavedEntry> RunSynchronous(std::vector<std::string> plugins = {})
  {
    Job job{root, plugins};
    rec = Recorder{};
    rec.jcr = &job.jcr;
    EXPECT_EQ(
        FindFiles(&job.jcr, job.jcr.fd_impl->ff, RecordSave, RecordPluginSave),
        1);
    return rec.saved;
  }

  fs::path root;
};

}  // namespace

TEST_F(BackupWalkerTest, SameResultAsFindFiles)
{
  const auto expected = RunSynchronous();

  for (std::size_t queue_bytes : {std::size_t{1}, kDefaultWalkerQueueBytes}) {
    SCOPED_TRACE("queue bytes " + std::to_string(queue_bytes));
    Job job{root};
    rec = Recorder{};
    EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                  RecordPluginSave, queue_bytes),
              1);
    EXPECT_EQ(rec.saved, expected);

    // all saves happen on the calling thread
    for (auto& id : rec.threads) { EXPECT_EQ(id, std::this_thread::get_id()); }

    // the walker is always ahead of the backup thread
    for (auto& p : rec.progress) {
      EXPECT_TRUE(p.active);
      EXPECT_LT(p.files_processed, p.files_discovered);
      EXPECT_LE(p.bytes_processed, p.bytes_discovered);
    }

    auto done = job.jcr.fd_impl->progress.Snapshot();
    EXPECT_TRUE(done.walk_finished);
    EXPECT_EQ(done.files_discovered, expected.size());
    EXPECT_EQ(done.files_processed, expected.size());
    // a, b, c, d; the hard links to a are only counted once
    EXPECT_EQ(done.bytes_discovered, 10u + 20u + 30u + 40u);
    EXPECT_EQ(done.bytes_processed, done.bytes_discovered);
    EXPECT_EQ(done.Percent(), 100u);
    EXPECT_EQ(job.jcr.fd_impl->backup_walker, nullptr);
  }
}

TEST_F(BackupWalkerTest, HardlinksAreResolvedByBackupThread)
{
  Job job{root};
  EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                nullptr, 1),
            1);

  int32_t first_fi = 0;
  int link_saved = 0;
  for (std::size_t i = 0; i < rec.saved.size(); ++i) {
    const auto& e = rec.saved[i];
    const auto name = fs::path(e.fname).filename();
    if (name != "a" && name != "a_link" && name != "a_link2") { continue; }
    if (e.type == FT_LNKSAVED) {
      ++link_saved;
      EXPECT_EQ(e.link_fi, first_fi) << e;
    } else {
      EXPECT_EQ(first_fi, 0) << "inode saved twice: " << e;
      first_fi = static_cast<int32_t>(i + 1);  // FileIndex given by RecordSave
    }
  }
  EXPECT_NE(first_fi, 0);
  EXPECT_EQ(link_saved, 2);
}

TEST_F(BackupWalkerTest, PluginCommandsRunOnCallingThreadInOrder)
{
  const std::vector<std::string> plugins{"first-plugin:", "second-plugin:"};
  const auto expected = RunSynchronous(plugins);
  ASSERT_GE(expected.size(), 2u);
  EXPECT_EQ(expected[expected.size() - 2].fname, "plugin:first-plugin:");
  EXPECT_EQ(expected.back().fname, "plugin:second-plugin:");

  Job job{root, plugins};
  rec = Recorder{};
  EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                RecordPluginSave, 1),
            1);
  EXPECT_EQ(rec.saved, expected);
  for (auto& id : rec.threads) { EXPECT_EQ(id, std::this_thread::get_id()); }
}

TEST_F(BackupWalkerTest, MissingPluginIsAnError)
{
  Job job{root, {"some-plugin:"}};
  EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                nullptr, 1),
            0);
}

TEST_F(BackupWalkerTest, TopLevelFailureIsReported)
{
  // the only top level entry is `root` itself, saved last (FT_DIREND)
  Job job{root};
  rec.fail_after = 3;
  EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                nullptr, 1),
            0);
  EXPECT_EQ(rec.saved.size(), 3u);
}

TEST_F(BackupWalkerTest, CancelStopsBothThreads)
{
  Job job{root};
  rec.fail_after = 1;
  rec.cancel_on_fail = true;
  // tiny queue, so the walker is blocked when the backup thread stops
  EXPECT_EQ(FindFilesWithWalker(&job.jcr, job.jcr.fd_impl->ff, RecordSave,
                                nullptr, 1),
            0);
  EXPECT_EQ(rec.saved.size(), 1u);
  EXPECT_TRUE(job.jcr.fd_impl->progress.Snapshot().walk_finished);
  EXPECT_EQ(job.jcr.fd_impl->backup_walker, nullptr);
}
