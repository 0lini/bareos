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
 * Split the backup pass into a walker thread, which traverses the fileset
 * and enqueues the entries it finds, and the backup thread (the job thread),
 * which consumes them and sends them to the storage daemon.
 */
#ifndef BAREOS_FILED_BACKUP_WALKER_H_
#define BAREOS_FILED_BACKUP_WALKER_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

class JobControlRecord;
struct FindFilesPacket;

namespace filedaemon {

/* FIFO for one producer and one consumer, bounded by the summed cost of the
 * queued items.  An item is always accepted into an empty queue, so a single
 * item larger than the limit cannot block forever. */
template <typename T> class BoundedWorkQueue {
 public:
  explicit BoundedWorkQueue(std::size_t max_cost) : max_cost_{max_cost} {}

  // Blocks while the queue is full.  Returns false if the queue was
  // closed or aborted; the item is dropped in that case.
  bool Push(T item, std::size_t cost)
  {
    std::unique_lock l{mutex_};
    not_full_.wait(l, [&] {
      return closed_ || aborted_ || items_.empty() || cost_ + cost <= max_cost_;
    });
    if (closed_ || aborted_) { return false; }
    items_.emplace_back(std::move(item), cost);
    cost_ += cost;
    not_empty_.notify_one();
    return true;
  }

  // Blocks until there is an item.  Returns nullopt once the queue is closed
  // and drained, or as soon as it is aborted.
  std::optional<T> Pop()
  {
    std::unique_lock l{mutex_};
    not_empty_.wait(l, [&] { return aborted_ || closed_ || !items_.empty(); });
    if (aborted_ || items_.empty()) { return std::nullopt; }
    auto [item, cost] = std::move(items_.front());
    items_.pop_front();
    cost_ -= cost;
    not_full_.notify_one();
    return std::move(item);
  }

  // Producer is done; the consumer still gets the queued items.
  void Close()
  {
    std::unique_lock l{mutex_};
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  // Drop all queued items and wake everybody up.
  void Abort()
  {
    std::deque<std::pair<T, std::size_t>> dropped;
    {
      std::unique_lock l{mutex_};
      aborted_ = true;
      dropped.swap(items_);
      cost_ = 0;
      not_empty_.notify_all();
      not_full_.notify_all();
    }
    // items are destroyed outside of the lock
  }

  std::size_t size() const
  {
    std::unique_lock l{mutex_};
    return items_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<std::pair<T, std::size_t>> items_;
  std::size_t cost_{0};
  const std::size_t max_cost_;
  bool closed_{false};
  bool aborted_{false};
};

using FileCallback = int(JobControlRecord*, FindFilesPacket*, bool);

// Upper bound for the memory used by queued entries (approximately).
inline constexpr std::size_t kDefaultWalkerQueueBytes = 64 * 1024 * 1024;

/* Drop-in replacement for FindFiles(jcr, ff, save_file, plugin_save) that
 * traverses the fileset in a separate walker thread.  save_file and
 * plugin_save are called from the calling thread only, in the same order
 * FindFiles() would call them.  Progress is reported in
 * jcr->fd_impl->progress. */
int FindFilesWithWalker(JobControlRecord* jcr,
                        FindFilesPacket* ff,
                        FileCallback* save_file,
                        FileCallback* plugin_save,
                        std::size_t max_queue_bytes = kDefaultWalkerQueueBytes);

}  // namespace filedaemon

#endif  // BAREOS_FILED_BACKUP_WALKER_H_
