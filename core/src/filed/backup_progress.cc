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
#include "filed/backup_progress.h"
#include "lib/edit.h"

namespace filedaemon {

std::string FormatBackupProgress(const BackupProgressSnapshot& progress,
                                 bool api)
{
  if (!progress.active) { return {}; }

  const std::optional percent = progress.Percent();
  char b1[32], b2[32], b3[32], b4[32];
  std::string out;

  if (api) {
    if (percent) { out += " Progress=" + std::to_string(*percent) + "\n"; }
    out += " FilesProcessed="
           + std::string{edit_uint64(progress.files_processed, b1)}
           + "\n FilesDiscovered="
           + std::string{edit_uint64(progress.files_discovered, b2)}
           + "\n BytesProcessed="
           + std::string{edit_uint64(progress.bytes_processed, b3)}
           + "\n BytesDiscovered="
           + std::string{edit_uint64(progress.bytes_discovered, b4)}
           + "\n WalkFinished=" + (progress.walk_finished ? "1" : "0") + "\n";
    return out;
  }

  out = "    Progress=";
  out += percent ? std::to_string(*percent) + "%" : std::string{"n/a"};
  out += std::string{" Files="}
         + edit_uint64_with_commas(progress.files_processed, b1) + "/"
         + edit_uint64_with_commas(progress.files_discovered, b2)
         + " Bytes=" + edit_uint64_with_commas(progress.bytes_processed, b3)
         + "/" + edit_uint64_with_commas(progress.bytes_discovered, b4);
  // until the walk is finished the totals are still growing
  if (!progress.walk_finished) { out += " (still scanning)"; }
  out += "\n";
  return out;
}

}  // namespace filedaemon
