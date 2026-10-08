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
 * Progress counters of a running backup.  The walker thread adds what it
 * discovers, the backup thread adds what it has finished.  Everything is
 * atomic so the status command can read it at any time.
 */
#ifndef BAREOS_FILED_BACKUP_PROGRESS_H_
#define BAREOS_FILED_BACKUP_PROGRESS_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace filedaemon {

// Integer percentage of done/total, clamped to [0, 100].
// Returns nullopt if nothing was discovered yet.
constexpr std::optional<std::uint32_t> ProgressPercent(std::uint64_t done,
                                                       std::uint64_t total)
{
  if (total == 0) { return std::nullopt; }
  if (done >= total) { return 100; }
  if (done <= UINT64_MAX / 100) {
    return static_cast<std::uint32_t>(done * 100 / total);
  }
  // done * 100 would overflow; here total > done > UINT64_MAX / 100
  const std::uint64_t percent = done / (total / 100);
  return static_cast<std::uint32_t>(percent < 99 ? percent : 99);
}

struct BackupProgressSnapshot {
  bool active{false};         // a walker based backup is (or was) running
  bool walk_finished{false};  // totals are final
  std::uint64_t files_discovered{0};
  std::uint64_t bytes_discovered{0};
  std::uint64_t files_processed{0};
  std::uint64_t bytes_processed{0};

  /* Overall progress.  Bytes dominate the runtime of a backup, so they are
   * used whenever there is file data; otherwise we fall back to the number
   * of entries. */
  constexpr std::optional<std::uint32_t> Percent() const
  {
    if (!active) { return std::nullopt; }
    if (bytes_discovered > 0) {
      return ProgressPercent(bytes_processed, bytes_discovered);
    }
    return ProgressPercent(files_processed, files_discovered);
  }
};

/* Status lines describing the progress, empty if there is nothing to report.
 * Plain: "    Progress=42% Files=1,000/2,000 Bytes=10,000/20,000\n"
 * Api:   " Progress=42\n FilesProcessed=1000\n ..." (one key per line) */
std::string FormatBackupProgress(const BackupProgressSnapshot& progress,
                                 bool api);

class BackupProgress {
 public:
  void Start()
  {
    files_discovered_ = 0;
    bytes_discovered_ = 0;
    files_processed_ = 0;
    bytes_processed_ = 0;
    walk_finished_ = false;
    active_ = true;
  }
  void Discovered(std::uint64_t bytes)
  {
    files_discovered_.fetch_add(1, std::memory_order_relaxed);
    bytes_discovered_.fetch_add(bytes, std::memory_order_relaxed);
  }
  void Processed(std::uint64_t bytes)
  {
    files_processed_.fetch_add(1, std::memory_order_relaxed);
    bytes_processed_.fetch_add(bytes, std::memory_order_relaxed);
  }
  void WalkFinished() { walk_finished_ = true; }
  void Disable() { active_ = false; }

  BackupProgressSnapshot Snapshot() const
  {
    return {active_.load(),
            walk_finished_.load(),
            files_discovered_.load(std::memory_order_relaxed),
            bytes_discovered_.load(std::memory_order_relaxed),
            files_processed_.load(std::memory_order_relaxed),
            bytes_processed_.load(std::memory_order_relaxed)};
  }

 private:
  std::atomic<bool> active_{false};
  std::atomic<bool> walk_finished_{false};
  std::atomic<std::uint64_t> files_discovered_{0};
  std::atomic<std::uint64_t> bytes_discovered_{0};
  std::atomic<std::uint64_t> files_processed_{0};
  std::atomic<std::uint64_t> bytes_processed_{0};
};

}  // namespace filedaemon

#endif  // BAREOS_FILED_BACKUP_PROGRESS_H_
