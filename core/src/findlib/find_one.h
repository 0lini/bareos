/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2026 Bareos GmbH & Co. KG

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
#ifndef BAREOS_FINDLIB_FIND_ONE_H_
#define BAREOS_FINDLIB_FIND_ONE_H_

int FindOneFile(JobControlRecord* jcr,
                FindFilesPacket* ff,
                int HandleFile(JobControlRecord* jcr,
                               FindFilesPacket* ff_pkt,
                               bool top_level),
                char* p,
                dev_t parent_device,
                bool top_level);
void TermFindOne(FindFilesPacket* ff);
bool HasFileChanged(JobControlRecord* jcr, FindFilesPacket* ff_pkt);
bool CheckChanges(JobControlRecord* jcr, FindFilesPacket* ff_pkt);

enum class HardlinkState
{
  kFirst,        // first time we save this inode; ff_pkt->linked is set
  kSameName,     // this exact name was already saved, nothing to do
  kAlreadySaved  // ff_pkt was turned into a FT_LNKSAVED entry
};

/* Look up the inode of ff_pkt->statp in ff_pkt->linkhash (creating the hash
 * if needed) and update ff_pkt according to the returned state. */
HardlinkState ResolveHardlink(FindFilesPacket* ff_pkt, const char* fname);
void RestoreFileTimes(const FindFilesPacket* ff_pkt, const char* fname);

#endif  // BAREOS_FINDLIB_FIND_ONE_H_
