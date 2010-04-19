// ext3grep -- An ext3 file system investigation and undelete tool
//
//! @file restore.h Declarations for file restore.cc.
//
// Copyright (C) 2008, by
// 
// Carlo Wood, Run on IRC <carlo@alinoe.com>
// RSA-1024 0x624ACAD5 1997-01-26                    Sign & Encrypt
// Fingerprint16 = 32 EC A7 B6 AC DB 65 A6  F6 F6 55 DD 1C DC FF 61
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef RESTORE_H
#define RESTORE_H

#ifndef USE_PCH
#include <string>	// Needed for std::string
#endif

#include "inode.h"	// Needed for InodePointer

// Real constants.
std::string const outputdir = "RESTORED_FILES/";
int const latest = -1;

void restore_inode(int inodenr, InodePointer real_inode, std::string const& outfile, int seqnr = latest);

enum get_undeleted_inode_type {
  ui_no_inode,
  ui_real_inode,
  ui_journal_inode,
  ui_inode_too_old
};

get_undeleted_inode_type get_undeleted_inode(int inodenr, Inode& inode, int* sequence = NULL, int seqnr = latest);

#endif // RESTORE_H
