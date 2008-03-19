// ext3grep -- An ext3 file system investigation and undelete tool
//
//! @file ext3grep.cc Main implementation.
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

#ifndef USE_PCH
#include "sys.h"
#include <sys/types.h>
#include <asm/byteorder.h>
#include "ext3.h"
#include <utime.h>
#include <sys/mman.h>
#include <cassert>
#include <ctime>
#include <cstdlib>
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <set>
#include <map>
#include <vector>
#include <list>
#include <limits>
#include <sstream>
#include "debug.h"
#endif

#include "locate.h"

// Super block accessors.
int inode_count(ext3_super_block const& super_block) { return super_block.s_inodes_count; }
int block_count(ext3_super_block const& super_block) { return super_block.s_blocks_count; }
int reserved_block_count(ext3_super_block const& super_block) { return super_block.s_r_blocks_count; }
int first_data_block(ext3_super_block const& super_block) { return super_block.s_first_data_block; }
int block_size(ext3_super_block const& super_block) { return EXT3_BLOCK_SIZE(&super_block); }
int fragment_size(ext3_super_block const& super_block) { return EXT3_FRAG_SIZE(&super_block); }
int blocks_per_group(ext3_super_block const& super_block) { return EXT3_BLOCKS_PER_GROUP(&super_block); }
int inodes_per_group(ext3_super_block const& super_block) { return EXT3_INODES_PER_GROUP(&super_block); }
int first_inode(ext3_super_block const& super_block) { return EXT3_FIRST_INO(&super_block); }
int inode_size(ext3_super_block const& super_block) { return EXT3_INODE_SIZE(&super_block); }
int inode_blocks_per_group(ext3_super_block const& super_block) { return inodes_per_group(super_block) * inode_size(super_block) / block_size(super_block); }
int groups(ext3_super_block const& super_block) { return inode_count(super_block) / inodes_per_group(super_block); }

// Convert Big Endian to Little Endian.
static inline __le32 be2le(__be32 v) { return __be32_to_cpu(v); }
static inline __le16 be2le(__be16 v) { return __be16_to_cpu(v); }
static inline __u8 be2le(__u8 v) { return v; }

// Using the headers from e2fsprogs, the big endian journal structs
// use normal types. However, since WE read raw data into them,
// they are really still big endian. Calling be2le on those
// types therefore still needs to do the conversion.
static inline __le32 be2le(__s32 const& v) { return be2le(*reinterpret_cast<__be32 const*>(&v)); }

int block_count(journal_superblock_t const& journal_super_block) { return be2le(journal_super_block.s_maxlen); }

struct bitmap_ptr {
  int index;
  union {
    uint64_t mask;
    unsigned char byte[8];
  };
};

bitmap_ptr get_bitmap_mask(unsigned int bit)
{
  bitmap_ptr result;
  result.mask = 0;	// Initialize all bits in the mask to zero.

  // From the book "File System Forensic Analysis":
  // Like other bitmaps we have seen in this book, it is organized into bytes,
  // and the least-significant bit corresponds to the block after the most-significant
  // bit of the previous byte. In other words, when we read the bytes we go left to
  // right, but inside each byte we read right to left.

  // Higher bit's result in higher indexes. Every 64 bit the index is incremented by one.
  result.index = bit >> 6;
  // Higher bits means higher bytes. Every 8 bit the byte index is incremented by one.
  // Higher bits means more significant bits. There are 2^3 bits per byte.
  result.byte[(bit & 63) >> 3] = 1 << (bit & 7);
  return result;
}

// Convert byte-offset to block.
// Returns the block number that contains the byte at offset bytes from the start of the device file.
int offset_to_block(ext3_super_block const& super_block, size_t offset)
{
  return offset / block_size(super_block);
}

// Convert block number to group.
// Returns the group number of block.
int block_to_group(ext3_super_block const& super_block, int block)
{
  return (block - first_data_block(super_block)) / blocks_per_group(super_block);
}

// Convert group to block number.
// Returns the block number of the first block of a group.
int group_to_block(ext3_super_block const& super_block, int group)
{
  return first_data_block(super_block) + group * blocks_per_group(super_block);
}

// Convert inode number to group.
// Returns the group number of inode.
int inode_to_group(ext3_super_block const& super_block, int inode_number)
{
  return (inode_number - 1) / inodes_per_group(super_block);
}

// Print superblock contents.
std::ostream& operator<<(std::ostream& os, ext3_super_block const& super_block);
std::ostream& operator<<(std::ostream& os, journal_superblock_t const& journal_super_block);

// Print group descriptor.
std::ostream& operator<<(std::ostream& os, ext3_group_desc const& group_desc);

// Print journal header;
std::ostream& operator<<(std::ostream& os, journal_header_t const& journal_header);
std::ostream& operator<<(std::ostream& os, journal_block_tag_t const& journal_block_tag);
std::ostream& operator<<(std::ostream& os, journal_revoke_header_t const& journal_revoke_header);

// The (first) super block starts here.
int const SUPER_BLOCK_OFFSET = 1024;

enum hist_type {
  hist_none = 0,
  hist_atime,
  hist_ctime,
  hist_mtime,
  hist_dtime,
  hist_group
};

enum is_directory_type {
  isdir_no = 0,
  isdir_start,
  isdir_extended
};

// Commandline options.
const char *progname;
bool commandline_superblock = false;
int commandline_group = -1;
int commandline_inode_to_block = -1;
int commandline_inode = -1;
int commandline_block = -1;
int commandline_journal_block = -1;
int commandline_journal_transaction = -1;
bool commandline_print = false;
bool commandline_ls = false;
bool commandline_journal = false;
bool commandline_dump_names = false;
int commandline_depth = 0;
bool commandline_deleted = false;
bool commandline_directory = false;
time_t commandline_before = 0;
time_t commandline_after = 0;
bool commandline_allocated = false;
bool commandline_unallocated = false;
bool commandline_reallocated = false;
bool commandline_action = false;
bool commandline_search_zeroed_inodes = false;
bool commandline_zeroed_inodes = false;
bool commandline_show_path_inodes = false;
std::string commandline_search;
std::string commandline_search_start;
int commandline_search_inode = -1;
hist_type commandline_histogram = hist_none;
std::string commandline_inode_dirblock_table;
int commandline_show_journal_inodes = -1;
std::string commandline_restore_file;
bool commandline_restore_all = false;
bool commandline_show_hardlinks = false;

struct Parent;

static void decode_commandline_options(int& argc, char**& argv);
static void dump_hex(unsigned char const* buf, size_t size);
static void print_block(unsigned char* block);
static void print_inode(Inode& inode);
static void iterate_over_directory(unsigned char* block, int blocknr,
    bool (*action)(ext3_dir_entry_2 const&, Inode&, bool, bool, bool, bool, bool, bool, Parent*, void*), Parent* parent, void* data);
static void iterate_over_directory_action(int blocknr, void* data);
static void iterate_over_existing_directory_action(int blocknr, void* data);
static void iterate_over_journal(
    bool (*action_tag)(uint32_t block, uint32_t sequence, journal_block_tag_t*, void* data),
    bool (*action_revoke)(uint32_t block, uint32_t sequence, journal_revoke_header_t*, void* data),
    bool (*action_commit)(uint32_t block, uint32_t sequence, void* data), void* data);
static void print_directory(unsigned char* block, int blocknr);
static void print_restrictions(void);
static bool is_directory(Inode& inode);
static is_directory_type is_directory(unsigned char* block, int blocknr, bool start_block = true, bool certainly_linked = true, int offset = 0);
static bool is_journal(int blocknr);
static bool is_in_journal(int blocknr);
static int is_inode_block(int blocknr);
static bool is_indirect_block_in_journal(int blocknr);
static bool is_symlink(Inode& inode);
static void hist_init(size_t min, size_t max);
static void hist_add(size_t val);
static void hist_print(void);
static int dir_inode_to_block(uint32_t inode);
static int journal_block_to_real_block(int blocknr);
static void init_journal(void);
static int journal_block_contains_inodes(int blocknr);
static void handle_commandline_journal_transaction(void);
static int print_symlink(std::ostream& os, Inode& inode);
static void print_block_descriptors(uint32_t block);
static void print_directory_inode(int inode);
static void dump_names(void);
static void init_files(void);
static void show_journal_inodes(int inode);
static void restore_file(std::string const& outfile);
static void show_hardlinks(void);

// Frequently used constant values from the superblock.
ext3_super_block super_block;
int groups_;
int block_size_;
int block_size_log_;
int inodes_per_group_;
int inode_size_;
uint32_t inode_count_;
uint32_t block_count_;

// The journal super block.
journal_superblock_t journal_super_block;
Inode journal_inode;
int journal_block_size_;
int journal_maxlen_;
int journal_first_;
int journal_sequence_;
int journal_start_;

// Convert block to byte-offset.
// Returns the offset (dd --seek) in the device file to the first byte of the block.
static inline off_t block_to_offset(int block)
{
  off_t offset = block;
  offset <<= block_size_log_;
  return offset;
}

// Globally used variables.
std::ifstream device;
uint64_t** inode_bitmap;
uint64_t** block_bitmap;
Inode** all_inodes;
#if USE_MMAP
void** all_mmaps;
#endif
ext3_group_desc* group_descriptor_table;
char* inodes_buf;
std::set<std::string> accepted_filenames;
int no_filtering = 0;
std::string device_name;
uint32_t wrapped_journal_sequence = 0;
#if USE_MMAP
long page_size_;
int device_fd;
#endif
static std::string const outputdir = "RESTORED_FILES/";

//-----------------------------------------------------------------------------
//
// Initialization
//

void init_consts()
{
  // Frequently used constants.
  groups_ = groups(super_block);
  block_size_ = block_size(super_block);
  block_size_log_ = EXT3_BLOCK_SIZE_BITS(&super_block);
  inodes_per_group_ = inodes_per_group(super_block);
  inode_size_ = inode_size(super_block);
  inode_count_ = inode_count(super_block);
  block_count_ = block_count(super_block);
#if USE_MMAP
  page_size_ = sysconf(_SC_PAGESIZE);
#endif

  // Global arrays.
  all_inodes = new Inode* [groups_];
#if USE_MMAP
  all_mmaps = new void* [groups_];
#endif
  block_bitmap = new uint64_t* [groups_];
  // We use this array to know of which groups we loaded the metadata. Therefore zero it out.
  std::memset(block_bitmap, 0, sizeof(uint64_t*) * groups_);
  inode_bitmap = new uint64_t* [groups_];
  assert((size_t)inode_size_ <= sizeof(Inode));
  assert((size_t)inode_size_ == sizeof(Inode));			// This fails if kernel headers are used.
  inodes_buf = new char[inodes_per_group_ * inode_size_];

  // Initialize group_descriptor_table.

  // Calculate the block where the group descriptor table starts.
  int const super_block_block = SUPER_BLOCK_OFFSET / block_size(super_block);
  // The block following the superblock is the group descriptor table.
  int const group_descriptor_table_block = super_block_block + 1;

  // Allocate group descriptor table.
  assert(EXT3_DESC_PER_BLOCK(&super_block) * sizeof(ext3_group_desc) == (size_t)block_size_);
  group_descriptor_table = new ext3_group_desc[groups_];

  device.seekg(block_to_offset(group_descriptor_table_block));
  assert(device.good());
  device.read(reinterpret_cast<char*>(group_descriptor_table), sizeof(ext3_group_desc) * groups_);
  assert(device.good());
}

void load_meta_data(int group);

inline Inode& get_inode(int inode)
{
  int group = (inode - 1) / inodes_per_group_;
  unsigned int bit = inode - 1 - group * inodes_per_group_;
  assert(bit < 8U * block_size_);
  if (block_bitmap[group] == NULL)
    load_meta_data(group);
  return all_inodes[group][bit];
}

void init_journal_consts(void)
{
  // Initialize journal constants.
  journal_block_size_ = be2le(journal_super_block.s_blocksize);
  assert(journal_block_size_ == block_size_);	// Sorry, I'm trying to recover my own data-- have no time to deal with this.
  journal_maxlen_ = be2le(journal_super_block.s_maxlen);
  journal_first_ = be2le(journal_super_block.s_first);
  journal_sequence_ = be2le(journal_super_block.s_sequence);
  journal_start_ = be2le(journal_super_block.s_start);
  journal_inode = get_inode(super_block.s_journal_inum);
}

unsigned char* get_block(int block, unsigned char* block_buf)
{
  device.seekg(block_to_offset(block));
  device.read((char*)block_buf, block_size_);
  assert(device.good());
  return block_buf;
}

//-----------------------------------------------------------------------------
//
// Block type detection: is_*
//

enum filename_char_type {
  fnct_ok,
  fnct_illegal,
  fnct_unlikely
};

inline filename_char_type is_filename_char(unsigned char c)
{
  if (c < 32 || c > 126 || c == '/')
    return fnct_illegal;
#if 0
  // These characters are legal... but very unlikely.
  static unsigned char hit[128 - 32] = {			// Mark 22, 2a, 3b, 3c, 3e, 3f, 5c, 60, 7c
//  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, // 2
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, // 3
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 4
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, // 5
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 6
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0  // 7
  };
  // These characters are legal, but very unlikely.
  // Don't reject them when a specific block was requested.
  if (commandline_block == -1 && hit[c - 32])
    return fnct_unlikely;
#endif
  return fnct_ok;
}

static bool is_inode(int block)
{
  int group = block_to_group(super_block, block);
  if (block_bitmap[group] == NULL)
    load_meta_data(group);
  int inode_table = group_descriptor_table[group].bg_inode_table;
  return block >= inode_table &&										    // The first block of the inode table.
         (size_t)block_size_ * (block + 1) <= (size_t)block_size_ * inode_table + inodes_per_group_ * inode_size_;  // The first byte after the block/inode table.
}

// Only valid when is_inode returns true.
// Returns the number of the first inode in the block.
int block_to_inode(int block)
{
  int group = block_to_group(super_block, block);
  assert(block_bitmap[group]);
  int inode_table = group_descriptor_table[group].bg_inode_table;
  assert(block >= inode_table && (size_t)block_size_ * (block + 1) <= (size_t)block_size_ * inode_table + inodes_per_group_ * inode_size_);
  return 1 + group * inodes_per_group_ + (size_t)block_size_ * (block - inode_table) / inode_size_;
}

// Convert inode number to block number.
// Return the block number that contains the inode.
int inode_to_block(ext3_super_block const& super_block, int inode)
{
  int group = inode_to_group(super_block, inode);
  return group_descriptor_table[group].bg_inode_table + (size_t)(inode - 1 - group * inodes_per_group_) * inode_size_ / block_size_;
}

// Return true if this inode is a directory.
static inline bool is_directory(Inode& inode)
{
  return (inode.mode() & 0xf000) == 0x4000;
}

// Return true if this inode is a symlink.
static inline bool is_symlink(Inode& inode)
{
  return (inode.mode() & 0xf000) == 0xa000;
}

// Return true if this inode is a regular file.
static inline bool is_regular_file(Inode& inode)
{
  return (inode.mode() & 0xf000) == 0x8000;
}

struct DelayedWarning {
  std::ostringstream* delayed_warning;

  DelayedWarning() : delayed_warning(NULL) { }
  ~DelayedWarning() { if (delayed_warning) delete delayed_warning; }

  void init(void) { if (!delayed_warning) delayed_warning = new std::ostringstream; }
  operator bool(void) const { return delayed_warning; }
  std::string str(void) const { assert(delayed_warning); return delayed_warning->str(); }
  std::ostream& stream(void) { init(); return *delayed_warning; }
};

// Return true if this block looks like it contains a directory.
is_directory_type is_directory(unsigned char* block, int blocknr, bool start_block, bool certainly_linked, int offset)
{
  assert(!start_block || offset == 0);
  // Must be aligned to 4 bytes.
  if ((offset & EXT3_DIR_ROUND))
    return isdir_no;
  // A minimal ext3_dir_entry_2 must fit.
  if (offset + EXT3_DIR_REC_LEN(1) > block_size_)
    return isdir_no;
  ext3_dir_entry_2* dir_entry = reinterpret_cast<ext3_dir_entry_2*>(block + offset);
  // The first block has the "." and ".." directories at the start.
  bool is_start = false;
  if (offset == 0)
  {
    ext3_dir_entry_2* parent_dir_entry = reinterpret_cast<ext3_dir_entry_2*>(block + EXT3_DIR_REC_LEN(1));
    is_start = (dir_entry->name_len == 1 &&
                dir_entry->name[0] == '.' &&
		dir_entry->rec_len == EXT3_DIR_REC_LEN(1) &&
		dir_entry->file_type == EXT3_FT_DIR &&
		parent_dir_entry->name_len == 2 &&
		parent_dir_entry->name[0] == '.' &&
		parent_dir_entry->name[1] == '.' &&
		parent_dir_entry->file_type == EXT3_FT_DIR);
  }      
  if (start_block)
  {
    // If a start block is requested, return isdir_no when it is NOT isdir_start,
    // even though it might still really be isdir_extended, in order to speed
    // up the test.
    if (!is_start)
      return isdir_no;
  }
  // The inode is not overwritten when a directory is deleted (except
  // for the first inode of an extended directory block).
  // So even for deleted directories we can check the inode range.
  DelayedWarning delayed_warning;
  if (dir_entry->inode == 0 && dir_entry->name_len > 0)
  {
    // If the inode is zero and the filename makes no sense, reject the directory.
    for (int c = 0; c < dir_entry->name_len; ++c)
      if (is_filename_char(dir_entry->name[c]) == fnct_illegal)
	return isdir_no;
    // If the inode is zero, but the filename makes sense, print a warning
    // only when the inode really wasn't expected to be zero. Do not reject
    // the directory though.
    if (certainly_linked && (offset != 0 || start_block))
      delayed_warning.stream() << "WARNING: zero inode (name: \"" << std::string(dir_entry->name, dir_entry->name_len) <<
	  "\"; block: " << blocknr << "; offset 0x" << std::hex << offset << std::dec << ")\n";
  }
  if (dir_entry->inode > inode_count_)
    return isdir_no;	// Inode out of range.
  // File names are at least 1 character long.
  if (dir_entry->name_len == 0)
    return isdir_no;
  // The record length must make sense.
  if ((dir_entry->rec_len & EXT3_DIR_ROUND) ||
      dir_entry->rec_len < EXT3_DIR_REC_LEN(dir_entry->name_len) ||
      offset + dir_entry->rec_len > block_size_)
    return isdir_no;
  // Add some extra paranoia in the case that the whole block appears to exist of a single direntry (for an extended block).
  if (dir_entry->rec_len == block_size_ &&
      (dir_entry->file_type == EXT3_FT_UNKNOWN ||
       dir_entry->file_type >= EXT3_FT_MAX ||
       dir_entry->name_len == 1 ||
       (dir_entry->name[0] == '_' && dir_entry->name[1] == 'Z')))	// Symbol table entry?
    return isdir_no;
  // The record length must point to the end of the block or chain to it.
  offset += dir_entry->rec_len;
  if (offset != block_size_ && is_directory(block, blocknr, false, certainly_linked, offset) == isdir_no)
    return isdir_no;
  // The file name may only exist of certain characters.
  bool illegal = false;
  bool ok = true;
  for (int c = 0; c < dir_entry->name_len; ++c)
    if (is_filename_char(dir_entry->name[c]) != fnct_ok)
    {
      // Google Earth contains a few files that end on '&nbsp;'. Accept ';' in that case.
      if (dir_entry->name_len - c == 1 && dir_entry->name_len > 6 && strncmp(&dir_entry->name[c - 5], "&nbsp;", 6) == 0)
        continue;
      ok = false;
      if (is_filename_char(dir_entry->name[c]) == fnct_illegal)
      {
        illegal = true;
	break;
      }
    }
  if (ok && delayed_warning)
    std::cerr << delayed_warning.str();
  if (!ok && !illegal && accepted_filenames.find(std::string(dir_entry->name, dir_entry->name_len)) == accepted_filenames.end())
  {
    std::cerr << "\nWARNING: Rejecting possible directory (block #" << blocknr << ") because an entry contains legal but unlikely characters: '";
    std::cerr.write(dir_entry->name, dir_entry->name_len);
    std::cerr << "'.\n";
    std::cerr << "If this looks like a filename to you, you must add --accept='";
    std::cerr.write(dir_entry->name, dir_entry->name_len);
    std::cerr << "' as commandline parameter!\n";
  }
  return ok ? (is_start ? isdir_start : isdir_extended) : isdir_no;
}

// Returns true if the block is inside an inode table,
// or part of the journal, containing inodes.
int is_inode_block(int block)
{
  if (is_inode(block))
    return block;
  if (!is_journal(block) || is_indirect_block_in_journal(block))
    return 0;
  return journal_block_contains_inodes(block);
}

bool is_allocated(int inode)
{
  int group = (inode - 1) / inodes_per_group_;
  if (!block_bitmap[group])
    load_meta_data(group);
  unsigned int bit = inode - 1 - group * inodes_per_group_;
  assert(bit < 8U * block_size_);
  struct bitmap_ptr bmp = get_bitmap_mask(bit);
  return (inode_bitmap[group][bmp.index] & bmp.mask);
}

bool is_block_number(uint32_t block_number)
{
  return block_number < block_count_;
}

//-----------------------------------------------------------------------------
//
// Indirect blocks
//

static bool found_block;
static int block_looking_for;
void find_block_action(int blocknr, void*)
{
  if (blocknr == block_looking_for)
    found_block = true;
}

void print_directory_action(int blocknr, void*)
{
  static bool using_static_buffer = false;
  assert(!using_static_buffer);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  unsigned char* block = get_block(blocknr, block_buf);
  using_static_buffer = true;
  ext3_dir_entry_2* dir_entry = reinterpret_cast<ext3_dir_entry_2*>(block);
  if (dir_entry->rec_len < block_size_)	// The directory could be entirely empty (unused).
    print_directory(block, blocknr);
  using_static_buffer = false;
}

unsigned int const direct_bit = 1;
unsigned int const indirect_bit = 2;

bool iterate_over_all_blocks_of_indirect_block(int block, void (*action)(int, void*), void* data, unsigned int)
{
  static bool using_static_buffer = false;
  assert(!using_static_buffer);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  __le32* block_ptr = (__le32*)get_block(block, block_buf);
  using_static_buffer = true;
  unsigned int i = 0;
  while (i < block_size_ / sizeof(__le32))
  {
    if (block_ptr[i])
    {
      if (!is_block_number(block_ptr[i]))
        break;
      action(block_ptr[i], data);
    }
    ++i;
  }
  using_static_buffer = false;
  return i < block_size_ / sizeof(__le32);
}

bool iterate_over_all_blocks_of_double_indirect_block(int block, void (*action)(int, void*), void* data, unsigned int indirect_mask)
{
  static bool using_static_buffer = false;
  assert(!using_static_buffer);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  __le32* block_ptr = (__le32*)get_block(block, block_buf);
  using_static_buffer = true;
  unsigned int i = 0;
  while (i < block_size_ / sizeof(__le32))
  {
    if (block_ptr[i])
    {
      if (!is_block_number(block_ptr[i]))
        break;
      if ((indirect_mask & indirect_bit))
        action(block_ptr[i], data);
      if ((indirect_mask & direct_bit))
        if (iterate_over_all_blocks_of_indirect_block(block_ptr[i], action, data, indirect_mask))
	  break;
    }
    ++i;
  }
  using_static_buffer = false;
  return i < block_size_ / sizeof(__le32);
}

bool iterate_over_all_blocks_of_tripple_indirect_block(int block, void (*action)(int, void*), void* data, unsigned int indirect_mask)
{
  static bool using_static_buffer = false;
  assert(!using_static_buffer);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  __le32* block_ptr = (__le32*)get_block(block, block_buf);
  using_static_buffer = true;
  unsigned int i = 0;
  while (i < block_size_ / sizeof(__le32))
  {
    if (block_ptr[i])
    {
      if (!is_block_number(block_ptr[i]))
        break;
      if ((indirect_mask & indirect_bit))
        action(block_ptr[i], data);
      if (iterate_over_all_blocks_of_double_indirect_block(block_ptr[i], action, data, indirect_mask))
        break;
    }
    ++i;
  }
  using_static_buffer = false;
  return i < block_size_ / sizeof(__le32);
}

// Returns true if an indirect block was encountered that doesn't look like an indirect block anymore.
bool iterate_over_all_blocks_of(Inode& inode, void (*action)(int, void*), void* data = NULL, unsigned int indirect_mask = direct_bit)
{
  if (is_symlink(inode) && inode.blocks() == 0)
    return false;		// Block pointers contain text.
  __le32 const* block_ptr = inode.block();
  if ((indirect_mask & direct_bit))
    for (int i = 0; i < EXT3_NDIR_BLOCKS; ++i)
      if (block_ptr[i])
	action(block_ptr[i], data);
  if (block_ptr[EXT3_IND_BLOCK])
  {
    if ((indirect_mask & indirect_bit))
      action(block_ptr[EXT3_IND_BLOCK], data);
    if ((indirect_mask & direct_bit))
      if (iterate_over_all_blocks_of_indirect_block(block_ptr[EXT3_IND_BLOCK], action, data, indirect_mask))
        return true;
  }
  if (block_ptr[EXT3_DIND_BLOCK])
  {
    if ((indirect_mask & indirect_bit))
      action(block_ptr[EXT3_DIND_BLOCK], data);
    if (iterate_over_all_blocks_of_double_indirect_block(block_ptr[EXT3_DIND_BLOCK], action, data, indirect_mask))
      return true;
  }
  if (block_ptr[EXT3_TIND_BLOCK])
  {
    if ((indirect_mask & indirect_bit))
      action(block_ptr[EXT3_TIND_BLOCK], data);
    if (iterate_over_all_blocks_of_tripple_indirect_block(block_ptr[EXT3_TIND_BLOCK], action, data, indirect_mask))
      return true;
  }
  return false;
}

//-----------------------------------------------------------------------------
//
// load_meta_data
//

void load_inodes(int group)
{
  if (!block_bitmap[group])
    load_meta_data(group);
  // The start block of the inode table.
  int block_number = group_descriptor_table[group].bg_inode_table;
#if USE_MMAP
  int const blocks_per_page = page_size_ / block_size_;
  off_t page = block_number / blocks_per_page;
  off_t page_aligned_offset = page * page_size_;
  off_t offset = block_to_offset(block_number);
  // Use mmap to avoid running out of memory.
  all_mmaps[group] = mmap(NULL, inodes_per_group_ * inode_size_ + (offset - page_aligned_offset),
          PROT_READ, MAP_PRIVATE | MAP_NORESERVE, device_fd, page_aligned_offset);
  all_inodes[group] = reinterpret_cast<Inode*>((char*)all_mmaps[group] + (offset - page_aligned_offset));
#else
  // Load all inodes into memory.
  all_inodes[group] = new Inode[inodes_per_group_];	// sizeof(Inode) == inode_size_
  device.seekg(block_to_offset(block_number));
  device.read(reinterpret_cast<char*>(all_inodes[group]), inodes_per_group_ * inode_size_);
  assert(device.good());
#ifdef DEBUG
  // We set this, so that we can find back where an inode struct came from
  // during debugging of this program in gdb. It is not used anywhere.
  for (int ino = 0; ino < inodes_per_group_; ++ino)
    all_inodes[group][ino].set_reserved2(ino + 1 + group * inodes_per_group_);
#endif
#endif
}

void load_meta_data(int group)
{
  if (block_bitmap[group])	// Already loaded?
    return;
  // Load block bitmap.
  block_bitmap[group] = new uint64_t[block_size_ / sizeof(uint64_t)];
  device.seekg(block_to_offset(group_descriptor_table[group].bg_block_bitmap));
  device.read(reinterpret_cast<char*>(block_bitmap[group]), block_size_);
  // Load inode bitmap.
  inode_bitmap[group] = new uint64_t[block_size_ / sizeof(uint64_t)];
  device.seekg(block_to_offset(group_descriptor_table[group].bg_inode_bitmap));
  device.read(reinterpret_cast<char*>(inode_bitmap[group]), block_size_);
  // Load all inodes into memory.
  load_inodes(group);
}

//-----------------------------------------------------------------------------
//
// main
//

int main(int argc, char* argv[])
{
  Debug(debug::init());

  decode_commandline_options(argc, argv);

  // The size of a super block is 1024 bytes.
  assert(sizeof(ext3_super_block) == 1024);

  // Open the device.
  assert(argc == 1);
  device_name = *argv;
  device.open(*argv);
  assert(device.good());
#if USE_MMAP
  device_fd = open(*argv, O_RDONLY);
  assert(device_fd != -1);
#endif

  // Read the first superblock.
  device.seekg(SUPER_BLOCK_OFFSET);
  assert(device.good());
  device.read(reinterpret_cast<char*>(&super_block), sizeof(ext3_super_block));
  assert(device.good());
  init_consts();

  if (commandline_superblock && !commandline_journal)
  {
    // Print contents of superblock.
    std::cout << super_block << '\n';
  }

  // Sanity checks.
  assert(super_block.s_magic == 0xEF53);	// EXT3.
  assert(super_block.s_creator_os == 0);	// Linux.
  assert(super_block.s_block_group_nr == 0);	// First super block.
  assert((uint32_t)groups_ * inodes_per_group(super_block) == inode_count_);	// All inodes belong to a group.
  // extX does not support block fragments.
  // "File System Forensic Analysis, chapter 14, Overview --> Blocks"
  assert(block_size_ == fragment_size(super_block));
  // The inode bitmap has to fit in a single block.
  assert(inodes_per_group(super_block) <= 8 * block_size_);
  // There should fit exactly an integer number of inodes in one block.
  assert((block_size_ / inode_size_) * inode_size_ == block_size_);
  // Space needed for the inode table should match the returned value of the number of blocks they need.
  assert((inodes_per_group_ * inode_size_ - 1) / block_size_ + 1 == inode_blocks_per_group(super_block));

  // Do we have a journal?
  if (super_block.s_journal_dev == 0)
  {
    Inode& journal_inode = get_inode(super_block.s_journal_inum);
    int first_block = journal_inode.block()[0];
    assert(first_block);
    // Read the first superblock.
    device.seekg(block_to_offset(first_block));
    assert(device.good());
    device.read(reinterpret_cast<char*>(&journal_super_block), sizeof(journal_superblock_s));
    assert(device.good());
    if (commandline_superblock && commandline_journal)
    {
      // Print contents of superblock.
      std::cout << "Journal Super Block:\n\n";
      std::cout << "Signature: 0x" << be2le(journal_super_block.s_header.h_magic) << std::dec << '\n';
      std::cout << journal_super_block << '\n';
    }
    // Sanity checks.
    assert(be2le(journal_super_block.s_header.h_magic) == JFS_MAGIC_NUMBER);
    init_journal_consts();
  }

  // Check commandline options against superblock contents.
  if (commandline_journal && super_block.s_journal_dev)
  {
    std::cerr << progname << ": --journal: The journal appears to be external.\n";
    exit(EXIT_FAILURE);
  }
  if (commandline_inode != -1)
  {
    if ((uint32_t)commandline_inode >= inode_count_)
    {
      std::cerr << progname << ": --inode: inode " << commandline_inode << " is out of range. There are only " << inode_count_ << " inodes.\n";
      exit(EXIT_FAILURE);
    }
    commandline_group = inode_to_group(super_block, commandline_inode);
    // std::cout << "Inode " << commandline_inode << " is in group " << commandline_group << '\n';
  }
  if (commandline_block != -1)
  {
    if (commandline_block >= block_count(super_block))
    {
      std::cerr << progname << ": --block: block " << commandline_block << " is out of range. There are only " << block_count(super_block) << " blocks.\n";
      exit(EXIT_FAILURE);
    }
    commandline_group = block_to_group(super_block, commandline_block);
  }
  if (commandline_journal_block != -1)
  {
    if (commandline_journal_block >= block_count(journal_super_block))
    {
      std::cerr << progname << ": --journal-block: block " << commandline_journal_block << " is out of range. There are only " <<
          block_count(journal_super_block) << " blocks in the journal.\n";
      exit(EXIT_FAILURE);
    }
  }
  if (commandline_show_journal_inodes != -1)
  {
    if ((uint32_t)commandline_show_journal_inodes >= inode_count_)
    {
      std::cerr << progname << ": --show-journal-inodes: inode " << commandline_show_journal_inodes <<
          " is out of range. There are only " << inode_count_ << " inodes.\n";
      exit(EXIT_FAILURE);
    }
    commandline_group = inode_to_group(super_block, commandline_show_journal_inodes);
    // std::cout << "Inode " << commandline_show_journal_inodes << " is in group " << commandline_group << '\n';
  }

  // Print group summary, if needed.
  if (!commandline_journal && commandline_inode_to_block == -1)
  {
    std::cout << "Number of groups: " << groups_ << '\n';
    if (commandline_group == -1)
    {
      if (!commandline_action)
	for (int group = 0; group < groups_; ++group)
	{
	  std::cout << " Group\t" << group << ": ";
	  std::cout << group_descriptor_table[group] << '\n';
	}
    }
    else if (commandline_group < 0 || commandline_group >= groups_)
    {
      std::cerr << progname << ": --group: group " << commandline_group << " is out of range.\n";
      exit(EXIT_FAILURE);
    }
    else if (!commandline_action)
    {
      std::cout << " Group\t" << commandline_group << ": ";
      std::cout << group_descriptor_table[commandline_group] << '\n';
    }
  }

  if (commandline_action && !commandline_journal)
  {
    if (commandline_inode_to_block != -1)
      commandline_group = inode_to_group(super_block, commandline_inode_to_block);
    if (!commandline_group)
      std::cout << "Loading group metadata.." << std::flush;
    for (int group = 0; group < groups_; ++group)
    {
      if (commandline_group != -1 && group != commandline_group)
	continue;
      if (!commandline_group)
	std::cout << '.' << std::flush;
      load_meta_data(group);
    }
    if (!commandline_group)
      std::cout << " done\n";
  }

  // Needed here?
  init_journal();

  // Handle --inode
  if (commandline_inode != -1)
  {
    Inode& inode(get_inode(commandline_inode));
    if (commandline_print)
    {
      std::cout << "\nHex dump of inode " << commandline_inode << ":\n";
      dump_hex((unsigned char*)&inode, inode_size_);
      std::cout << std::dec << '\n';
    }
    unsigned int bit = commandline_inode - 1 - commandline_group * inodes_per_group_;
    assert(bit < 8U * block_size_);
    struct bitmap_ptr bmp = get_bitmap_mask(bit);
    bool allocated = (inode_bitmap[commandline_group][bmp.index] & bmp.mask);
    if (allocated)
      std::cout << "Inode is Allocated\n";
    else
      std::cout << "Inode is Unallocated\n";
    if (commandline_print)
    {
      std::cout << "Group: " << commandline_group << '\n';
      print_inode(inode);
    }
    if (is_directory(inode))
      print_directory_inode(commandline_inode);
  }
  // Handle --block
  if (commandline_block != -1 || (commandline_journal_block != -1 && commandline_journal))
  {
    if (commandline_journal && commandline_block != -1)
    {
      print_block_descriptors(commandline_block);
    }
    else
    {
      if (commandline_journal_block != -1 && commandline_journal)
      {
	// Translate block number.
	commandline_block = journal_block_to_real_block(commandline_journal_block);
	commandline_group = block_to_group(super_block, commandline_block);
      }
      unsigned char* block = new unsigned char[block_size_];    
      device.seekg(block_to_offset(commandline_block));
      device.read(reinterpret_cast<char*>(block), block_size_);
      if (commandline_print)
      {
	std::cout << "Hex dump of block " << commandline_block << ":\n";
	print_block(block);
	std::cout << '\n';
      }
      std::cout << "Group: " << commandline_group << '\n';
      unsigned int bit = commandline_block - first_data_block(super_block) - commandline_group * blocks_per_group(super_block);
      assert(bit < 8U * block_size_);
      struct bitmap_ptr bmp = get_bitmap_mask(bit);
      is_directory_type isdir = is_directory(block, commandline_block, false);
      if (block_bitmap[commandline_group] == NULL)
	load_meta_data(commandline_group);
      bool allocated = (block_bitmap[commandline_group][bmp.index] & bmp.mask);
      bool journal = is_journal(commandline_block);
      if (isdir == isdir_no)
      {
	if (allocated)
	{
	  std::cout << "Block " << commandline_block;
	  if (journal)
	  {
	    std::cout << " belongs to the journal.";
	    int real_block;
	    journal_header_t* header = reinterpret_cast<journal_header_t*>(block);
	    if (be2le(header->h_magic) == JFS_MAGIC_NUMBER)
	    {
	      std::cout << "\n\n";
	      switch (be2le(header->h_blocktype))
	      {
		case JFS_DESCRIPTOR_BLOCK:
		{
		  std::cout << *header << '\n';
		  journal_block_tag_t* journal_block_tag = reinterpret_cast<journal_block_tag_t*>(block + sizeof(journal_header_t));
		  int curblock = commandline_block;
		  for (;;)
		  {
		    uint32_t flags = be2le(journal_block_tag->t_flags);
		    ++curblock;
		    while(is_indirect_block_in_journal(curblock))
		      ++curblock;
		    int refered_block = be2le(journal_block_tag->t_blocknr);
		    std::cout << "  " << curblock << ((flags & JFS_FLAG_ESCAPE) ? "(escaped)" : "") << " = " <<
			refered_block << ((flags & JFS_FLAG_DELETED) ? "(deleted)" : "") << '\n';
		    if ((flags & JFS_FLAG_LAST_TAG))
		      break;
		    if (!(flags & JFS_FLAG_SAME_UUID))
		      journal_block_tag = reinterpret_cast<journal_block_tag_t*>((unsigned char*)journal_block_tag + 16);
		    ++journal_block_tag;
		  }
		  break;
		}
		case JFS_COMMIT_BLOCK:
		{
		  std::cout << *header << '\n';
		  break;
		}
		case JFS_SUPERBLOCK_V1:
		case JFS_SUPERBLOCK_V2:
		{
		  std::cout << *reinterpret_cast<journal_superblock_t*>(block) << '\n';
		  break;
		}
		case JFS_REVOKE_BLOCK:
		{
		  std::cout << *reinterpret_cast<journal_revoke_header_t*>(block) << '\n';
		  break;
		}
	      }
	    }
	    else if ((real_block = is_inode_block(commandline_block)))
	    {
	      std::cout << " It contains inode table block " << real_block << ".\n";
	      if (commandline_print)
	      {
		int inodenr = block_to_inode(real_block);
	        for (Inode* inode = reinterpret_cast<Inode*>(block); reinterpret_cast<unsigned char*>(inode) < block + block_size_;
		    inode = reinterpret_cast<Inode*>(reinterpret_cast<unsigned char*>(inode) + inode_size_), ++inodenr)
		{
		  std::cout << "\n--------------Inode " << inodenr << "-----------------------\n";
		  print_inode(*inode);
	        }
	      }
	    }
	    else
	      std::cout << '\n';
	  }
	  else
	  {
	    std::cout << " is Allocated.";
	    if (is_inode(commandline_block))
	    {
	      int inode = block_to_inode(commandline_block);
	      std::cout << " It's inside the inode table of group " << commandline_group <<
		  " (inodes [" << inode << " - " << (inode + block_size_ / inode_size_) << ">).";
	    }
	    std::cout << '\n';
	  }
	}
	else
	{
	  std::cout << "Block " << commandline_block << " is Unallocated.\n";
	  assert(!is_inode(commandline_block));	// All inode blocks are allocated.
	  assert(!journal);			// All journal blocks are allocated.
	}
      }
      else
      {
	std::cout << "\nBlock " << commandline_block << " is a directory. The block is " << (allocated ? journal ? "a Journal block" : "Allocated" : "Unallocated") << "\n\n";
	if (commandline_ls)
	  print_restrictions();
	if (isdir == isdir_start)
	{
	  ext3_dir_entry_2* dir_entry = reinterpret_cast<ext3_dir_entry_2*>(block);
	  Inode& inode = get_inode(dir_entry->inode);
	  if (!is_directory(inode) || (inode.block()[0] && inode.block()[0] != (__le32)commandline_block))
	  {
	    print_directory(block, commandline_block);
	    std::cout << "WARNING: inode " << dir_entry->inode << " was reallocated!\n";
	  }
	  else if (!inode.block()[0])
	  {
	    print_directory(block, commandline_block);
	    if (allocated)	// Is this at all possible?
	      std::cout << "WARNING: inode " << dir_entry->inode << " doesn't contain any blocks. This directory was deleted.\n";
	  }
	  else
	  {
	    // Run over all blocks.
	    bool reused_or_corrupted_indirect_block1 = iterate_over_all_blocks_of(inode, print_directory_action);
	    assert(!reused_or_corrupted_indirect_block1);
	  }
	}
	else
	  print_directory(block, commandline_block);
      }
      delete [] block;
    }
  }
  // Make sure the output directory exists.
  if (!commandline_restore_file.empty() || commandline_restore_all)
  {
    struct stat statbuf;
    if (stat(outputdir.c_str(), &statbuf) == -1)
    {
      int error = errno;
      if (error != ENOENT)
      {
	std::cout << std::flush;
	std::cerr << "ERROR: stat: " << outputdir << ": " << strerror(error) << std::endl;
	exit(EXIT_FAILURE);
      }
      else if (mkdir(outputdir.c_str(), 0755) == -1 && errno != EEXIST)
      {
	perror("mkdir");
	std::cout << std::flush;
	std::cerr << "Failed to create output directory " << outputdir << '\n';
	exit(EXIT_FAILURE);
      }
      std::cout << "Writing output to directory " << outputdir << std::endl;
    }
    else if (!S_ISDIR(statbuf.st_mode))
    {
      std::cout << std::flush;
      std::cerr << "ERROR: " << outputdir << " exists but is not a directory!\n";
      exit(EXIT_FAILURE);
    }
  }
  // Handle --dump-names
  if (commandline_restore_all || commandline_dump_names)
    dump_names();
  // Handle --restore-file
  if (!commandline_restore_file.empty())
    restore_file(commandline_restore_file);
  // Handle --show-hardlinks
  if (commandline_show_hardlinks)
    show_hardlinks();
  // Handle --journal-transaction
  if (commandline_journal_transaction != -1)
    handle_commandline_journal_transaction();
  // Handle --histogram
  if (commandline_histogram)
  {
    std::cout << '\n';
    if (commandline_group != -1)
      std::cout << "Only showing histogram of group " << commandline_group << '\n';
    print_restrictions();
    if (commandline_deleted || commandline_histogram == hist_dtime)
      std::cout << "Only showing deleted entries.\n";
    if (commandline_histogram == hist_atime ||
        commandline_histogram == hist_ctime ||
	commandline_histogram == hist_mtime ||
	commandline_histogram == hist_dtime)
      hist_init(commandline_after, commandline_before);
    else if (commandline_histogram == hist_group)
      hist_init(0, groups_);
    // Run over all (requested) groups.
    for (int group = 0, ibase = 0; group < groups_; ++group, ibase += inodes_per_group_)
    {
      if (commandline_group != -1 && group != commandline_group)
	continue;
      // Run over all inodes.
      for (int bit = 0, inode_number = ibase + 1; bit < inodes_per_group_; ++bit, ++inode_number)
      {
	Inode& inode(get_inode(inode_number));
	bool deleted = (inode.dtime() != 0);
	if ((commandline_deleted || commandline_histogram == hist_dtime) && !deleted)
          continue;
	if (commandline_directory && !is_directory(inode))
	  continue;
	if (commandline_allocated || commandline_unallocated)
	{
	  struct bitmap_ptr bmp = get_bitmap_mask(bit);
	  bool allocated = (inode_bitmap[group][bmp.index] & bmp.mask);
	  if (commandline_allocated && !allocated)
	    continue;
	  if (commandline_unallocated && allocated)
	    continue;
	}
	time_t xtime = 0;
	if (commandline_histogram == hist_dtime)
	{
	  xtime = inode.dtime();
	  if (xtime == 0)
	    continue;
	}
	else if (commandline_histogram == hist_atime)
	{
	  xtime = inode.atime();
	  if (xtime == 0)
	    continue;
        }
	else if (commandline_histogram == hist_ctime)
	{
	  xtime = inode.ctime();
	  if (xtime == 0)
	    continue;
	}
	else if (commandline_histogram == hist_mtime)
	{
	  xtime = inode.mtime();
	  if (xtime == 0)
	    continue;
        }
	if (xtime && commandline_after <= xtime && xtime < commandline_before)
	  hist_add(xtime);
	if (commandline_histogram == hist_group)
	{
	  if (commandline_after && commandline_after > (time_t)inode.dtime())
	    continue;
	  if (commandline_before && (time_t)inode.dtime() >= commandline_before)
	    continue;
	  hist_add(group);
        }
      }
    }
    hist_print();
  }
  // Handle --search and --search-start
  if (!commandline_search_start.empty() || !commandline_search.empty())
  {
    bool start = !commandline_search_start.empty();
    size_t len = start ? commandline_search_start.length() : commandline_search.length();
    assert(len <= (size_t)block_size_);
    char* pattern = new char [len];
    strncpy(pattern, start ? commandline_search_start.data() : commandline_search.data(), len);
    static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
    if (commandline_allocated && commandline_unallocated)
      commandline_allocated = commandline_unallocated = false;
    if (commandline_allocated)
      std::cout << "Allocated blocks ";
    else if (commandline_unallocated)
      std::cout << "Unallocated blocks ";
    else
      std::cout << "Blocks ";
    std::cout << (start ? "starting with" : "containing") << " \"" << std::string(pattern, len) << "\":" << std::flush;
    assert((inodes_per_group_ * inode_size_) % block_size_ == 0);
    for (int group = 0; group < groups_; ++group)
    {
      int first_block = group_to_block(super_block, group);  
      int last_block = std::min(first_block + blocks_per_group(super_block), block_count(super_block));
      // Skip inodes.
      int inode_table = group_descriptor_table[group].bg_inode_table;
      first_block = inode_table + inodes_per_group_ * inode_size_ / block_size_;
      unsigned int bit = first_block - first_data_block(super_block) - group * blocks_per_group(super_block);
      for (int block = first_block; block < last_block; ++block, ++bit)
      {
	struct bitmap_ptr bmp = get_bitmap_mask(bit);
	bool allocated = (block_bitmap[group][bmp.index] & bmp.mask);
	if (commandline_allocated && !allocated)
	  continue;
	if (commandline_unallocated && allocated)
	  continue;
	bool found = false;
        get_block(block, block_buf);
        if (start)
	{
#if 1
	  if (std::memcmp(block_buf, pattern, len) == 0)
	    found = true;
#else
          if (std::isdigit(block_buf[0]) && std::isdigit(block_buf[1]) && std::isdigit(block_buf[2]) && block_buf[3] == ' ' && std::isdigit(block_buf[4]) &&
	      std::isdigit(block_buf[5]) && block_buf[9] == 0 && block_buf[10] == 0 && block_buf[11] == 0 && block_buf[12] == 0 &&
	      block_buf[13] == 0 && block_buf[14] == 0 && block_buf[15] == 0 && block_buf[16] == 0)
	    std::cout << block << " : " << block_buf << '\n';
#endif
	}
        else
	{
	  for (unsigned char* ptr = block_buf; ptr < block_buf + block_size_ - len; ++ptr)
	  {
	    if (*ptr == *pattern &&
	        (len == 1 || (ptr[1] == pattern[1] &&
		(len == 2 || (ptr[2] == pattern[2] && std::memcmp(ptr, pattern, len) == 0)))))
	    {
	      found = true;
	      break;
	    }
	  }
	}
	if (found)
	{
	  if (!commandline_allocated && allocated)
	    std::cout << ' ' << block << " (allocated)" << std::flush;
          else
	    std::cout << ' ' << block << std::flush;
        }
      }
    }
    delete [] pattern;
    std::cout << '\n';
  }
  // Handle --search-inode
  if (commandline_search_inode != -1)
  {
    std::cout << "Inodes refering to block " << commandline_search_inode << ':' << std::flush;
    for (uint32_t inode = 1; inode <= inode_count_; ++inode)
    {
      Inode& ino = get_inode(inode);
      found_block = false;
      block_looking_for = commandline_search_inode;
      bool reused_or_corrupted_indirect_block2 = iterate_over_all_blocks_of(ino, find_block_action);
      assert(!reused_or_corrupted_indirect_block2);
      if (found_block)
        std::cout << ' ' << inode << std::flush;
    }
    std::cout << '\n';
  }
  // Handle --search-zeroed-inodes
  if (commandline_search_zeroed_inodes)
  {
    std::cout << "Allocated inodes filled with zeroes:" << std::flush;
    for (uint32_t inode = 1; inode <= inode_count_; ++inode)
    {
      if (commandline_group != -1)
      {
	int group = (inode - 1) / inodes_per_group_;
	if (group != commandline_group)
	  continue;
      }
      Inode& ino = get_inode(inode);
      static char zeroes[128] = {0, };
      if (is_allocated(inode) && std::memcmp(&ino, zeroes, sizeof(zeroes)) == 0)
        std::cout << ' ' << inode << std::flush;
    }
    std::cout << '\n';
  }
  // Handle --inode-to-block
  if (commandline_inode_to_block != -1)
  {
    int block = inode_to_block(super_block, commandline_inode_to_block);
    std::cout << "Inode " << commandline_inode_to_block << " resides in block " << block <<
        " at offset 0x" << std::hex << ((commandline_inode_to_block - block_to_inode(block)) * inode_size_) << std::dec << ".\n";
  }
  // Handle --show-journal-inodes
  if (commandline_show_journal_inodes != -1)
    show_journal_inodes(commandline_show_journal_inodes);

  // Print some useful information if no useful information was printed yet.
  if (!commandline_action && !commandline_journal)
  {
    std::cout << "\nNo action was specified. For example, specify one of:\n";
    std::cout << "    --inode ino            Show info on inode 'ino'; inode 2 is the root.\n";
    std::cout << "    --block blk [--ls]     Show info on block 'blk'.\n";
    std::cout << "    --histogram=dtime --after=1000000000 --before=1400000000\n";
    std::cout << "                           Show deletion-time histogram (zoom in afterwards).\n";
    std::cout << "    --help                 Show all possible command line options.\n";
  }

  // Clean up.
  if (commandline_action)
  {
    delete [] inodes_buf;
    for (int group = 0; group < groups_; ++group)
    {
      if (block_bitmap[group])
      {
	delete [] inode_bitmap[group];
	delete [] block_bitmap[group];
#if USE_MMAP
        munmap(all_mmaps[group], inodes_per_group_ * inode_size_ + ((char*)all_inodes[group] - (char*)all_mmaps[group]));
#else
	delete [] all_inodes[group];
#endif
      }
    }
    delete [] inode_bitmap;
    delete [] block_bitmap;
    delete [] all_inodes;
#if USE_MMAP
    delete [] all_mmaps;
#endif
    delete [] group_descriptor_table;
  }

  device.close();
#if USE_MMAP
  close(device_fd);
#endif
}

//-----------------------------------------------------------------------------
//
// ostream operator<<'s
//

struct FileSystemState {
  private:
    __le16 M_state;
  public:
    FileSystemState(__le16 state) : M_state(state) { }
    friend std::ostream& operator<<(std::ostream& os, FileSystemState const& state)
	{
	  if ((state.M_state & EXT3_VALID_FS))
	    os << "'Unmounted cleanly'";
	  else
	    os << "Not clean";
	  if ((state.M_state & EXT3_ERROR_FS))
	    os << " 'Errors detected'";
	  return os;
	}
};

std::ostream& operator<<(std::ostream& os, ext3_super_block const& super_block)
{
  // This was generated with:
  // awk 'BEGIN { decode=0 } /^struct ext3_super_block/ { decode=1 } /^};/ { decode=0 } { if (decode) print; }' /usr/include/linux/ext3_fs.h | sed -rn 's/^[[:space:]]*(|\/\*[0-9A-F]*\*\/[[:space:]]*)__[[:alnum:]_]*[[:space:]]*(s_[[:alnum:]_]*)(;|\[[0-9]+\];)[[:space:]]*\/\*[[:space:]](.*)[[:space:]]\*\/.*/  os << "\4: " << super_block.\2 << '"'"'\\n'"'"';/p'
  os << "Inodes count: " << inode_count(super_block) << '\n';
  os << "Blocks count: " << block_count(super_block) << '\n';
  os << "Reserved blocks count: " << reserved_block_count(super_block) << '\n';
  os << "Free blocks count: " << super_block.s_free_blocks_count << '\n';
  os << "Free inodes count: " << super_block.s_free_inodes_count << '\n';
  os << "First Data Block: " << first_data_block(super_block) << '\n';
  os << "Block size: " << block_size(super_block) << '\n';
  os << "Fragment size: " << fragment_size(super_block) << '\n';
  os << "Number of blocks per group: " << blocks_per_group(super_block) << '\n';
  os << "Number of fragments per group: " << super_block.s_frags_per_group << '\n';
  os << "Number of inodes per group: " << inodes_per_group(super_block) << '\n';
  time_t mtime = super_block.s_mtime;
  os << "Mount time: " << std::ctime(&mtime);
  time_t wtime = super_block.s_wtime;
  os << "Write time: " << std::ctime(&wtime);
  os << "Mount count: " << super_block.s_mnt_count << '\n';
  os << "Maximal mount count: " << super_block.s_max_mnt_count << '\n';
  os << "Magic signature: " << std::hex << "0x" << super_block.s_magic << std::dec << '\n';
  os << "File system state: " << FileSystemState(super_block.s_state) << '\n';
  // os << "Behaviour when detecting errors: " << super_block.s_errors << '\n';
  // os << "minor revision level: " << super_block.s_minor_rev_level << '\n';
  // os << "time of last check: " << super_block.s_lastcheck << '\n';
  // os << "max. time between checks: " << super_block.s_checkinterval << '\n';
  // os << "OS: " << super_block.s_creator_os << '\n';
  // os << "Revision level: " << super_block.s_rev_level << '\n';
  // os << "Default uid for reserved blocks: " << super_block.s_def_resuid << '\n';
  // os << "Default gid for reserved blocks: " << super_block.s_def_resgid << '\n';
  // os << "First non-reserved inode: " << super_block.s_first_ino << '\n';
  os << "Size of inode structure: " << super_block.s_inode_size << '\n';
  os << "Block group # of this superblock: " << super_block.s_block_group_nr << '\n';
  // os << "compatible feature set: " << super_block.s_feature_compat << '\n';
  // os << "incompatible feature set: " << super_block.s_feature_incompat << '\n';
  // os << "readonly-compatible feature set: " << super_block.s_feature_ro_compat << '\n';
  // os << "128-bit uuid for volume: " << super_block.s_uuid << '\n';
  // os << "For compression: " << super_block.s_algorithm_usage_bitmap << '\n';
  // os << "Nr to preallocate for dirs: " << super_block.s_prealloc_dir_blocks << '\n';
  os << "Per group desc for online growth: " << super_block.s_reserved_gdt_blocks << '\n';
  os << "UUID of journal superblock:";
  for (int i = 0; i < 16; ++i)
    os << " 0x" << std::hex << std::setfill('0') << std::setw(2) << (int)super_block.s_journal_uuid[i];
  os << std::dec << '\n';
  os << "Inode number of journal file: " << super_block.s_journal_inum << '\n';
  os << "Device number of journal file: " << super_block.s_journal_dev << '\n';
  os << "Start of list of inodes to delete: " << super_block.s_last_orphan << '\n';
  // os << "HTREE hash seed: " << super_block.s_hash_seed << '\n';
  // os << "Default hash version to use: " << super_block.s_def_hash_version << '\n';
  os << "First metablock block group: " << super_block.s_first_meta_bg << '\n';
  // os << "Padding to the end of the block: " << super_block.s_reserved << '\n';
  return os;
}

std::ostream& operator<<(std::ostream& os, ext3_group_desc const& group_desc)
{
  os << "block bitmap at " << group_desc.bg_block_bitmap <<
        ", inodes bitmap at " << group_desc.bg_inode_bitmap <<
	", inode table at " << group_desc.bg_inode_table << '\n';
  os << "\t   " << group_desc.bg_free_blocks_count << " free blocks, " <<
                   group_desc.bg_free_inodes_count << " free inodes, " <<
		   group_desc.bg_used_dirs_count << " used directory";
  return os;
}

std::ostream& operator<<(std::ostream& os, journal_header_t const& journal_header)
{
  os << "Block type: ";
  switch (be2le(journal_header.h_blocktype))
  {
    case JFS_DESCRIPTOR_BLOCK:
      os << "Descriptor block";
      break;
    case JFS_COMMIT_BLOCK:
      os << "Commit block";
      break;
    case JFS_SUPERBLOCK_V1:
      os << "Superblock version 1";
      break;
    case JFS_SUPERBLOCK_V2:
      os << "Superblock version 2";
      break;
    case JFS_REVOKE_BLOCK:
      os << "Revoke block";
      break;
    default:
      os << "*UNKNOWN* (0x" << std::hex << be2le(journal_header.h_blocktype) << std::dec << ')';
      break;
  }
  os << '\n';
  os << "Sequence Number: " << be2le(journal_header.h_sequence);
  return os;
}

std::ostream& operator<<(std::ostream& os, journal_superblock_t const& journal_super_block)
{
  os << journal_super_block.s_header << '\n';
  os << "Journal block size: " << be2le(journal_super_block.s_blocksize) << '\n';
  os << "Number of journal blocks: " << be2le(journal_super_block.s_maxlen) << '\n';
  os << "Journal block where the journal actually starts: " << be2le(journal_super_block.s_first) << '\n';
  os << "Sequence number of first transaction: " << be2le(journal_super_block.s_sequence) << '\n';
  os << "Journal block of first transaction: " << be2le(journal_super_block.s_start) << '\n';
  os << "Error number: " << be2le(journal_super_block.s_errno) << '\n';
  if (be2le(journal_super_block.s_header.h_blocktype) != JFS_SUPERBLOCK_V2)
    return os;
  os << "Compatible Features: " << be2le(journal_super_block.s_feature_compat) << '\n';
  os << "Incompatible features: " << be2le(journal_super_block.s_feature_incompat) << '\n';
  os << "Read only compatible features: " << be2le(journal_super_block.s_feature_ro_compat) << '\n';
  os << "Journal UUID:";
  for (int i = 0; i < 16; ++i)
    os << std::hex << " 0x" << std::setfill('0') << std::setw(2) << (int)be2le(journal_super_block.s_uuid[i]);
  os << std::dec << '\n';
  int32_t nr_users = be2le(journal_super_block.s_nr_users);
  os << "Number of file systems using journal: " << nr_users << '\n';
  assert(nr_users <= 48);
  os << "Location of superblock copy: " << be2le(journal_super_block.s_dynsuper) << '\n';
  os << "Max journal blocks per transaction: " << be2le(journal_super_block.s_max_transaction) << '\n';
  os << "Max file system blocks per transaction: " << be2le(journal_super_block.s_max_trans_data) << '\n';
  os << "IDs of all file systems using the journal:\n";
  for (int u = 0; u < nr_users; ++u)
  {
    os << (u + 1) << '.';
    for (int i = 0; i < 16; ++i)
      os << std::hex << " 0x" << std::setfill('0') << std::setw(2) << (int)be2le(journal_super_block.s_users[u * 16 + i]);
    os << std::dec << '\n';
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, journal_block_tag_t const& journal_block_tag)
{
  os << "File system block: " << be2le(journal_block_tag.t_blocknr) << '\n';
  os << "Entry flags:";
  uint32_t flags = be2le(journal_block_tag.t_flags);
  if ((flags & JFS_FLAG_ESCAPE))
    os << " ESCAPED";
  if ((flags & JFS_FLAG_SAME_UUID))
    os << " SAME_UUID";
  if ((flags & JFS_FLAG_DELETED))
    os << " DELETED";
  if ((flags & JFS_FLAG_LAST_TAG))
    os << " LAST_TAG";
  os << '\n';
  return os;
}

std::ostream& operator<<(std::ostream& os, journal_revoke_header_t const& journal_revoke_header)
{
  os << journal_revoke_header.r_header << '\n';
  uint32_t count = be2le(journal_revoke_header.r_count);
  os << "Bytes used: " << count << '\n';
  assert(sizeof(journal_revoke_header_t) <= count && count <= (size_t)block_size_);
  count -= sizeof(journal_revoke_header_t);
  assert(count % sizeof(__be32) == 0);
  count /= sizeof(__be32);
  __be32* ptr = reinterpret_cast<__be32*>((unsigned char*)&journal_revoke_header + sizeof(journal_revoke_header_t));
  int c = 0;
  for (uint32_t b = 0; b < count; ++b)
  {
    std::cout << std::setfill(' ') << std::setw(8) << be2le(ptr[b]);
    ++c;
    c &= 7;
    if (c == 0)
      std::cout << '\n';
  }
  return os;
}

//-----------------------------------------------------------------------------
//
// Commandline
//

static void print_usage(std::ostream& os)
{
  os << "Usage: " << progname << " [options] [--] device-file\n";
//       012345678901234567890123456789012345678901234567890123456789012345678901234567890
  os << "Options:\n";
  os << "  --version, -[vV]       Print version and exit successfully.\n";
  os << "  --help,                Print this help and exit successfully.\n";
  os << "  --superblock           Print contents of superblock in addition to the rest.\n";
  os << "                         If no action is specified then this option is implied.\n";
  os << "  --print                Print content of block or inode, if any.\n";
  os << "  --ls                   Print directories with only one line per entry.\n";
  os << "                         This option is often needed to turn on filtering.\n";
  os << "  --accept filen         Accept 'filen' as a legal filename.\n";
  os << "                         Can be used multiple times.\n";
  os << "  --journal              Show content of journal.\n";
  os << "  --show-path-inodes     Show the inode of each directory component in paths.\n";
//       012345678901234567890123456789012345678901234567890123456789012345678901234567890
  os << "Filters:\n";
  os << "  --group grp            Only process group 'grp'.\n";
  os << "  --directory            Only process directory inodes.\n";
  os << "  --after dtime          Only entries deleted on or after 'dtime'.\n";
  os << "  --before dtime         Only entries deleted before 'dtime'.\n";
  os << "  --deleted              Only show/process deleted entries.\n";
  os << "  --allocated            Only show/process allocated inodes/blocks.\n";
  os << "  --unallocated          Only show/process unallocated inodes/blocks.\n";
  os << "  --reallocated          Do not suppress entries with reallocated inodes.\n";
  os << "                         Inodes are considered 'reallocated' if the entry\n";
  os << "                         is deleted but the inode is allocated, but also when\n";
  os << "                         the file type in the dir entry and the inode are\n";
  os << "                         different.\n";
  os << "  --zeroed-inodes        Do not suppress entries with zeroed inodes. Linked\n";
  os << "                         entries are always shown, regardless of this option.\n";
  os << "  --depth depth          Process directories recursively up till a depth\n";
  os << "                         of 'depth'.\n";
//       012345678901234567890123456789012345678901234567890123456789012345678901234567890
  os << "Actions:\n";
  os << "  --inode-to-block ino   Print the block that contains inode 'ino'.\n";
  os << "  --inode ino            Show info on inode 'ino'.\n";
  os << "                         If --ls is used and the inode is a directory, then\n";
  os << "                         the filters apply to the entries of the directory.\n";
  os << "                         If you do not use --ls then --print is implied.\n";
  os << "  --block blk            Show info on block 'blk'.\n";
  os << "                         If --ls is used and the block is the first block\n";
  os << "                         of a directory, then the filters apply to entries\n";
  os << "                         of the directory.\n";
  os << "                         If you do not use --ls then --print is implied.\n";
//       012345678901234567890123456789012345678901234567890123456789012345678901234567890
  os << "  --histogram=[atime|ctime|mtime|dtime|group]\n";
  os << "                         Generate a histogram based on the given specs.\n";
  os << "                         Using atime, ctime or mtime will change the\n";
  os << "                         meaning of --after and --before to those times.\n";
  os << "  --journal-block jblk   Show info on journal block 'jblk'.\n";
  os << "  --journal-transaction seq\n";
  os << "                         Show info on transaction with sequence number 'seq'.\n";
  os << "  --dump-names           Write the path of files to stdout.\n";
  os << "                         This implies --ls but suppresses it's output.\n";
  os << "  --search-start str     Find blocks that start with the fixed string 'str'.\n";
  os << "  --search str           Find blocks that contain the fixed string 'str'.\n";
  os << "  --search-inode blk     Find inodes that refer to block 'blk'.\n";
  os << "  --search-zeroed-inodes Return allocated inode table entries that are zeroed.\n";
//       012345678901234567890123456789012345678901234567890123456789012345678901234567890
  os << "  --inode-dirblock-table dir\n";
  os << "                         Print a table for directory path 'dir' of directory\n";
  os << "                         block numbers found and the inodes used for each file.\n";
  os << "  --show-journal-inodes ino\n";
  os << "                         Show copies of inode 'ino' still in the journal.\n";
  os << "  --restore-file 'path'  Will restore file 'path'. 'path' is relative to root\n";
  os << "                         of the partition and does not start with a '/' (it\n";
  os << "                         must be one of the paths returned by --dump-names).\n";
  os << "                         The restored directory, file or symbolic link is\n";
  os << "                         created in the current directory as ./'path'.\n";
  os << "  --restore-all          As --restore-file but attempts to restore everything.\n";
  os << "                         The use of --after is highly recommended because the\n";
  os << "                         attempt to restore very old files will only result in\n";
  os << "                         them being hard linked to a more recently deleted file\n";
  os << "                         and as such polute the output.\n";
  os << "  --show-hardlinks       Show all inodes that are shared by two or more files.\n";
}

static void print_version(void)
{
  std::cout << "ext3grep v" VERSION ", Copyright (C) 2008 Carlo Wood.\n";
  std::cout << "ext3grep comes with ABSOLUTELY NO WARRANTY;\n";
  std::cout << "This program is free software; your freedom to use, change\n";
  std::cout << "and distribute this program is protected by the GPL.\n";
}

extern char *optarg;
extern int optind, opterr, optopt;

enum opts {
  opt_version,
  opt_superblock,
  opt_group,
  opt_inode,
  opt_block,
  opt_print,
  opt_ls,
  opt_after,
  opt_before,
  opt_deleted,
  opt_allocated,
  opt_unallocated,
  opt_histogram,
  opt_directory,
  opt_accept,
  opt_dump_names,
  opt_reallocated,
  opt_depth,
  opt_journal,
  opt_journal_block,
  opt_journal_transaction,
  opt_search,
  opt_search_start,
  opt_search_inode,
  opt_search_zeroed_inodes,
  opt_inode_to_block,
  opt_zeroed_inodes,
  opt_show_path_inodes,
  opt_inode_dirblock_table,
  opt_show_journal_inodes,
  opt_restore_file,
  opt_restore_all,
  opt_show_hardlinks,
  opt_help
};

static void decode_commandline_options(int& argc, char**& argv)
{
  int short_option;
  static int long_option;
  struct option longopts[] = {
    {"help", 0, &long_option, opt_help},
    {"version", 0, &long_option, opt_version},
    {"superblock", 0, &long_option, opt_superblock},
    {"group", 1, &long_option, opt_group},
    {"inode", 1, &long_option, opt_inode},
    {"block", 1, &long_option, opt_block},
    {"print", 0, &long_option, opt_print},
    {"ls", 0, &long_option, opt_ls},
    {"after", 1, &long_option, opt_after},
    {"before", 1, &long_option, opt_before},
    {"deleted", 0, &long_option, opt_deleted},
    {"allocated", 0, &long_option, opt_allocated},
    {"unallocated", 0, &long_option, opt_unallocated},
    {"reallocated", 0, &long_option, opt_reallocated},
    {"histogram", 1, &long_option, opt_histogram},
    {"directory", 0, &long_option, opt_directory},
    {"accept", 1, &long_option, opt_accept},
    {"dump-names", 0, &long_option, opt_dump_names},
    {"depth", 1, &long_option, opt_depth},
    {"journal", 0, &long_option, opt_journal},
    {"journal-block", 1, &long_option, opt_journal_block},
    {"journal-transaction", 1, &long_option, opt_journal_transaction},
    {"search", 1, &long_option, opt_search},
    {"search-start", 1, &long_option, opt_search_start},
    {"search-inode", 1, &long_option, opt_search_inode},
    {"search-zeroed-inodes", 0, &long_option, opt_search_zeroed_inodes},
    {"inode-to-block", 1, &long_option, opt_inode_to_block},
    {"zeroed-inodes", 0, &long_option, opt_zeroed_inodes},
    {"show-path-inodes", 0, &long_option, opt_show_path_inodes},
    {"inode-dirblock-table", 1, &long_option, opt_inode_dirblock_table},
    {"show-journal-inodes", 1, &long_option, opt_show_journal_inodes},
    {"restore-file", 1, &long_option, opt_restore_file},
    {"restore-all", 0, &long_option, opt_restore_all},
    {"show-hardlinks", 0, &long_option, opt_show_hardlinks},
    {NULL, 0, NULL, 0}
  };

  int exclusive1 = 0;
  int exclusive2 = 0;
  std::string hist_arg;
  progname = argv[0];
  while ((short_option = getopt_long(argc, argv, "vV", longopts, NULL)) != -1)
  {
    switch (short_option)
    {
      case 0:
        switch (long_option)
        {
          case opt_help:
            print_usage(std::cout);
            exit(EXIT_SUCCESS);
          case opt_version:
            print_version();
            exit(EXIT_SUCCESS);
	  case opt_superblock:
	    commandline_superblock = true;
	    break;
	  case opt_print:
	    commandline_print = true;
	    break;
	  case opt_ls:
	    commandline_ls = true;
	    break;
	  case opt_dump_names:
	    commandline_dump_names = true;
	    ++exclusive1;
	    ++exclusive2;
	    break;
	  case opt_journal:
	    commandline_journal = true;
	    break;
	  case opt_show_path_inodes:
	    commandline_show_path_inodes = true;
	    break;
	  case opt_depth:
	    commandline_depth = atoi(optarg);
	    if (commandline_depth < 0)
	    {
	      std::cerr << progname << ": --depth: cannot use negative values.\n";
	      exit(EXIT_FAILURE);
	    }
	    break;
	  case opt_deleted:
	    commandline_deleted = true;
	    break;
	  case opt_directory:
	    commandline_directory = true;
	    break;
	  case opt_allocated:
	    commandline_allocated = true;
	    break;
	  case opt_unallocated:
	    commandline_unallocated = true;
	    break;
	  case opt_reallocated:
	    commandline_reallocated = true;
	    break;
	  case opt_zeroed_inodes:
	    commandline_zeroed_inodes = true;
	    break;
	  case opt_after:
            commandline_after = atoi(optarg);
	    break;
	  case opt_before:
            commandline_before = atoi(optarg);
	    break;
	  case opt_search_zeroed_inodes:
	    commandline_search_zeroed_inodes = true;
	    ++exclusive2;
	    break;
	  case opt_search:
            commandline_search = optarg;
	    ++exclusive2;
	    break;
	  case opt_search_start:
            commandline_search_start = optarg;
	    ++exclusive2;
	    break;
	  case opt_inode_dirblock_table:
	    commandline_inode_dirblock_table = optarg;
	    break;
	  case opt_restore_file:
	    commandline_restore_file = optarg;
	    break;
	  case opt_restore_all:
	    commandline_restore_all = true;
	    break;
	  case opt_show_hardlinks:
	    commandline_show_hardlinks = true;
	    break;
	  case opt_search_inode:
            commandline_search_inode = atoi(optarg);
	    if (commandline_search_inode <= 0)
	    {
	      std::cerr << progname << ": --search-inode: block " << commandline_search_inode << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive2;
	    break;
          case opt_group:
            commandline_group = atoi(optarg);
	    if (commandline_group < 0)
	    {
	      std::cerr << progname << ": --group: group " << commandline_group << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive1;
            break;
          case opt_inode_to_block:
            commandline_inode_to_block = atoi(optarg);
	    if (commandline_inode_to_block < 1)
	    {
	      std::cerr << progname << ": --inode-to-block: inode " << commandline_inode_to_block << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
            break;
          case opt_inode:
            commandline_inode = atoi(optarg);
	    if (commandline_inode < 1)
	    {
	      std::cerr << progname << ": --inode: inode " << commandline_inode << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive1;
	    ++exclusive2;
            break;
          case opt_block:
            commandline_block = atoi(optarg);
	    if (commandline_block < 0)
	    {
	      std::cerr << progname << ": --block: block " << commandline_block << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive1;
	    ++exclusive2;
            break;
	  case opt_show_journal_inodes:
	    commandline_show_journal_inodes = atoi(optarg);
	    if (commandline_show_journal_inodes < 1)
	    {
	      std::cerr << progname << ": --show-journal-inodes: inode " << commandline_show_journal_inodes << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive1;
	    ++exclusive2;
	    break;
          case opt_journal_block:
            commandline_journal_block = atoi(optarg);
	    if (commandline_journal_block < 0)
	    {
	      std::cerr << progname << ": --journal-block: block " << commandline_journal_block << " is out of range.\n";
	      exit(EXIT_FAILURE);
	    }
	    ++exclusive1;
	    ++exclusive2;
            break;
	  case opt_journal_transaction:
            commandline_journal_transaction = atoi(optarg);
	    break;
	  case opt_histogram:
	  {
	    hist_arg = optarg;
	    if (hist_arg == "atime")
	      commandline_histogram = hist_atime;
	    else if (hist_arg == "ctime")
	      commandline_histogram = hist_ctime;
	    else if (hist_arg == "mtime")
	      commandline_histogram = hist_mtime;
	    else if (hist_arg == "dtime")
	      commandline_histogram = hist_dtime;
	    else if (hist_arg == "group")
	      commandline_histogram = hist_group;
	    else
	    {
	      std::cerr << progname << ": --histogram: " << hist_arg << ": unknown histogram type.\n";
	      exit(EXIT_FAILURE);
	    }
	    break;
	  }
	  case opt_accept:
	  {
	    accepted_filenames.insert(optarg);
	    break;
	  }
        }
        break;
      case 'v':
      case 'V':
        print_version();
        exit(EXIT_SUCCESS);
    }
  }

  if (exclusive1 > 1)
  {
    std::cerr << progname << ": Only one of --group, --inode, --block, --journal-block, --dump-names or --show-journal-inodes may be specified.\n";
    exit(EXIT_FAILURE);
  }
  if (exclusive2 > 1)
  {
    std::cerr << progname << ": Only one of --inode, --block, --search*, --journal-block, --dump-names or --show-journal-inodes may be specified.\n";
    exit(EXIT_FAILURE);
  }
  if (commandline_allocated && commandline_unallocated)
  {
    std::cerr << progname << ": Only one of --allocated or --unallocated may be specified.\n";
    exit(EXIT_FAILURE);
  }
  if (commandline_dump_names)
    commandline_ls = true;
  bool outputwritten = false;
  if ((commandline_block != -1 || commandline_inode != -1) && !commandline_ls && !commandline_print)
  {
    std::cout << "No --ls used; implying --print.\n";
    commandline_print = true;
    outputwritten = true;
  }
  commandline_action =
      (commandline_inode != -1 ||
       commandline_block != -1 ||
       commandline_journal_block != -1 ||
       commandline_journal_transaction != -1 ||
       commandline_dump_names ||
       commandline_show_journal_inodes != -1 ||
       commandline_histogram ||
       !commandline_search.empty() ||
       !commandline_search_start.empty() ||
       commandline_search_inode != -1||
       commandline_search_zeroed_inodes ||
       commandline_inode_to_block != -1 ||
       !commandline_restore_file.empty() ||
       commandline_restore_all ||
       commandline_show_hardlinks);
  if (!commandline_action && !commandline_superblock)
  {
    std::cout << "No action specified; implying --superblock.\n";
    commandline_superblock = true;
    outputwritten = true;
  }
  if ((commandline_histogram == hist_atime ||
       commandline_histogram == hist_ctime ||
       commandline_histogram == hist_mtime ||
       commandline_histogram == hist_dtime) &&
      !(commandline_before && commandline_after))
  {
    if (!commandline_before)
    {
      commandline_before = std::numeric_limits<int32_t>::max();
      std::cout << progname << ": --histogram=" << hist_arg << ": no --before given, assuming --before=" << commandline_before << '\n';
    }
    if (!commandline_after)
    {
      commandline_after = 1;
      std::cout << progname << ": --histogram=" << hist_arg << ": no --after given, assuming --after=" << commandline_after << '\n';
    }
  }
  if (commandline_before || commandline_after)
  {
    std::cout << "Only show/process deleted entries if they are deleted ";
    outputwritten = true;
    std::string after(std::ctime(&commandline_after));
    std::string before(std::ctime(&commandline_before));
    if (commandline_after)
      std::cout << "on or after " << after.substr(0, after.length() - 1);
    if (commandline_before && commandline_after)
      std::cout << " and ";
    if (commandline_before)
      std::cout << "before " << before.substr(0, before.length() - 1);
    std::cout << '.' << std::endl;
    if (commandline_before && commandline_after)
      assert(commandline_after < commandline_before);
  }
  if (!accepted_filenames.empty())
  {
    std::cout << "Accepted filenames:";
    for (std::set<std::string>::iterator iter = accepted_filenames.begin(); iter != accepted_filenames.end(); ++iter)
      std::cout << " '" << *iter << "'";
    outputwritten = true;
  }
  if (outputwritten)
    std::cout << '\n';

  argv += optind;
  argc -= optind;

  if (argc == 0)
  {
    print_usage(std::cerr);
    exit(EXIT_FAILURE);
  }
}

//-----------------------------------------------------------------------------
//
// dump_hex
//

static void dump_hex(unsigned char const* buf, size_t size)
{
  for (size_t addr = 0; addr < size; addr += 16)
  {
    std::cout << std::hex << std::setfill('0') << std::setw(4) << addr << " |";
    int offset;
    for (offset = 0; offset < 16 && addr + offset < size; ++offset)
      std::cout << ' ' << std::hex << std::setfill('0') << std::setw(2) << (int)buf[addr + offset];
    for (; offset < 16; ++offset)
      std::cout << "   ";
    std::cout << " | ";
    for (int offset = 0; offset < 16 && addr + offset < size; ++offset)
    {
      unsigned char c = buf[addr + offset];
      if (!std::isprint(c))
	c = '.';
      std::cout << c;
    }
    std::cout << '\n';
  }
}

//-----------------------------------------------------------------------------
//
// Printing
//

static void print_block(unsigned char* block)
{
  dump_hex(block, block_size_);
  std::cout << std::dec;
}

static void print_restrictions(void)
{
  if (commandline_allocated)
    std::cout << "Only showing entries with allocated inodes.\n";
  if (commandline_unallocated)
    std::cout << "Only showing entries with unallocated inodes.\n";
  if (commandline_deleted)
    std::cout << "Only showing entries that were deleted.\n";
  if (commandline_directory)
    std::cout << "Only showing inodes that are directories.\n";
  if (commandline_before || commandline_after)
  {
    std::cout << "Only show/process deleted entries if they are deleted ";
    if (commandline_after)
      std::cout << "on or after " << commandline_after;
    if (commandline_before && commandline_after)
      std::cout << " and ";
    if (commandline_before)
      std::cout << "before " << commandline_before;
    std::cout << '.' << std::endl;
  }
}

class FileMode {
  private:
    __le16 M_mode;
  public:
    FileMode(__le16 mode) : M_mode(mode) { } 
    friend std::ostream& operator<<(std::ostream& os, FileMode const& file_mode)
	{
	  __le16 mode(file_mode.M_mode);
	  switch ((mode & 0xf000))
	  {
	    case 0x1000:
	      os << 'p'; // "FIFO";
	      break;
	    case 0x2000:
	      os << 'c'; // "Character device";
	      break;
	    case 0x4000:
	      os << 'd'; // "Directory";
	      break;
	    case 0x6000:
	      os << 'b'; // "Block device";
	      break;
	    case 0x8000:
	      os << 'r'; // "Regular file";
	      break;
	    case 0xA000:
	      os << 'l'; // "Symbolic link";
	      break;
	    case 0xC000:
	      os << 's'; // "UNIX socket";
	      break;
	  }
	  static char const* s[4] = {
	    "rwxrwxrwx",
	    "rwsrwsrwt",
	    "---------",
	    "--S--S--T"
	  };
	  int i = 0;
	  __le16 smask = 04000;
	  for (__le16 mask = 0400; mask; mask >>= 1, ++i)
	  {
	    int k = (mode & (smask >> (i / 3))) ? 1 : 0;
	    if ((mode & mask))
	      os << s[k][i];
	    else
	      os << s[k + 2][i];
	  }
	  return os;
	}
};

static int mode_map[8] = {
  0x10000, // EXT3_FT_UNKNOWN
   0x8000, // EXT3_FT_REG_FILE
   0x4000, // EXT3_FT_DIR
   0x2000, // EXT3_FT_CHRDEV
   0x6000, // EXT3_FT_BLKDEV
   0x1000, // EXT3_FT_FIFO
   0xC000, // EXT3_FT_SOCK
   0xA000  // EXT3_FT_SYMLINK
};

static void print_inode(Inode& inode)
{
  std::cout << "Generation Id: " << inode.generation() << '\n';
  union {
    uid_t uid;
    uint16_t uid_word[2];
  };
  uid_word[0] = inode.uid_low();
  uid_word[1] = inode.uid_high();
  union {
    uid_t gid;
    uint16_t gid_word[2];
  };
  gid_word[0] = inode.gid_low();
  gid_word[1] = inode.gid_high();
  std::cout << "uid / gid: " << uid << " / " << gid << '\n';
  std::cout << "mode: " << FileMode(inode.mode()) << '\n';
  std::cout << "size: " << inode.size() << '\n';
  std::cout << "num of links: " << inode.links_count() << '\n';
  std::cout << "sectors: " << inode.blocks();
  // A sector is 512 bytes. Therefore, we are using 'inode.i_blocks * 512 / block_size_' blocks.
  // 'inode.i_size / block_size_' blocks are used for the content, thus
  // '(inode.i_blocks * 512 - inode.i_size) / block_size_' blocks should
  // be used for indirect blocks.
  if ((inode.mode() & 0xf000) != 0xa000 || inode.blocks() != 0)		// Not an inline symlink?
  {
    int number_of_indirect_blocks = (inode.blocks() * 512 - inode.size()) / block_size_;
    std::cout << " (--> " << number_of_indirect_blocks << " indirect " << ((number_of_indirect_blocks == 1) ? "block" : "blocks") << ").\n";
  }
  time_t atime = inode.atime();
  std::cout << "\nInode Times:\n";
  std::cout << "Accessed:       ";
  if (atime > 0)
    std::cout << atime << " = " << std::ctime(&atime);
  else
    std::cout << "0\n";
  time_t ctime = inode.ctime();
  std::cout << "File Modified:  ";
  if (ctime > 0)
    std::cout << ctime << " = " << std::ctime(&ctime);
  else
    std::cout << "0\n";
  time_t mtime = inode.mtime();
  std::cout << "Inode Modified: ";
  if (mtime > 0)
    std::cout << mtime << " = " << std::ctime(&mtime);
  else
    std::cout << "0\n";
  time_t dtime = inode.dtime();
  std::cout << "Deletion time:  ";
  if (dtime > 0)
    std::cout << dtime << " = " << std::ctime(&dtime);
  else
    std::cout << "0\n";
  //std::cout << "File flags: " << inode.flags() << '\n';
  if ((inode.mode() & 0xf000) != 0xa000 || inode.blocks() != 0)		// Not an inline symlink?
  {
    std::cout << "\nDirect Blocks:";
    for (int n = 0; n < EXT3_NDIR_BLOCKS; ++n)
      if (inode.block()[n])
	std::cout << ' ' << inode.block()[n];
    std::cout << '\n';
    if (inode.block()[EXT3_IND_BLOCK])
      std::cout << "Indirect Block: " << inode.block()[EXT3_IND_BLOCK] << '\n';
    if (inode.block()[EXT3_DIND_BLOCK])
      std::cout << "Double Indirect Block: " << inode.block()[EXT3_DIND_BLOCK] << '\n';
    if (inode.block()[EXT3_TIND_BLOCK])
      std::cout << "Tripple Indirect Block: " << inode.block()[EXT3_TIND_BLOCK] << '\n';
  }
  else
  {
    std::cout << "Symbolic link target name: ";
    print_symlink(std::cout, inode);
    std::cout << '\n';
  }
  //std::cout << "File ACL: " << inode.file_acl() << '\n';
  //std::cout << "Directory ACL: " << inode.dir_acl() << '\n';
  //std::cout << "Fragment address: " << inode.faddr() << '\n';
  //std::cout << "Fragment number: " << (int)inode.osd2.linux2.l_i_frag << '\n';
  //std::cout << "Fragment size: " << (int)inode.osd2.linux2.l_i_fsize << '\n';
}

char const* dir_entry_file_type(int file_type, bool ls)
{
  switch ((file_type & 7))
  {
    case EXT3_FT_UNKNOWN:
      return (ls ? "?" : "EXT3_FT_UNKNOWN");
    case EXT3_FT_REG_FILE:
      return (ls ? "r" : "EXT3_FT_REG_FILE");
    case EXT3_FT_DIR:
      return (ls ? "d" : "EXT3_FT_DIR");
    case EXT3_FT_CHRDEV:
      return (ls ? "c" : "EXT3_FT_CHRDEV");
    case EXT3_FT_BLKDEV:
      return (ls ? "b" : "EXT3_FT_BLKDEV");
    case EXT3_FT_FIFO:
      return (ls ? "p" : "EXT3_FT_FIFO");
    case EXT3_FT_SOCK:
      return (ls ? "s" : "EXT3_FT_SOCK");
    case EXT3_FT_SYMLINK:
      return (ls ? "l" : "EXT3_FT_SYMLINK");
  }
  exit(EXIT_FAILURE); // Suppress compiler warning.
}

struct Parent {
  Parent* M_parent;
  ext3_dir_entry_2 const* M_dir_entry;
  Inode const* M_inode;
  uint32_t M_inodenr;

  Parent(Inode const* inode, uint32_t inodenr) : M_parent(NULL), M_dir_entry(NULL), M_inode(inode), M_inodenr(inodenr) { }
  Parent(Parent* parent, ext3_dir_entry_2 const* dir_entry, Inode const* inode, uint32_t inodenr) :
      M_parent(parent), M_dir_entry(dir_entry), M_inode(inode), M_inodenr(inodenr) { }
  std::string dirname(bool show_inodes) const;
};

std::string Parent::dirname(bool show_inodes) const
{
  if (!M_dir_entry)
    return std::string();
  std::string path(M_dir_entry->name, M_dir_entry->name_len);
  if (show_inodes)
  {
    std::ostringstream tmp;
    tmp << '(' << M_dir_entry->inode << ')';
    path += tmp.str();
    for (Parent const* lparent = M_parent; lparent->M_dir_entry; lparent = lparent->M_parent)
    {
      tmp.str("");
      tmp << std::string(lparent->M_dir_entry->name, lparent->M_dir_entry->name_len) << '(' << lparent->M_dir_entry->inode << ')';
      path = tmp.str() + '/' + path;
    }
  }
  else
  {
    for (Parent const* lparent = M_parent; lparent->M_dir_entry; lparent = lparent->M_parent)
      path = std::string(lparent->M_dir_entry->name, lparent->M_dir_entry->name_len) + '/' + path;
  }
  return path;
}

int print_symlink(std::ostream& os, Inode& inode)
{
  uint32_t len = 0;
  if (inode.blocks() == 0)
  {
    if (inode.size() == 0)
    {
      std::cout << "<ZERO-LENGTH-SYMLINK>";
      return 0;
    }
    for (int i = 0; i < EXT3_N_BLOCKS; ++i)
    {
      union {
	char chars[4];
	__le32 block;
      } translate;
      translate.block = inode.block()[i];
      for (int j = 0; j < 4; ++j)
      {
	char c = translate.chars[j];
	assert(c != 0);
	os << c;
	if (++len == inode.size())
	  return len;
      }
    }
  }
  else
  {
    assert(inode.block()[0]);
    assert(!inode.block()[1]);			// Name can't be longer than block_size_?!
    unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
    unsigned char* block = get_block(inode.block()[0], block_buf);
    assert(block[block_size_ - 1] == '\0');	// Zero termination exists.
    len = strlen((char*)block);
    os << block;
  }
  return len;
}

bool print_dir_entry_long_action(ext3_dir_entry_2 const& dir_entry, Inode& inode,
    bool UNUSED(deleted), bool UNUSED(allocated), bool reallocated, bool zero_inode, bool linked, bool filtered, Parent*, void*)
{
  std::cout << "\ninode: " << dir_entry.inode << '\n';
  std::cout << "Directory entry length: " << dir_entry.rec_len << '\n';
  std::cout << "Name length: " << (int)dir_entry.name_len << '\n';
  std::cout << "File type: " << dir_entry_file_type(dir_entry.file_type, false);
  std::cout << "\nFile name: \"" << std::string(dir_entry.name, dir_entry.name_len) << "\"\n";
  if (!reallocated && !zero_inode && (dir_entry.file_type & 7) == EXT3_FT_SYMLINK)
  {
    std::cout << "Symbolic link to: ";
    print_symlink(std::cout, inode);
    std::cout << '\n';
  }
  std::cout << "Filtered: " << (filtered ? "Yes" : "No") << '\n';
  if (commandline_group == -1 || inode_to_group(super_block, dir_entry.inode) == commandline_group)
  {
    if (zero_inode)
      std::cout << "Inode: ZERO\n";
    else
    {
      std::cout << "\nInode:\n";
      print_inode(inode);
    }
    if (zero_inode && linked)
      std::cout << "The directory entry is linked but has a zero inode. This needs to be fixed!\n";
  }
  return false;
}

//-----------------------------------------------------------------------------
//
// Directories
// Iterating over directories
//

struct iterate_data_st {
  bool (*action)(ext3_dir_entry_2 const&, Inode&, bool, bool, bool, bool, bool, bool, Parent*, void*);
  Parent* parent;
  void* data;
  unsigned char* block_buf;

  iterate_data_st(void) : block_buf(NULL) { }
  ~iterate_data_st() { if (block_buf) delete [] block_buf; }
};

static int depth;
bool read_block_action(ext3_dir_entry_2 const& dir_entry, Inode& inode,
    bool deleted, bool allocated, bool reallocated, bool zero_inode, bool linked, bool filtered, Parent* parent, void* data);
bool init_directories_action(ext3_dir_entry_2 const& dir_entry, Inode&, bool, bool, bool, bool, bool, bool, Parent* parent, void*);

static void filter_dir_entry(ext3_dir_entry_2 const& dir_entry,
                             bool deleted, bool linked,
			     bool (*action)(ext3_dir_entry_2 const&, Inode&, bool, bool, bool, bool, bool, bool, Parent*, void*),
			     Parent* parent, void* data)
{
  Inode* inode = NULL;
  int file_type = (dir_entry.file_type & 7);
  bool zero_inode = (dir_entry.inode == 0);
  bool filtered = (zero_inode && !commandline_zeroed_inodes);
  bool allocated = false;
  bool reallocated = false;
  if (!zero_inode)
  {
    inode = &get_inode(dir_entry.inode);
    allocated = is_allocated(dir_entry.inode);
    reallocated = (deleted && allocated) || (deleted && inode->dtime() == 0) || (mode_map[file_type] != (inode->mode() & 0xf000));
    deleted = deleted || inode->dtime();
    // Block pointers are erased on ext3 on deletion (that is the whole point of writing this tool!),
    // however - in the case of symlinks, the name of the symlink is (still) in this place.
    if (!is_symlink(*inode) && inode->dtime() != 0 && inode->block()[0] != 0)
    {
      time_t dtime = inode->dtime();
      std::string dtime_str(std::ctime(&dtime));
      std::cout << "WARNING: Inode " << dir_entry.inode << " has non-zero dtime (" << inode->dtime() <<
	  "  " << dtime_str.substr(0, dtime_str.length() - 1) << ") but non-zero block list (" << inode->block()[0] << ").\n";
    }
    filtered = !(\
	(!commandline_allocated || allocated) &&
	(!commandline_unallocated || !allocated) &&
	(!commandline_deleted || deleted) &&
	(!commandline_directory || file_type == EXT3_FT_DIR) &&
	(!reallocated || commandline_reallocated) &&
	(reallocated || (inode->dtime() == 0 && !commandline_deleted) ||
	       (commandline_after <= (time_t)inode->dtime() && (!commandline_before || (time_t)inode->dtime() < commandline_before))));
  }
  if (no_filtering)	// Also no recursion.
    action(dir_entry, *inode, deleted, allocated, reallocated, zero_inode, linked, filtered, parent, data);
  else if (!filtered)
  {
    if (action(dir_entry, *inode, deleted, allocated, reallocated, zero_inode, linked, filtered, parent, data))
      return;	// Recursion aborted.
    // Handle recursion.
    if (parent && file_type == EXT3_FT_DIR && depth < commandline_depth)
    {
      // Skip "." and ".." when iterating recursively.
      if ((dir_entry.name_len == 1 && dir_entry.name[0] == '.') ||
	  (dir_entry.name_len == 2 && dir_entry.name[0] == '.' && dir_entry.name[1] == '.'))
        return;
      iterate_data_st idata;
      idata.action = action;
      idata.data = data;
      Parent new_parent(parent, &dir_entry, inode, dir_entry.inode);
      idata.parent = &new_parent;
      // Break possible loops as soon as we see an inode number that we encountered before.
      static std::vector<uint32_t> inodes(64);
      if (inodes.size() < (size_t)depth + 1)
        inodes.resize(inodes.size() * 2);
      for (int d = 1; d < depth; ++d)
      {
        if (inodes[d] == dir_entry.inode)
	{
	  std::cout << "Detected loop for inode " << dir_entry.inode << " (" << idata.parent->dirname(commandline_show_path_inodes) << ").\n";
	  return;
	}
      }
      inodes[depth] = dir_entry.inode;
      ++depth;
      if (!deleted && allocated && !reallocated)	// Existing directory?
      {
	bool reused_or_corrupted_indirect_block3 = iterate_over_all_blocks_of(get_inode(dir_entry.inode), iterate_over_existing_directory_action, &idata);
	assert(!reused_or_corrupted_indirect_block3);
      }
      else
      {
        // We only know the first block, but that is enough to construct the directory tree.
	int blocknr = dir_inode_to_block(dir_entry.inode);
	if (blocknr != -1)
	{
	  // There could be loops if we linked the wrong directory to an inode.
	  // In any case we have to break those loops. Try to be smart about it:

	  // Find the dtime of the parent, or a parent of the parent.
	  uint32_t dtime = 0;
	  Parent* parent_iter = parent;
          while (!dtime)
	  {
	    if (!parent_iter)
	      break;
	    dtime = parent_iter->M_inode->dtime();
	    parent_iter = parent_iter->M_parent;
	  }
	  // It turns out that a parent can be time-stamped as deleted before
	  // it's subdirectories when using rm -rf (?). Allow for 60 seconds
	  // of time difference.
	  if (!dtime || dtime + 60 >= inode->dtime())
	  {
	    // Now, before actually processing this new directory, check if the inode it contains for ".." is equal to the inode
	    // of it's parent directory!
	    idata.block_buf = new unsigned char [block_size_];
	    get_block(blocknr, idata.block_buf);
	    ext3_dir_entry_2* dir_entry = reinterpret_cast<ext3_dir_entry_2*>(idata.block_buf);
	    assert(dir_entry->name_len == 1 && dir_entry->name[0] == '.');
	    dir_entry = reinterpret_cast<ext3_dir_entry_2*>(idata.block_buf + dir_entry->rec_len);
	    assert(dir_entry->name_len == 2 && dir_entry->name[0] == '.' && dir_entry->name[1] == '.');
	    if (dir_entry->inode == parent->M_inodenr)
	      iterate_over_directory_action(blocknr, &idata);
	    else
	      std::cout << "The directory \"" << idata.parent->dirname(commandline_show_path_inodes) << "\" is lost.\n";
	  }
	}
	else
	  std::cout << "Cannot find a directory block for inode " << dir_entry.inode << ".\n";
      }
      --depth;
    }
  }
}

static void iterate_over_directory_action(int blocknr, void* data)
{
  iterate_data_st* idata = reinterpret_cast<iterate_data_st*>(data);
  iterate_over_directory(idata->block_buf, blocknr, idata->action, idata->parent, idata->data);
}

static void iterate_over_existing_directory_action(int blocknr, void* data)
{
  iterate_data_st* idata = reinterpret_cast<iterate_data_st*>(data);
  unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  get_block(blocknr, block_buf);
  iterate_over_directory(block_buf, blocknr, idata->action, idata->parent, idata->data);
}

static void iterate_over_directory(unsigned char* block, int blocknr,
    bool (*action)(ext3_dir_entry_2 const&, Inode&, bool, bool, bool, bool, bool, bool, Parent*, void*), Parent* parent, void* data)
{
  ext3_dir_entry_2 const* dir_entry;
  ext3_dir_entry_2 const* map[EXT3_MAX_BLOCK_SIZE / EXT3_DIR_PAD];
  std::memset(map, 0, sizeof(map));

  if (action == read_block_action)
    ++no_filtering;

  int offset = 0;
  while (offset < block_size_)
  {
    dir_entry = reinterpret_cast<ext3_dir_entry_2 const*>(block + offset);
    filter_dir_entry(*dir_entry, false, true, action, parent, data);
    map[offset / EXT3_DIR_PAD] = dir_entry;
    offset += dir_entry->rec_len;
  }

  // Search for deleted entries.
  offset = block_size_ - EXT3_DIR_REC_LEN(1);
  while (offset > 0)
  {
    dir_entry = reinterpret_cast<ext3_dir_entry_2 const*>(block + offset);
    if (!map[offset / EXT3_DIR_PAD])
    {
      if (is_directory(block, blocknr, false, false, offset))
        filter_dir_entry(*dir_entry, true, false, action, parent, data);
    }
    offset -= EXT3_DIR_PAD;
  }

  if (action == read_block_action)
    --no_filtering;
}

class DirectoryBlock;
class Directory;

struct Index {
  int cur;	// Indicates the order in memory.
  int next;	// The index of the DirEntry that ext3_dir_entry_2::rec_len refers to or zero if it refers to the end.
};

struct DirEntry {
  std::list<DirectoryBlock>::const_iterator M_directory_iterator;	// Pointer to DirectoryBlock containing this entry.
  Directory* M_directory;						// Pointer to Directory, if this is a directory.
  int M_file_type;							// The dir entry file type.
  int M_inode;								// The inode referenced by this DirEntry.
  std::string M_name;							// The file name of this DirEntry.
  union {
    ext3_dir_entry_2 const* dir_entry;					// Temporary pointer into block_buf.
    Index index;							// Ordering index of dir entry.
  };
  bool deleted;								// Copies of values calculated by filter_dir_entry.
  bool allocated;
  bool reallocated;
  bool zero_inode;
  bool linked;
  bool filtered;

  bool exactly_equal(DirEntry const& de) const;
  void print(void) const;
};

bool DirEntry::exactly_equal(DirEntry const& de) const
{
  assert(index.cur == de.index.cur);
  return M_inode == de.M_inode && M_name == de.M_name && M_file_type == de.M_file_type && index.next == de.index.next;
}

class DirectoryBlock {
  private:
    int M_block;
    std::vector<DirEntry> M_dir_entry;
  public:
    void read_block(int block, std::list<DirectoryBlock>::iterator iter);
    void read_dir_entry(ext3_dir_entry_2 const& dir_entry, Inode& inode,
        bool deleted, bool allocated, bool reallocated, bool zero_inode, bool linked, bool filtered, std::list<DirectoryBlock>::iterator iter);

    bool exactly_equal(DirectoryBlock const& dir) const;
    int block(void) const { return M_block; }
    void print(void) const;

    std::vector<DirEntry> const& dir_entries(void) const { return M_dir_entry; }
    std::vector<DirEntry>& dir_entries(void) { return M_dir_entry; }
};

bool DirectoryBlock::exactly_equal(DirectoryBlock const& dir) const
{
  if (M_dir_entry.size() != dir.M_dir_entry.size())
    return false;
  std::vector<DirEntry>::const_iterator iter1 = M_dir_entry.begin();
  std::vector<DirEntry>::const_iterator iter2 = dir.M_dir_entry.begin();
  for (;iter1 != M_dir_entry.end(); ++iter1, ++iter2)
    if (!iter1->exactly_equal(*iter2))
      return false;
  return true;
}

bool read_block_action(ext3_dir_entry_2 const& dir_entry, Inode& inode,
    bool deleted, bool allocated, bool reallocated, bool zero_inode, bool linked, bool filtered, Parent*, void* data)
{
  std::list<DirectoryBlock>::iterator* iter_ptr = reinterpret_cast<std::list<DirectoryBlock>::iterator*>(data);
  DirectoryBlock* directory = &**iter_ptr;
  directory->read_dir_entry(dir_entry, inode, deleted, allocated, reallocated, zero_inode, linked, filtered, *iter_ptr);
  return false;
}

void DirectoryBlock::read_dir_entry(ext3_dir_entry_2 const& dir_entry, Inode& UNUSED(inode),
    bool deleted, bool allocated, bool reallocated, bool zero_inode, bool linked, bool filtered, std::list<DirectoryBlock>::iterator iter)
{
  DirEntry new_dir_entry;
  new_dir_entry.M_directory_iterator = iter;
  new_dir_entry.M_directory = NULL;
  new_dir_entry.M_file_type = dir_entry.file_type & 7;	// Only the last 3 bits are used.
  new_dir_entry.M_inode = dir_entry.inode;
  new_dir_entry.M_name = std::string(dir_entry.name, dir_entry.name_len);
  new_dir_entry.dir_entry = &dir_entry;		// This points directy into the block_buf that we are processing.
  						// It will be replaced with the indices before that buffer is destroyed.
  new_dir_entry.deleted = deleted;
  new_dir_entry.allocated = allocated;
  new_dir_entry.reallocated = reallocated;
  new_dir_entry.zero_inode = zero_inode;
  new_dir_entry.linked = linked;
  new_dir_entry.filtered = filtered;
  M_dir_entry.push_back(new_dir_entry);
}

struct DirEntrySortPred {
  bool operator()(DirEntry const& de1, DirEntry const& de2) const { return de1.dir_entry < de2.dir_entry; }
};

void DirectoryBlock::read_block(int block, std::list<DirectoryBlock>::iterator list_iter)
{
  M_block = block;
  static bool using_static_buffer = false;
  assert(!using_static_buffer);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  get_block(block, block_buf);
  using_static_buffer = true;
  iterate_over_directory(block_buf, block, read_block_action, NULL, &list_iter);
  // Sort the vector by dir_entry pointer.
  std::sort(M_dir_entry.begin(), M_dir_entry.end(), DirEntrySortPred());
  int size = M_dir_entry.size();
  assert(size >= 2);	// We should have at least the '.' and '..' entries.
  // Make a temporary backup of the dir_entry pointers.
  // At the same time, overwrite the pointers in the vector with with the index.
  ext3_dir_entry_2 const** index_to_dir_entry = new ext3_dir_entry_2 const* [M_dir_entry.size()];
  int i = 0;
  for (std::vector<DirEntry>::iterator iter = M_dir_entry.begin(); iter != M_dir_entry.end(); ++iter, ++i)
  {
    index_to_dir_entry[i] = iter->dir_entry;
    iter->index.cur = i;
  }
  // Assign a value to index.next, if any.
  for (std::vector<DirEntry>::iterator iter = M_dir_entry.begin(); iter != M_dir_entry.end(); ++iter)
  {
    ext3_dir_entry_2 const* dir_entry = index_to_dir_entry[iter->index.cur];
    ext3_dir_entry_2 const* next_dir_entry = (ext3_dir_entry_2 const*)(reinterpret_cast<char const*>(dir_entry) + dir_entry->rec_len);
    int next = 0;
    for (int j = 0; j < size; ++j)
      if (index_to_dir_entry[j] == next_dir_entry)
      {
        next = j;
        break;
      }
    // Either this entry points to another that we found, or it should point to the end of this block.
    assert(next > 0 || (unsigned char*)next_dir_entry == block_buf + block_size_);
    // If we didn't find anything, use the value 0.
    iter->index.next = next;
  }
  delete [] index_to_dir_entry;
  using_static_buffer = false;
}

class Directory {
  private:
    uint32_t M_inode_number;
    std::list<DirectoryBlock> M_blocks;

  public:
    Directory(uint32_t inode_number) : M_inode_number(inode_number) { }
    Directory(uint32_t inode_number, int first_block);

  std::list<DirectoryBlock>& blocks(void) { return M_blocks; }
  std::list<DirectoryBlock> const& blocks(void) const { return M_blocks; }

  uint32_t inode_number(void) const { return M_inode_number; }
  int first_block(void) const { assert(!M_blocks.empty()); return M_blocks.begin()->block(); }
};

//-----------------------------------------------------------------------------
//
// Directory printing
//

void DirEntry::print(void) const
{
  if (filtered)
    return;
  std::cout << std::setfill(' ') << std::setw(4) << index.cur << ' ';
  if (index.next)
    std::cout << std::setfill(' ') << std::setw(4) << index.next << ' ';
  else
    std::cout << " end ";
  std::cout << dir_entry_file_type(M_file_type, true);
  std::cout << std::setfill(' ') << std::setw(8) << M_inode << "  ";
  std::cout << (zero_inode ? 'Z' : deleted ? reallocated ? 'R' : 'D' : ' ');
  Inode* inode = NULL;
  if (!zero_inode)
  {
    inode = &get_inode(M_inode);
    if (deleted && !reallocated)
    {
      time_t dtime = inode->dtime();
      std::string dtime_str(ctime(&dtime));
      std::cout << ' ' << std::setw(10) << inode->dtime() << ' ' << dtime_str.substr(0, dtime_str.length() - 1);
    }
  }
  if (zero_inode && linked)
    std::cout << " * LINKED ENTRY WITH ZERO INODE *   ";
  else if (zero_inode || !deleted || reallocated)
    std::cout << std::string(36, ' ');
  if (zero_inode || reallocated)
    std::cout << "  ??????????";
  else
    std::cout << "  " << FileMode(inode->mode());
  std::cout << "  " << M_name;
  if (!(reallocated || zero_inode) && M_file_type == EXT3_FT_SYMLINK)
  {
    std::cout << " -> ";
    print_symlink(std::cout, *inode);
  }
  std::cout << '\n';
}

void DirectoryBlock::print(void) const
{
  for (std::vector<DirEntry>::const_iterator iter = M_dir_entry.begin(); iter != M_dir_entry.end(); ++iter)
    iter->print();
}

static void print_directory(unsigned char* block, int blocknr)
{
  depth = 1;
  if (commandline_ls)
  {
    std::cout << "          .-- File type in dir_entry (r=regular file, d=directory, l=symlink)\n";
    std::cout << "          |          .-- D: Deleted ; R: Reallocated\n";
    std::cout << "Indx Next |  Inode   | Deletion time                        Mode        File name\n";
    std::cout << "==========+==========+----------------data-from-inode------+-----------+=========\n";
    std::list<DirectoryBlock> db(1);
    db.begin()->read_block(blocknr, db.begin());
    db.begin()->print();
    std::cout << '\n';
  }
  else
  {
    ++no_filtering;
    iterate_over_directory(block, blocknr, print_dir_entry_long_action, NULL, NULL);
    --no_filtering;
  }
}

//-----------------------------------------------------------------------------
//
// Histogram
//

int const histsize = 100;
static size_t S_min;
static size_t S_max;
static size_t S_bs;
static int histo[histsize];
static int S_maxcount;

static void hist_init(size_t min, size_t max)
{
  S_min = min;
  S_max = max;

  assert(max > min);

  S_bs = 1;
  while ((max - 1 - min) / S_bs > histsize - 1)
    ++S_bs;
  std::memset(histo, 0, sizeof(histo));
  S_maxcount = 0;
}

static void hist_add(size_t val)
{
  assert(val >= S_min && val < S_max);
  histo[(val - S_min) / S_bs] += 1;
  S_maxcount = std::max(S_maxcount, histo[(val - S_min) / S_bs]);
}

static void hist_print(void)
{
  if (S_maxcount == 0)
  {
    std::cout << "No counts\n";
    return;
  }
  static char const line[] = "===============================================================================================================================================================END!";
  int i = 0;
  size_t total_count = 0;
  for (size_t val = S_min;; val += S_bs, ++i)
  {
    if (commandline_histogram == hist_atime ||
        commandline_histogram == hist_ctime ||
	commandline_histogram == hist_mtime ||
	commandline_histogram == hist_dtime)
    {
      time_t time_val = val;
      std::string time_str(ctime(&time_val));
      std::cout << time_str.substr(0, time_str.length() - 1) << "  ";
    }
    std::cout << std::setfill(' ') << std::setw(8) << val << ' ';
    if (val >= S_max)
      break;
    std::cout << std::setfill(' ') << std::setw(8) << histo[i] << ' ';
    std::streamsize tower = static_cast<std::streamsize>(histo[i] * 100.0 / S_maxcount);
    std::cout.write(line, tower);
    std::cout << '\n';
    total_count += histo[i];
  }
  std::cout << "\nTotals:\n";
  std::cout << std::setw(8) << S_min << " - " << std::setfill(' ') << std::setw(8) << (S_max - 1) << ' ';
  std::cout << std::setfill(' ') << std::setw(8) << total_count << '\n';
}

//-----------------------------------------------------------------------------
//
// Journal
//

class Descriptor;
static void add_block_descriptor(uint32_t block, Descriptor*);
static void add_block_in_journal_descriptor(Descriptor* descriptor);

enum descriptor_type_nt {
  dt_unknown,
  dt_tag,
  dt_revoke,
  dt_commit
};

std::ostream& operator<<(std::ostream& os, descriptor_type_nt descriptor_type)
{
  switch (descriptor_type)
  {
    case dt_unknown:
      os << "*UNKNOWN*";
      break;
    case dt_tag:
      os << "TAG";
      break;
    case dt_revoke:
      os << "REVOKE";
      break;
    case dt_commit:
      os << "COMMIT";
      break;
  }
  return os;
}

class Descriptor {
  private:
    uint32_t const M_block;			// Block number in the journal.
    uint32_t const M_sequence;
  public:
    Descriptor(uint32_t block, uint32_t sequence) : M_block(block), M_sequence(sequence) { }
    uint32_t block(void) const { return M_block; }
    uint32_t sequence(void) const { return M_sequence; }
    virtual descriptor_type_nt descriptor_type(void) const = 0;
    virtual void print_blocks(void) const = 0;
    virtual void add_block_descriptors(void) = 0;
  protected:
    virtual ~Descriptor() { }
};

class DescriptorTag : public Descriptor {
  private:
    uint32_t M_blocknr;		// Block number on the file system.
    uint32_t M_flags;
  public:
    DescriptorTag(uint32_t block, uint32_t sequence, journal_block_tag_t* block_tag) :
        Descriptor(block, sequence), M_blocknr(be2le(block_tag->t_blocknr)), M_flags(be2le(block_tag->t_flags)) { }
    virtual descriptor_type_nt descriptor_type(void) const { return dt_tag; }
    virtual void print_blocks(void) const;
    virtual void add_block_descriptors(void) { add_block_descriptor(M_blocknr, this); add_block_in_journal_descriptor(this); }
    uint32_t block(void) const { return M_blocknr; }
};

void DescriptorTag::print_blocks(void) const
{
  std::cout << ' ' << Descriptor::block() << '=' << M_blocknr;
  if ((M_flags & (JFS_FLAG_ESCAPE|JFS_FLAG_DELETED)))
  {
    std::cout << '(';
    uint32_t flags = M_flags;
    if ((flags & JFS_FLAG_ESCAPE))
    {
      std::cout << "ESCAPED";
      flags &= ~JFS_FLAG_ESCAPE;
    }
    if (flags)
      std::cout << '|';
    if ((flags & JFS_FLAG_DELETED))
    {
      std::cout << "DELETED";
      flags &= ~JFS_FLAG_DELETED;
    }
    std::cout << ')';
  }
}

class DescriptorRevoke : public Descriptor {
  private:
    std::vector<uint32_t> M_blocks;
  public:
    DescriptorRevoke(uint32_t block, uint32_t sequence, journal_revoke_header_t* revoke_header);
    virtual descriptor_type_nt descriptor_type(void) const { return dt_revoke; }
    virtual void print_blocks(void) const;
    virtual void add_block_descriptors(void);
};

void DescriptorRevoke::add_block_descriptors(void)
{
  for (std::vector<uint32_t>::iterator iter = M_blocks.begin(); iter != M_blocks.end(); ++iter)
    add_block_descriptor(*iter, this);
  add_block_in_journal_descriptor(this);
}

DescriptorRevoke::DescriptorRevoke(uint32_t block, uint32_t sequence, journal_revoke_header_t* revoke_header) : Descriptor(block, sequence)
{
  uint32_t count = be2le(revoke_header->r_count);
  assert(sizeof(journal_revoke_header_t) <= count && count <= (size_t)block_size_);
  count -= sizeof(journal_revoke_header_t);
  assert(count % sizeof(__be32) == 0);
  count /= sizeof(__be32);
  __be32* ptr = reinterpret_cast<__be32*>((unsigned char*)revoke_header + sizeof(journal_revoke_header_t));
  for (uint32_t b = 0; b < count; ++b)
    M_blocks.push_back(be2le(ptr[b]));
}

void DescriptorRevoke::print_blocks(void) const
{
  for (std::vector<uint32_t>::const_iterator iter = M_blocks.begin(); iter != M_blocks.end(); ++iter)
    std::cout << ' ' << *iter;
}

class DescriptorCommit : public Descriptor {
  public:
    DescriptorCommit(uint32_t block, uint32_t sequence) : Descriptor(block, sequence) { }
    virtual descriptor_type_nt descriptor_type(void) const { return dt_commit; }
    virtual void print_blocks(void) const { }
    virtual void add_block_descriptors(void) { add_block_in_journal_descriptor(this); };
};

class Transaction {
  private:
    int M_block;
    int M_sequence;
    bool M_committed;
    std::vector<Descriptor*> M_descriptor;
  public:
    void init(int block, int sequence) { M_block = block; M_sequence = sequence; M_committed = false; }
    void set_committed(void) { assert(!M_committed); M_committed = true; }
    void append(Descriptor* descriptor) { M_descriptor.push_back(descriptor); }

    void print_descriptors(void) const;
    int block(void) const { return M_block; }
    int sequence(void) const { return M_sequence; }
    bool committed(void) const { return M_committed; }
    bool contains_tag_for_block(int block);
};

bool Transaction::contains_tag_for_block(int block)
{
  for (std::vector<Descriptor*>::iterator iter = M_descriptor.begin(); iter != M_descriptor.end(); ++iter)
  {
    if ((*iter)->descriptor_type() == dt_tag)
    {
      DescriptorTag& tag(*static_cast<DescriptorTag*>(*iter));
      if (tag.block() == (uint32_t)block)
        return true;
    }
  }
  return false;
}

void Transaction::print_descriptors(void) const
{
  descriptor_type_nt dt = dt_unknown;
  for (std::vector<Descriptor*>::const_iterator iter = M_descriptor.begin(); iter != M_descriptor.end(); ++iter)
  {
    if ((*iter)->descriptor_type() != dt)
    {
      if (dt != dt_unknown)
	std::cout << '\n';
      dt = (*iter)->descriptor_type();
      std::cout << dt << ':';
    }
    (*iter)->print_blocks();
  }
  std::cout << '\n';
}

std::vector<Descriptor*> all_descriptors;
typedef std::map<int, Transaction> sequence_transaction_map_type;
sequence_transaction_map_type sequence_transaction_map;
typedef std::map<int, std::vector<Descriptor*> > block_to_descriptors_map_type;
block_to_descriptors_map_type block_to_descriptors_map;
typedef std::map<int, Descriptor*> block_in_journal_to_descriptors_map_type;
block_in_journal_to_descriptors_map_type block_in_journal_to_descriptors_map;
typedef std::map<int, int> block_to_dir_inode_map_type;
block_to_dir_inode_map_type block_to_dir_inode_map;

static unsigned int number_of_descriptors;
static uint32_t min_sequence;
static uint32_t max_sequence;

static void add_block_descriptor(uint32_t block, Descriptor* descriptor)
{
  block_to_descriptors_map_type::iterator iter = block_to_descriptors_map.find(block);
  if (iter == block_to_descriptors_map.end())
  {
    std::pair<block_to_descriptors_map_type::iterator, bool> res =
        block_to_descriptors_map.insert(block_to_descriptors_map_type::value_type(block, std::vector<Descriptor*>()));
    assert(res.second);
    res.first->second.push_back(descriptor);
  }
  else
    iter->second.push_back(descriptor);
}

static void add_block_in_journal_descriptor(Descriptor* descriptor)
{
  std::pair<block_in_journal_to_descriptors_map_type::iterator, bool> res =
      block_in_journal_to_descriptors_map.insert(block_in_journal_to_descriptors_map_type::value_type(descriptor->block(), descriptor));
  assert(res.second);
}

static void print_block_descriptors(uint32_t block)
{
  block_to_descriptors_map_type::iterator iter = block_to_descriptors_map.find(block);
  if (iter == block_to_descriptors_map.end())
  {
    std::cout << "There are no descriptors in the journal referencing block " << block << ".\n";
    return;
  }
  std::cout << "Journal descriptors referencing block " << block << ":\n";
  for (std::vector<Descriptor*>::iterator iter2 = iter->second.begin(); iter2 != iter->second.end(); ++iter2)
  {
    std::cout << (*iter2)->sequence() << ' ' << (*iter2)->block() << '\n';
  }
}

static uint32_t find_largest_journal_sequence_number(int block)
{
  block_to_descriptors_map_type::iterator iter = block_to_descriptors_map.find(block);
  if (iter == block_to_descriptors_map.end())
    return 0;
  return (*iter->second.rbegin())->sequence();
}

bool action_tag_count(uint32_t, uint32_t sequence, journal_block_tag_t*, void*)
{
  min_sequence = std::min(sequence, min_sequence);
  max_sequence = std::max(sequence, max_sequence);
  ++number_of_descriptors;
  return false;
}

bool action_revoke_count(uint32_t, uint32_t sequence, journal_revoke_header_t*, void*)
{
  min_sequence = std::min(sequence, min_sequence);
  max_sequence = std::max(sequence, max_sequence);
  ++number_of_descriptors;
  return false;
}

bool action_commit_count(uint32_t, uint32_t sequence, void*)
{
  min_sequence = std::min(sequence, min_sequence);
  max_sequence = std::max(sequence, max_sequence);
  ++number_of_descriptors;
  return false;
}

void count_descriptors(void)
{
  number_of_descriptors = 0;
  min_sequence = 0xffffffff;
  max_sequence = 0;
  iterate_over_journal(action_tag_count, action_revoke_count, action_commit_count, NULL);
}

bool action_tag_fill(uint32_t block, uint32_t sequence, journal_block_tag_t* block_tag, void* data)
{
  uint32_t& descriptor_count = *reinterpret_cast<uint32_t*>(data);
  all_descriptors[descriptor_count++] = new DescriptorTag(block, sequence, block_tag);
  return false;
}

bool action_revoke_fill(uint32_t block, uint32_t sequence, journal_revoke_header_t* revoke_header, void* data)
{
  uint32_t& descriptor_count = *reinterpret_cast<uint32_t*>(data);
  all_descriptors[descriptor_count++] = new DescriptorRevoke(block, sequence, revoke_header);
  return false;
}

bool action_commit_fill(uint32_t block, uint32_t sequence, void* data)
{
  uint32_t& descriptor_count = *reinterpret_cast<uint32_t*>(data);
  all_descriptors[descriptor_count++] = new DescriptorCommit(block, sequence);
  return false;
}

struct AllDescriptorsPred {
  bool operator()(Descriptor* d1, Descriptor* d2) const { return d1->sequence() < d2->sequence(); }
};

static int smallest_block_nr;
static int largest_block_nr;
static uint64_t* journal_block_bitmap = NULL;
static int min_journal_block;
static int max_journal_block;		// One more than largest block belonging to the journal.
static uint64_t* is_indirect_block_in_journal_bitmap = NULL;

void find_blocknr_range_action(int blocknr, void*)
{
  if (blocknr > largest_block_nr)
    largest_block_nr = blocknr;
  if (blocknr < smallest_block_nr)
    smallest_block_nr = blocknr;
}

void fill_journal_bitmap_action(int blocknr, void*)
{
  struct bitmap_ptr bmp = get_bitmap_mask(blocknr - min_journal_block);
  journal_block_bitmap[bmp.index] |= bmp.mask;
}

void indirect_journal_block_action(int blocknr, void*)
{
  struct bitmap_ptr bmp = get_bitmap_mask(blocknr - min_journal_block);
  is_indirect_block_in_journal_bitmap[bmp.index] |= bmp.mask;
}

void directory_inode_action(int blocknr, void* data)
{
  int inode_number = *reinterpret_cast<int*>(data);
  block_to_dir_inode_map_type::iterator iter = block_to_dir_inode_map.find(blocknr);
  if (iter == block_to_dir_inode_map.end())
    block_to_dir_inode_map.insert(block_to_dir_inode_map_type::value_type(blocknr, inode_number));
  else
    iter->second = inode_number;	// We're called with ascending sequence numbers. Therefore, keep the last.
}

static void init_journal(void)
{
  // Determine which blocks belong to the journal.
  assert(is_allocated(super_block.s_journal_inum));	// Maybe this is the way to detect external journals?
  Inode& journal_inode = get_inode(super_block.s_journal_inum);
  // Find the block range used by the journal.
  smallest_block_nr = block_count(super_block);
  largest_block_nr = 0;
  bool reused_or_corrupted_indirect_block4 = iterate_over_all_blocks_of(journal_inode, find_blocknr_range_action, NULL, indirect_bit | direct_bit);
  assert(!reused_or_corrupted_indirect_block4);
  assert(smallest_block_nr < largest_block_nr);		// A non-external journal must have a size.
  min_journal_block = smallest_block_nr;
  max_journal_block = largest_block_nr + 1;
  std::cout << "Minimum / maximum journal block: " << min_journal_block << " / " << max_journal_block << '\n';
  // Allocate and fill the bitmaps.
  int size = (max_journal_block - min_journal_block + 63) / 64;
  is_indirect_block_in_journal_bitmap = new uint64_t [size];
  memset(is_indirect_block_in_journal_bitmap, 0, size * sizeof(uint64_t));
  bool reused_or_corrupted_indirect_block5 = iterate_over_all_blocks_of(journal_inode, indirect_journal_block_action, NULL, indirect_bit);
  assert(!reused_or_corrupted_indirect_block5);
  journal_block_bitmap = new uint64_t [size];
  memset(journal_block_bitmap, 0, size * sizeof(uint64_t));
  bool reused_or_corrupted_indirect_block6 = iterate_over_all_blocks_of(journal_inode, fill_journal_bitmap_action, NULL, indirect_bit | direct_bit);
  assert(!reused_or_corrupted_indirect_block6);
  // Initialize the Descriptors.
  std::cout << "Loading journal descriptors..." << std::flush;
  wrapped_journal_sequence = 0;
  count_descriptors();
  all_descriptors.clear();
  all_descriptors.resize(number_of_descriptors);
  uint32_t descriptor_count = 0;
  iterate_over_journal(action_tag_fill, action_revoke_fill, action_commit_fill, &descriptor_count);
  assert(all_descriptors.size() == number_of_descriptors);
  assert(number_of_descriptors == 0 || all_descriptors[number_of_descriptors - 1]->descriptor_type() != dt_unknown);
  // Sort the descriptors in ascending sequence number.
  std::sort(all_descriptors.begin(), all_descriptors.end(), AllDescriptorsPred());
  for (std::vector<Descriptor*>::iterator iter = all_descriptors.begin(); iter != all_descriptors.end(); ++iter)
  {
    int sequence = (*iter)->sequence();
    std::pair<sequence_transaction_map_type::iterator, bool> res =
        sequence_transaction_map.insert(sequence_transaction_map_type::value_type(sequence, Transaction()));
    switch((*iter)->descriptor_type())
    {
      case dt_tag:
      case dt_revoke:
        if (res.second)						// Did we just create this Transaction object?
	  res.first->second.init((*iter)->block(), sequence);	// Initialize it.
	res.first->second.append(*iter);
	(*iter)->add_block_descriptors();
        break;
      case dt_commit:
        if (res.second)						// Did we just create this Transaction object?
	  sequence_transaction_map.erase(res.first);		// We're not interested in a descriptor that exists of only a commit block.
	  							// FIXME: could be a wrapped around commit.
        else
	  res.first->second.set_committed();
        break;
      case dt_unknown:
        assert((*iter)->descriptor_type() != dt_unknown);	// Fail; this should really never happen.
	break;
    }
  }
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  Inode* inode = reinterpret_cast<Inode*>(block_buf);
  // Run over all descriptors, in increasing sequence number.
  for (std::vector<Descriptor*>::iterator iter = all_descriptors.begin(); iter != all_descriptors.end(); ++iter)
  {
    // Skip non-tags.
    if ((*iter)->descriptor_type() != dt_tag)
      continue;
    DescriptorTag* tag = static_cast<DescriptorTag*>(*iter);
    // Only process those that contain inodes.
    uint32_t block_nr = tag->block();
    if (is_inode(block_nr))
    {
      int inode_number = block_to_inode(block_nr);
      // Run over all inodes in the journal block.
      get_block(tag->Descriptor::block(), block_buf);
      for (unsigned int i = 0; i < block_size_ / sizeof(Inode); ++i, ++inode_number)
      {
        // Skip non-directories.
	if (!is_directory(inode[i]))
	  continue;
        // Skip deleted inodes.
        if (inode[i].dtime() != 0 || inode[i].atime() == 0 || inode[i].block()[0] == 0)
	  continue;
	// Run over all blocks of the directory inode.
	bool reused_or_corrupted_indirect_block7 = iterate_over_all_blocks_of(inode[i], directory_inode_action, &inode_number);
	if (reused_or_corrupted_indirect_block7)
	{
	  std::cout << "Note: Block " << tag->Descriptor::block() << " in the journal contains a copy of inode " << inode_number <<
	      " which is a directory, but this directory has reused or corrupted (double/triple) indirect blocks.\n";
	}
      }
    }
  }
  std::cout << " done\n";
  if (wrapped_journal_sequence)
  {
    static bool printed = false;
    if (!printed)
    {
      printed = true;
      std::cout << "Journal transaction " << wrapped_journal_sequence << " wraps around, some data blocks might have been lost of this transaction.\n";
    }
  }
  std::cout << "Number of descriptors in journal: " << number_of_descriptors << "; min / max sequence numbers: " << min_sequence << " / " << max_sequence << '\n';
}

static bool is_in_journal(int blocknr)
{
  if (!journal_block_bitmap)
    init_journal();
  return blocknr >= min_journal_block && blocknr < max_journal_block;
}

static bool is_journal(int blocknr)
{
  if (super_block.s_journal_dev)
  {
    assert(!commandline_journal);
    return false;
  }
  if (!is_in_journal(blocknr))
    return false;
  struct bitmap_ptr bmp = get_bitmap_mask(blocknr - min_journal_block);
  return (journal_block_bitmap[bmp.index] & bmp.mask);
}

static bool is_indirect_block_in_journal(int blocknr)
{
  assert(is_indirect_block_in_journal_bitmap);
  if (blocknr >= max_journal_block || blocknr < min_journal_block)
    return false;
  struct bitmap_ptr bmp = get_bitmap_mask(blocknr - min_journal_block);
  return (is_indirect_block_in_journal_bitmap[bmp.index] & bmp.mask);
}

static int journal_block_contains_inodes(int blocknr)
{
  block_in_journal_to_descriptors_map_type::iterator iter = block_in_journal_to_descriptors_map.find(blocknr);
  if (iter == block_in_journal_to_descriptors_map.end())
    return 0;
  Descriptor& descriptor(*iter->second);
  if (descriptor.descriptor_type() != dt_tag)
    return 0;
  DescriptorTag& descriptor_tag(static_cast<DescriptorTag&>(descriptor));
  return is_inode(descriptor_tag.block()) ? descriptor_tag.block() : 0;
}

// This is the only function that accepts "journal block numbers",
// as opposed to "file system block numbers".
static int journal_block_to_real_block(int blocknr)
{
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  assert(blocknr >= 0 && blocknr < journal_maxlen_);
  if (blocknr < 12)
    return journal_inode.block()[blocknr];
  blocknr -= 12;
  int const vpb = block_size_ / sizeof(__le32);	// Values Per Block.
  if (blocknr < vpb)
  {
    get_block(journal_inode.block()[EXT3_IND_BLOCK], block_buf);
    __le32* indirect_block = reinterpret_cast<__le32*>(block_buf);
    return indirect_block[blocknr];
  }
  blocknr -= vpb;
  if (blocknr < vpb * vpb)
  {
    get_block(journal_inode.block()[EXT3_DIND_BLOCK], block_buf);
    __le32* double_indirect_block = reinterpret_cast<__le32*>(block_buf);
    get_block(double_indirect_block[blocknr / vpb], block_buf);
    __le32* indirect_block = reinterpret_cast<__le32*>(block_buf);
    return indirect_block[blocknr % vpb];
  }
  blocknr -= vpb * vpb;
  assert(blocknr < vpb * vpb * vpb);
  get_block(journal_inode.block()[EXT3_TIND_BLOCK], block_buf);
  __le32* tripple_indirect_block = reinterpret_cast<__le32*>(block_buf);
  get_block(tripple_indirect_block[blocknr / (vpb * vpb)], block_buf);
  __le32* double_indirect_block = reinterpret_cast<__le32*>(block_buf);
  get_block(double_indirect_block[(blocknr / vpb) % vpb], block_buf);
  __le32* indirect_block = reinterpret_cast<__le32*>(block_buf);
  return indirect_block[blocknr % vpb];
}

static void iterate_over_journal(
    bool (*action_tag)(uint32_t block, uint32_t sequence, journal_block_tag_t*, void* data),
    bool (*action_revoke)(uint32_t block, uint32_t sequence, journal_revoke_header_t*, void* data),
    bool (*action_commit)(uint32_t block, uint32_t sequence, void* data),
    void* data)
{
  uint32_t jbn = be2le(journal_super_block.s_first);
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  while(jbn < (uint32_t)journal_maxlen_)
  {
    // bn is the real block number inside the journal.
    uint32_t bn = journal_block_to_real_block(jbn);
    unsigned char* block = get_block(bn, block_buf);
    journal_header_t* descriptor = reinterpret_cast<journal_header_t*>(block);
    if (be2le(descriptor->h_magic) == JFS_MAGIC_NUMBER)
    {
      uint32_t blocktype = be2le(descriptor->h_blocktype);
      uint32_t sequence = be2le(descriptor->h_sequence);
      switch (blocktype)
      {
	case JFS_DESCRIPTOR_BLOCK:
	{
	  journal_block_tag_t* ptr = reinterpret_cast<journal_block_tag_t*>((unsigned char*)descriptor + sizeof(journal_header_t));
	  uint32_t flags;
	  do
	  {
	    ++jbn;
	    if (jbn >= (uint32_t)journal_maxlen_)
	    {
	      // This could be cheched by checking that the wrapped around block starts with JFS_MAGIC_NUMBER (which thus overwrote the data block).
	      wrapped_journal_sequence = sequence;
	      return;
	    }
	    else if (action_tag(journal_block_to_real_block(jbn), sequence, ptr, data))
	      return;
	    flags = be2le(ptr->t_flags);
	    if (!(flags & JFS_FLAG_SAME_UUID))
	      ptr += 16 / sizeof(journal_block_tag_t*);
	    ++ptr;
	  }
	  while(!(flags & JFS_FLAG_LAST_TAG));
	  break;
	}
	case JFS_COMMIT_BLOCK:
	{
	  if (action_commit && action_commit(bn, sequence, data))
	    return;
	  break;
	}
	case JFS_REVOKE_BLOCK:
	{
	  if (action_revoke(bn, sequence, (journal_revoke_header_t*)descriptor, data))
	    return;
	  break;
	}
	default:
	{
	  std::cerr << "WARNING: iterate_over_journal: unexpected blocktype (" << blocktype << "). Journal corrupt?\n";
	  return;
	}
      }
    }
    ++jbn;
  }
}

void handle_commandline_journal_transaction(void)
{
  sequence_transaction_map_type::iterator iter = sequence_transaction_map.find(commandline_journal_transaction);
  int prev = -1;
  int next = -1;
  if (iter == sequence_transaction_map.end())
  {
    std::cout << "There is no transaction in the journal with sequence number " << commandline_journal_transaction << '\n';
    if ((size_t)commandline_journal_transaction > max_sequence ||
        (size_t)commandline_journal_transaction < min_sequence)
      std::cout << "The sequences numbers found are in the range [" << min_sequence << ", " << max_sequence << "].\n";
    if ((size_t)commandline_journal_transaction < max_sequence)
    {
      if ((size_t)commandline_journal_transaction > min_sequence)
      {
	prev = commandline_journal_transaction;
	do
	{
	  --prev;
	  iter = sequence_transaction_map.find(prev);
	}
	while(iter == sequence_transaction_map.end());
      }
    }
    else if ((size_t)commandline_journal_transaction > min_sequence)
      prev = max_sequence;
    if ((size_t)commandline_journal_transaction > min_sequence)
    {
      if ((size_t)commandline_journal_transaction < max_sequence)
      {
	next = commandline_journal_transaction;
	do
	{
	  ++next;
	  iter = sequence_transaction_map.find(next);
	}
	while(iter == sequence_transaction_map.end());
      }
    }
    else if ((size_t)commandline_journal_transaction < max_sequence)
      next = min_sequence;
    if (prev != -1 && next != -1)
      std::cout << "Prev / Next sequences numbers: " << prev << ' ' << next << '\n';
    else if (prev != -1)
      std::cout << "Prev sequence number: " << prev << '\n';
    else if (next != -1)
      std::cout << "Next sequence number: " << next << '\n';
  }
  else
  {
    sequence_transaction_map_type::iterator store = iter;
    ++iter;
    if (iter != sequence_transaction_map.end())
      next = iter->second.sequence();
    iter = store;
    if (iter != sequence_transaction_map.begin())
    {
      --iter;
      prev = iter->second.sequence();
      iter = store;
    }
    Transaction& transaction(iter->second);
    if (prev != -1 && next != -1)
      std::cout << "Prev / Current / Next sequences numbers: " << prev << ' ' << transaction.sequence() << ' ' << next << '\n';
    else if (prev != -1)
      std::cout << "Prev / Current sequences numbers: " << prev << ' ' << transaction.sequence() << '\n';
    else if (next != -1)
      std::cout << "Current / Next sequences numbers: " << transaction.sequence() << ' ' << next << '\n';
    else
      std::cout << "Sequence number: " << transaction.sequence() << '\n';
    if (!transaction.committed())
      std::cout << "Transaction was NOT COMMITTED!\n";
    transaction.print_descriptors();
  }
}

void get_inodes_from_journal(int inode, std::vector<std::pair<int, Inode> >& inodes)
{
  uint32_t block = inode_to_block(super_block, inode);
  int offset = (inode - block_to_inode(block)) * inode_size_;
  block_to_descriptors_map_type::iterator descriptors_iter = block_to_descriptors_map.find(block);
  if (descriptors_iter != block_to_descriptors_map.end())
  {
    std::vector<Descriptor*>& descriptors(descriptors_iter->second);
    for (std::vector<Descriptor*>::reverse_iterator descriptor_iter = descriptors.rbegin(); descriptor_iter != descriptors.rend(); ++descriptor_iter)
    {
      Descriptor& descriptor(**descriptor_iter);
      if (descriptor.descriptor_type() != dt_tag)
        continue;
      DescriptorTag& tag(static_cast<DescriptorTag&>(descriptor));
      assert(tag.block() == block);
      static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
      get_block(descriptor.block(), block_buf);
      Inode* inode_ptr = reinterpret_cast<Inode*>(block_buf + offset);
      inodes.push_back(std::pair<int, Inode>(descriptor.sequence(), *inode_ptr));
    }
  }
}

//-----------------------------------------------------------------------------
//
// dir_inode_to_block
//

// dir_inode_to_block_cache is an array of either
// one block numbers stored directly, or pointers to
// array with more than one block (allocated with new).
// The first entry of such an array contains the length
// of the array.
//
// This pseudo vector only stores non-zero block values.
// If 'blocknr' is empty, then the vector is empty.

#define BVASSERT(x) assert(x)

union blocknr_vector_type {
  size_t blocknr;		// The must be a size_t in order to align the least significant bit with the least significant bit of blocknr_vector.
  uint32_t* blocknr_vector;

  void push_back(uint32_t blocknr);
  void remove(uint32_t blocknr);
  void erase(void) { if (is_vector()) delete [] blocknr_vector; blocknr = 0; }
  blocknr_vector_type& operator=(std::vector<uint32_t> const& vec);

  bool empty(void) const { return blocknr == 0; }
  // The rest is only valid if empty() returned false.
  bool is_vector(void) const { BVASSERT(!empty()); return !(blocknr & 1); }
  uint32_t size(void) const { return is_vector() ? blocknr_vector[0] : 1; }
  uint32_t first_entry(void) const { return is_vector() ? blocknr_vector[1] : (blocknr >> 1); }
  uint32_t operator[](int index) const { BVASSERT(index >= 0 && (size_t)index < size()); return (index == 0) ? first_entry() : blocknr_vector[index + 1]; }
};

blocknr_vector_type& blocknr_vector_type::operator=(std::vector<uint32_t> const& vec)
{
  if (!empty())
    erase();
  uint32_t size = vec.size();
  if (size > 0)
  {
    if (size == 1)
      blocknr = (vec[0] << 1) | 1; 
    else
    {
      blocknr_vector = new uint32_t [size + 1]; 
      blocknr_vector[0] = size;
      for (uint32_t i = 0; i < size; ++i)
        blocknr_vector[i + 1] = vec[i];
    }
  }
  return *this;
}

void blocknr_vector_type::push_back(uint32_t bnr)
{
  if (empty())
  {
    assert(bnr);
    blocknr = (bnr << 1) | 1;
  }
  else if (is_vector())
  {
    uint32_t size = blocknr_vector[0] + 1;
    uint32_t* ptr = new uint32_t [size + 1]; 
    ptr[0] = size;
    for (uint32_t i = 1; i < size; ++i)
      ptr[i] = blocknr_vector[i];
    ptr[size] = bnr;
    delete [] blocknr_vector;
    blocknr_vector = ptr;
  }
  else
  {
    uint32_t* ptr = new uint32_t [3];
    ptr[0] = 2;
    ptr[1] = blocknr >> 1;
    ptr[2] = bnr;
    blocknr_vector = ptr;
  }
}

void blocknr_vector_type::remove(uint32_t blknr)
{
  assert(is_vector());
  uint32_t size = blocknr_vector[0];
  int found = 0;
  for (uint32_t j = 1; j <= size; ++j)
    if (blocknr_vector[j] == blknr)
    {
      found = j;
      break;
    }
  assert(found);
  blocknr_vector[found] = blocknr_vector[size];
  blocknr_vector[0] = --size;
  if (size == 1)
  {
    int last_block = blocknr_vector[1];
    delete [] blocknr_vector;
    blocknr = (last_block << 1) | 1;
  }
}

blocknr_vector_type* dir_inode_to_block_cache;
std::vector<int> extended_blocks;

#define INCLUDE_JOURNAL 1

void init_dir_inode_to_block_cache(void)
{
  if (dir_inode_to_block_cache)
    return;
  dir_inode_to_block_cache = new blocknr_vector_type [inode_count_ + 1];
  std::memset(dir_inode_to_block_cache, 0, sizeof(blocknr_vector_type) * (inode_count_ + 1));
  std::string device_name_basename = device_name.substr(device_name.find_last_of('/') + 1);
  std::string cache_stage1 = device_name_basename + ".ext3grep.stage1";
  struct stat sb;
  bool have_cache = !(stat(cache_stage1.c_str(), &sb) == -1);
  if (!have_cache && errno != ENOENT)
  {
    std::cerr << "Failed to open " << std::flush;
    perror(cache_stage1.c_str());
    exit(EXIT_FAILURE);
  }
  if (!have_cache)
  {
    std::cout << "Finding all blocks that might be directories.\n";
    std::cout << "D: block containing directory start, d: block containing more directory entries.\n";
    std::cout << "Each plus represents a directory start that references the same inode as a directory start that we found previously.\n";
    static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
    for (int group = 0; group < groups_; ++group)
    {
      std::cout << "\nSearching group " << group << ": " << std::flush;
      int first_block = first_data_block(super_block) + group * blocks_per_group(super_block);
      int last_block = std::min(first_block + blocks_per_group(super_block), block_count(super_block));
      for (int block = first_block; block < last_block; ++block)
      {
#if !INCLUDE_JOURNAL
	if (is_journal(block))
	  continue;
#endif
	unsigned char* block_ptr = get_block(block, block_buf);
	is_directory_type result = is_directory(block_ptr, block, false);
	if (result == isdir_start)
	{
	  ext3_dir_entry_2* dir_entry = reinterpret_cast<ext3_dir_entry_2*>(block_ptr);
	  assert(dir_entry->name_len == 1 && dir_entry->name[0] == '.');
	  if (dir_inode_to_block_cache[dir_entry->inode].empty())
	    std::cout << 'D' << std::flush;
	  else
	    std::cout << '+' << std::flush;
	  dir_inode_to_block_cache[dir_entry->inode].push_back(block);
	}
	else if (result == isdir_extended)
	{
	  std::cout << 'd' << std::flush;
	  extended_blocks.push_back(block);
        }
      }
    }
    std::cout << '\n';
    std::cout << "Writing analysis so far to '" << cache_stage1 << "'. Delete that file if you want to do this stage again.\n";
    std::ofstream cache;
    cache.open(cache_stage1.c_str());
    cache << "# Stage 1 data for " << device_name << ".\n";
    cache << "# Inodes and directory start blocks that use it for dir entry '.'.\n";
    cache << "# INODE : BLOCK [BLOCK ...]\n";
    for (uint32_t i = 1; i <= inode_count_; ++i)
    {
      blocknr_vector_type const bv = dir_inode_to_block_cache[i];
      if (bv.empty())
	continue;
      cache << i << " :";
      uint32_t const size = bv.size();
      for (uint32_t j = 0; j < size; ++j)
	cache << ' ' << bv[j];
      cache << '\n';
    }
    cache << "# Extended directory blocks.\n";
    for (std::vector<int>::iterator iter = extended_blocks.begin(); iter != extended_blocks.end(); ++iter)
      cache << *iter << '\n';
    cache.close();
  }
  else
  {
    std::cout << "Loading " << cache_stage1 << "...\n";
    std::ifstream cache;
    cache.open(cache_stage1.c_str());
    if (!cache.is_open())
    {
      std::cerr << "Failed to open " << std::flush;
      perror(cache_stage1.c_str());
      exit(EXIT_FAILURE);
    }
    int inode;
    int block;
    char c;
    for(;;)
    {
      cache.get(c);
      if (c == '#')
        cache.ignore(std::numeric_limits<int>::max(), '\n');
      else
      {
        cache.putback(c);
        break;
      }
    }
    while (cache >> inode)
    {
      cache >> c;
      if (cache.eof())
	break;
      assert(c == ':');
      std::vector<uint32_t> blocknr;
      while(cache >> block)
      {
	blocknr.push_back(block);
	c = cache.get();
	if (c != ' ')
	{
	  assert(c == '\n');
	  break;
	}
      }
      dir_inode_to_block_cache[inode] = blocknr;
    }
    cache.clear();
    for(;;)
    {
      cache.get(c);
      if (c == '#')
        cache.ignore(std::numeric_limits<int>::max(), '\n');
      else
      {
        cache.putback(c);
        break;
      }
    }
    while (cache >> block)
      extended_blocks.push_back(block);
    cache.close();
  }
  int inc = 0, sinc = 0, ainc = 0, asinc = 0, cinc = 0;
  for (uint32_t i = 1; i <= inode_count_; ++i)
  {
    bool allocated = is_allocated(i);
    blocknr_vector_type const bv = dir_inode_to_block_cache[i];
    if (allocated)
    {
      Inode& inode = get_inode(i);
      if (is_directory(inode))
      {
	++ainc;
	uint32_t first_block = inode.block()[0];
	// If the inode is an allocated directory, it must reference at least one block.
	if (!first_block)
	{
	  std::cout << std::flush;
	  std::cerr << "ERROR: inode " << i << " is an allocated inode that does not reference any block. "
	      "This seems to indicate a corrupted file system. Manual investigation is needed." << std::endl;
	}
	assert(first_block);
	// If inode is an allocated directory, then we must have found it's directory block already.
	if (bv.empty())
	{
	  std::cerr << "WARNING: inode " << i << " is an allocated inode without directory block pointing to it!\n";
	  continue;
	}
	int count = 0;
	uint32_t size = bv.size();
	for (uint32_t j = 0; j < size; ++j)
	  if (bv[j] == first_block)
	  {
	    ++count;
	    break;	// Remaining blocks have different value.
	  }
	// We must have found the actual directory.
	assert(count == 1);
	// Replace the blocks we found with the canonical block.
	dir_inode_to_block_cache[i].erase();
	dir_inode_to_block_cache[i].push_back(first_block);
	++cinc;
      }
    }
    if (bv.empty())
      continue;
    ++inc;
    if (bv.is_vector())
    {
      ++sinc;
      if (allocated)
	++asinc;
    }
  }
  std::cout << "Result of stage one:\n";
  std::cout << "  " << inc << " inodes are referenced by one or more directory blocks, " <<
      ainc << " of those inodes " << ((ainc == 1) ? "is" : "are") << " still allocated.\n";
  std::cout << "  " << sinc << " inodes are referenced by more than one directory block, " <<
      asinc << " of those inodes " << ((asinc == 1) ? "is" : "are") << " still allocated.\n";
  std::cout << "  " << extended_blocks.size() << " blocks contain an extended directory.\n";
  // Resolve shared inodes.
  int esinc = 0, jsinc = 0, hsinc = 0;
  for (uint32_t i = 1; i <= inode_count_; ++i)
  {
    // All blocks refering to this inode.
    blocknr_vector_type const bv = dir_inode_to_block_cache[i];
    // None?
    if (bv.empty())
      continue;
    uint32_t size = bv.size();
    // Only one? Then we're done.
    if (size == 1)
      continue;

    // Make a list of these blocks as DirectoryBlock.
    std::list<DirectoryBlock> dirs(size);
    std::list<DirectoryBlock>::iterator iter = dirs.begin();
    for (uint32_t j = 0; j < size; ++j, ++iter)
      iter->read_block(bv[j], iter);

    // Remove blocks that are part of the journal, except if all blocks
    // are part of the journal: then keep the block with the highest
    // sequence number.
#if INCLUDE_JOURNAL
    uint32_t highest_sequence = 0;
    int journal_block_count = 0;
    int total_block_count = 0;
    iter = dirs.begin();
    while (iter != dirs.end())
    {
      ++total_block_count;
      if (is_journal(iter->block()))
      {
        ++journal_block_count;
	block_in_journal_to_descriptors_map_type::iterator iter2 = block_in_journal_to_descriptors_map.find(iter->block());
	if (iter2 == block_in_journal_to_descriptors_map.end())
	{
	  std::cout << "Cannot find block " << iter->block() << " (in journal) in block_in_journal_to_descriptors_map!\n";
	  std::cout << "Dump of block_in_journal_to_descriptors_map:\n";
	  for (block_in_journal_to_descriptors_map_type::iterator iter3 = block_in_journal_to_descriptors_map.begin(); iter3 != block_in_journal_to_descriptors_map.end(); ++iter3)
	  {
	    std::cout << iter3->first << ", {" << iter3->second->sequence() << ", " << iter3->second->block() << "}\n";
	  }
	  std::cout << std::flush;
	}
	assert(iter2 != block_in_journal_to_descriptors_map.end());
	uint32_t sequence = iter2->second->sequence();
	highest_sequence = std::max(highest_sequence, sequence);
      }
      else
        break;	// No need to continue.
      ++iter;
    }
    bool need_keep_one_journal = (total_block_count == journal_block_count);
#endif
    iter = dirs.begin();
    while (iter != dirs.end())
    {
#if !INCLUDE_JOURNAL
      assert(!is_journal(iter->block()));
#else
      if (is_journal(iter->block()))
      {
        if (need_keep_one_journal)
	{
	  block_in_journal_to_descriptors_map_type::iterator iter2 = block_in_journal_to_descriptors_map.find(iter->block());
	  if (iter2->second->sequence() == highest_sequence)
	  {
	    ++iter;
	    continue;
	  }
	}
	if (size > 1)
	  dir_inode_to_block_cache[i].remove(iter->block());
	else
	  dir_inode_to_block_cache[i].erase();
	--size;
	iter = dirs.erase(iter);
      }
      else
#endif
	++iter;
    }
    // Only one left? Then we're done with this inode.
    if (dirs.size() == 1)
    {
      ++jsinc;
      continue;
    }
    assert(dirs.size() > 0);
    assert(size == dirs.size());

    // Find blocks in the journal and select the one with the highest sequence number.
    int best_blocknr = -1;
    uint32_t max_sequence = 0;
    iter = dirs.begin();
    for (iter = dirs.begin(); iter != dirs.end(); ++iter)
    {
      int blocknr = iter->block();
      uint32_t sequence_found = find_largest_journal_sequence_number(blocknr);
      if (sequence_found > max_sequence)
      {
	max_sequence = sequence_found;
	best_blocknr = blocknr;
      }
    }
    if (best_blocknr != -1)
    {
      iter = dirs.begin();
      while (iter != dirs.end())
      {
	if (iter->block() != best_blocknr)
	{
	  dir_inode_to_block_cache[i].remove(iter->block());
	  iter = dirs.erase(iter);
	}
	else
	  ++iter;
      }
    }
    // Only one left? Then we're done with this inode.
    if (dirs.size() == 1)
    {
      ++hsinc;
      continue;
    }

    // Remove blocks that are exactly equal.
    iter = dirs.begin();
    while (iter != dirs.end())
    {
      bool found_duplicate = false;
      for (std::list<DirectoryBlock>::iterator iter2 = dirs.begin(); iter2 != iter; ++iter2)
	if (iter2->exactly_equal(*iter))
	{
	  found_duplicate = true;
	  break;
	}
      if (found_duplicate)
      {
	dir_inode_to_block_cache[i].remove(iter->block());
	iter = dirs.erase(iter);
      }
      else
	++iter;
    }
    // Only one left? Then we're done with this inode.
    if (dirs.size() == 1)
    {
      ++esinc;
      continue;
    }

  }	// Next inode.

  std::cout << "Result of stage two:\n";
  if (cinc > 0)
    std::cout << "  " << cinc << " of those inodes could be resolved because " << ((cinc == 1) ? "it is" : "they are") << " still allocated.\n";
  if (jsinc > 0)
    std::cout << "  " << jsinc << " inodes could be resolved because all refering blocks but one were journal blocks.\n";
  if (hsinc > 0)
    std::cout << "  " << hsinc << " inodes could be resolved because at least one of the blocks was found in the journal.\n";
  if (esinc > 0)
    std::cout << "  " << esinc << " inodes could be resolved because all refering blocks were exactly identical.\n";
  if (sinc - asinc - jsinc - esinc - hsinc > 0)
  {
    std::cout << "  " << sinc - asinc - jsinc - esinc - hsinc << " remaining inodes to solve...\n";
    std::cout << "Blocks sharing the same inode:\n";
    std::cout << "# INODE : BLOCK [BLOCK ...]\n";
    for (uint32_t i = 1; i <= inode_count_; ++i)
    {
      blocknr_vector_type const bv = dir_inode_to_block_cache[i];
      if (bv.empty())
	continue;
      uint32_t size = bv.size();
      if (size == 1)
	continue;
      std::cout << i << " :";
      for (uint32_t j = 0; j < size; ++j)
	std::cout << ' ' << bv[j];
      std::cout << '\n';
    }
  }
  else
    std::cout << "All directory inodes are accounted for!\n";
  std::cout << '\n';
}

void init_directories(void);

int dir_inode_to_block(uint32_t inode)
{
  assert(inode > 0 && inode <= inode_count_);
  if (!dir_inode_to_block_cache)
    init_directories();
  blocknr_vector_type const bv = dir_inode_to_block_cache[inode];
  if (bv.empty())
    return -1;
  // In case of multiple values... return one.
  return bv[0];
}

typedef std::map<std::string, Directory> all_directories_type;
all_directories_type all_directories;
typedef std::map<uint32_t, all_directories_type::iterator> inode_to_directory_type;
inode_to_directory_type inode_to_directory;

Directory::Directory(uint32_t inode_number, int first_block) : M_inode_number(inode_number), M_blocks(1)
{
  std::list<DirectoryBlock>::iterator iter = M_blocks.begin();
  iter->read_block(first_block, iter);
}

bool init_directories_action(ext3_dir_entry_2 const& dir_entry, Inode&, bool, bool, bool, bool, bool, bool, Parent* parent, void*)
{
  // Get the inode number.
  uint32_t inode_number = dir_entry.inode;

  // If this is a new directory, skip iterating into it if we already processed it.
  // If it's directory '.' we need to continue with this function.
  if (dir_entry.name_len != 1 || dir_entry.name[0] != '.')
  {
    inode_to_directory_type::iterator iter = inode_to_directory.find(inode_number);
    return iter != inode_to_directory.end();
  }

  // And the first block.
  int first_block = dir_inode_to_block(inode_number);
  assert(first_block != -1);

  // Store a new entry in the all_directories container.
  std::pair<all_directories_type::iterator, bool> res =
      all_directories.insert(all_directories_type::value_type(parent->dirname(false), Directory(inode_number, first_block)));
  if (!res.second)
  {
    if (inode_number == res.first->second.inode_number() && first_block == res.first->second.first_block())
    {
      //std::cout << "Aborting recursion of " << parent->dirname(commandline_show_path_inodes) << '\n';
      return true;	// Abort recursion.
    }
    std::cout << "Directory " << parent->dirname(commandline_show_path_inodes) << " is linked to both inode/block " << inode_number << '/' << first_block <<
	" as well as " << res.first->second.inode_number() << '/' << res.first->second.first_block() << "!\n";
  }
  std::pair<inode_to_directory_type::iterator, bool> res2 =
      inode_to_directory.insert(inode_to_directory_type::value_type(inode_number, res.first));
  if (!res2.second)
  {
    if (inode_number == res2.first->second->second.inode_number() && res.first == res2.first->second)
    {
      //std::cout << "Aborting recursion of " << parent->dirname(commandline_show_path_inodes) << '\n';
      return true;	// Abort recursion.
    }

    std::cout << "Inode number " << inode_number << " is linked to both, " << parent->dirname(commandline_show_path_inodes) << " as well as " << res2.first->second->first << "!\n";
    bool new_path = path_exists(parent->dirname(false));
    bool old_path = path_exists(res2.first->second->first);
    if (new_path && !old_path)
    {
      std::cout << "Using \"" << parent->dirname(commandline_show_path_inodes) << "\" as \"" << res2.first->second->first << " doesn't exist in the locate database.\n";
      inode_to_directory.erase(res2.first);
      std::pair<inode_to_directory_type::iterator, bool> res3 =
	  inode_to_directory.insert(inode_to_directory_type::value_type(inode_number, res.first));
      assert(res3.second);
    }
    else if (!new_path && old_path)
      std::cout << "Keeping \"" << res2.first->second->first << "\" as \"" << parent->dirname(commandline_show_path_inodes) << " doesn't exist in the locate database.\n";
    else if (!new_path && !old_path)
      std::cout << "Neither exist in the locate database. Keeping \"" << res2.first->second->first << "\".\n";
    assert(!(new_path && old_path));
  }
  return false;
}

struct extended_directory_action_data_st {
  int blocknr;
  std::map<int, int> linked;
  std::map<int, int> unlinked;
};

bool filename_heuristics_action(ext3_dir_entry_2 const& dir_entry, Inode& UNUSED(inode),
    bool UNUSED(deleted), bool UNUSED(allocated), bool UNUSED(reallocated), bool UNUSED(zero_inode), bool UNUSED(linked), bool UNUSED(filtered),
    Parent*, void* data)
{
  std::set<std::string>* filesnames = reinterpret_cast<std::set<std::string>*>(data);
  std::string filename(dir_entry.name, dir_entry.name_len);
  filesnames->insert(filename);
  return false;
}

bool extended_directory_action(ext3_dir_entry_2 const& dir_entry, Inode& UNUSED(inode),
    bool UNUSED(deleted), bool UNUSED(allocated), bool UNUSED(reallocated), bool zero_inode, bool linked, bool UNUSED(filtered), Parent*, void* ptr)
{
  extended_directory_action_data_st* data = reinterpret_cast<extended_directory_action_data_st*>(ptr);
  int file_type = (dir_entry.file_type & 7);
  if (file_type == EXT3_FT_DIR && !zero_inode)
  {
    int blocknr2 = dir_inode_to_block(dir_entry.inode);
    if (blocknr2 == -1)
    {
      std::cout << "Cannot find a directory block for inode " << dir_entry.inode << ".\n";
      return true;
    }
    static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
    get_block(blocknr2, block_buf);
    ext3_dir_entry_2 const* dir_entry2 = reinterpret_cast<ext3_dir_entry_2 const*>(block_buf);
    assert(dir_entry2->inode == dir_entry.inode);
    dir_entry2 = reinterpret_cast<ext3_dir_entry_2 const*>(block_buf + dir_entry2->rec_len);
    assert(dir_entry2->name_len == 2 && dir_entry2->name[0] == '.' && dir_entry2->name[1] == '.');
    assert(dir_entry2->inode);
    std::map<int, int>& inode_to_count(linked ? data->linked : data->unlinked);
    std::map<int, int>::iterator iter = inode_to_count.find(dir_entry2->inode);
    if (iter == inode_to_count.end())
      inode_to_count[dir_entry2->inode] = 1;
    else
      ++(inode_to_count[dir_entry2->inode]);
  }
  return false;
}

void link_extended_directory_block_to_inode(unsigned char* block_buf, int blocknr, int inode)
{
  // Add extended directory as DirectoryBlock to the corresponding Directory.
  inode_to_directory_type::iterator directory_iter = inode_to_directory.find(inode);
  if (directory_iter == inode_to_directory.end())
  {
    std::cout << "WARNING: Can't link block " << blocknr << " to inode " << inode << " because that inode cannot be found in the inode_to_directory map!\n";
    return;
  }
  directory_iter->second->second.blocks().push_back(DirectoryBlock());
  std::list<DirectoryBlock>::iterator directory_block_iter = directory_iter->second->second.blocks().end();
  --directory_block_iter;
  directory_block_iter->read_block(blocknr, directory_block_iter);

  // Set up a Parent object that will return the correct dirname.
  ext3_dir_entry_2 fake_dir_entry;
  fake_dir_entry.inode = inode;
  fake_dir_entry.rec_len = 0;	// Not used
  fake_dir_entry.file_type = 0; // Not used
  fake_dir_entry.name_len = directory_iter->second->first.size();
  strncpy(fake_dir_entry.name, directory_iter->second->first.c_str(), fake_dir_entry.name_len);
  Parent dummy_parent(NULL, 0);
  Parent parent(&dummy_parent, &fake_dir_entry, &get_inode(inode), inode);
  assert(parent.dirname(false) == std::string(directory_iter->second->first));
  // Iterate over all directory blocks that we can reach.
  int depth_store = commandline_depth;
  commandline_depth = 10000;
  iterate_over_directory(block_buf, blocknr, init_directories_action, &parent, NULL);
  commandline_depth = depth_store;
}

void init_directories(void)
{
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  std::string device_name_basename = device_name.substr(device_name.find_last_of('/') + 1);
  std::string cache_stage2 = device_name_basename + ".ext3grep.stage2";
  struct stat sb;
  bool have_cache = !(stat(cache_stage2.c_str(), &sb) == -1);
  if (!have_cache && errno != ENOENT)
  {
    std::cerr << "Failed to open " << std::flush;
    perror(cache_stage2.c_str());
    exit(EXIT_FAILURE);
  }
  if (!have_cache)
  {
    init_dir_inode_to_block_cache();

    // Get root inode.
    Inode& root_inode(get_inode(2));
    Parent parent(&root_inode, 2);
    // Get the block that refers to inode 2.
    int root_blocknr = dir_inode_to_block(2);
    assert(root_blocknr != -1);	// This should be impossible; inode 2 is never wiped(?).
    // Get the contents of the first block of the root directory.
    unsigned char* block_buf = new unsigned char [block_size_];
    get_block(root_blocknr, block_buf);
    // Iterate over all first directory blocks that we can reach.
    int depth_store = commandline_depth;
    commandline_depth = 10000;
    iterate_over_directory(block_buf, root_blocknr, init_directories_action, &parent, NULL);
    commandline_depth = depth_store;

    std::cout << "\nProcessing extended directory blocks...\n";
    // Run over all extended directory blocks.
    for (std::vector<int>::iterator iter = extended_blocks.begin(); iter != extended_blocks.end(); ++iter)
    {
      int blocknr = *iter;
      block_to_dir_inode_map_type::iterator iter = block_to_dir_inode_map.find(blocknr);
      int inode_from_journal = (iter == block_to_dir_inode_map.end()) ? -1 : iter->second;
      get_block(blocknr, block_buf);
      extended_directory_action_data_st data;
      data.blocknr = blocknr;
      ++no_filtering;
      iterate_over_directory(block_buf, blocknr, extended_directory_action, NULL, &data);
      --no_filtering;
      bool linked = (data.linked.size() > 0);
      std::map<int, int>& inode_to_count(linked ? data.linked : data.unlinked);
      if (inode_to_count.size() > 0)
      {
	assert(inode_to_count.size() == 1);
	std::cout << "Extended directory at " << blocknr << " belongs to inode " << inode_to_count.begin()->first <<
	    " (from " << inode_to_count.begin()->second << ' ' << (linked ? "linked" : "unlinked") << " directories).\n";
	if (inode_from_journal != -1 && inode_from_journal != inode_to_count.begin()->first)
	  std::cout << "WARNING: according to the journal it should have been inode " << inode_from_journal << "!?\n";
        link_extended_directory_block_to_inode(block_buf, blocknr, inode_to_count.begin()->first);
      }
      else
      {
	// Do some heuristics on the filenames.
	std::set<std::string> filenames;
	++no_filtering;
	iterate_over_directory(block_buf, blocknr, filename_heuristics_action, NULL, &filenames);
	--no_filtering;
	if (filenames.empty())
	{
	  if (inode_from_journal != -1)
	  {
	    std::cout << "Extended directory at " << blocknr << " belongs to inode " << iter->second << " (empty; from journal)).\n";
	    link_extended_directory_block_to_inode(block_buf, blocknr, inode_from_journal);
	  }
	  else
	    std::cout << "Could not find an inode for empty extended directory at " << blocknr << '\n';
	}
	else
	{
	  std::string dir = parent_directory(blocknr, filenames); 
	  if (dir.empty())
	  {
	    if (inode_from_journal != -1)
	    {
	      std::cout << "Extended directory at " << blocknr << " belongs to inode " << inode_from_journal << " (from journal).\n";
	      link_extended_directory_block_to_inode(block_buf, blocknr, inode_from_journal);
	    }
            else
	      std::cout << "Could not find an inode for extended directory at " << blocknr << ", disregarding it's contents.\n";
	  }
	  else
	  {
	    all_directories_type::iterator directory_iter = all_directories.find(dir);
	    if (directory_iter == all_directories.end())
	    {
	      // FIXME: should be processed again after adding extended directory blocks to all_directories!
	      std::cout << "Extended directory at " << blocknr << " belongs to directory " << dir << " but that directory doesn't exist!\n";
	      // Fall back to journal.
	      if (inode_from_journal != -1)
	      {
		std::cout << "Extended directory at " << blocknr << " belongs to inode " << inode_from_journal << " (fall back to journal).\n";
		link_extended_directory_block_to_inode(block_buf, blocknr, inode_from_journal);
	      }
	    }
	    else
	    {
	      std::cout << "Extended directory at " << blocknr << " belongs to inode " << directory_iter->second.inode_number() << '\n';
	      if (inode_from_journal != -1 && (uint32_t)inode_from_journal != directory_iter->second.inode_number())
	        std::cout << "WARNING: according to the journal it should have been inode " << inode_from_journal << "!?\n";
	      link_extended_directory_block_to_inode(block_buf, blocknr, directory_iter->second.inode_number());
	    }
	  }
	}
      }
    }

    delete [] block_buf;
    std::cout << '\n';

    std::cout << "Writing analysis so far to '" << cache_stage2 << "'. Delete that file if you want to do this stage again.\n";
    std::ofstream cache;
    cache.open(cache_stage2.c_str());
    cache << "# Stage 2 data for " << device_name << ".\n";
    cache << "# Inodes path and directory blocks.\n";
    cache << "# INODE PATH BLOCK [BLOCK ...]\n";

    for (inode_to_directory_type::iterator iter = inode_to_directory.begin(); iter != inode_to_directory.end(); ++iter)
    {
      cache << iter->first << " '" << iter->second->first << "'";
      Directory& directory(iter->second->second);
      assert(directory.inode_number() == iter->first);
      for (std::list<DirectoryBlock>::iterator iter2 = directory.blocks().begin(); iter2 != directory.blocks().end(); ++iter2)
        cache << ' ' << iter2->block();
      cache << '\n';
    }

    cache.close();
  }
  else
  {
    std::cout << "Loading " << cache_stage2 << "..." << std::flush;
    std::ifstream cache;
    cache.open(cache_stage2.c_str());
    if (!cache.is_open())
    {
      std::cout << " error" << std::endl;
      std::cerr << "Failed to open " << std::flush;
      perror(cache_stage2.c_str());
      exit(EXIT_FAILURE);
    }
    int inode;
    int blocknr;
    char c;
    for(;;)
    {
      cache.get(c);
      if (c == '#')
        cache.ignore(std::numeric_limits<int>::max(), '\n');
      else
      {
        cache.putback(c);
        break;
      }
    }
    assert(!dir_inode_to_block_cache);
    dir_inode_to_block_cache = new blocknr_vector_type [inode_count_ + 1];
    std::memset(dir_inode_to_block_cache, 0, sizeof(blocknr_vector_type) * (inode_count_ + 1));
    std::stringstream path;
    while (cache >> inode)
    {
      cache.get(c);
      assert(c == ' ');
      cache.get(c);
      assert(c == '\'');
      path.str("");
      cache.get(*path.rdbuf(), '\'');
      if (inode == 2)	// If the function extracts no elements, it calls setstate(failbit).
	cache.clear();
      cache.get(c);	// Extraction stops on end-of-file or on an element that compares equal to delim (which is not extracted).
      assert(c == '\'');
      std::pair<all_directories_type::iterator, bool> res = all_directories.insert(all_directories_type::value_type(path.str(), Directory(inode)));
      assert(res.second);
      std::pair<inode_to_directory_type::iterator, bool> res2 = inode_to_directory.insert(inode_to_directory_type::value_type(inode, res.first));
      assert(res2.second);
      std::vector<uint32_t> block_numbers;
      while(cache >> blocknr)
      {
        block_numbers.push_back(blocknr);
	c = cache.get();
	if (c != ' ')
	{
	  assert(c == '\n');
	  break;
	}
      }
      dir_inode_to_block_cache[inode] = block_numbers;
      std::list<DirectoryBlock>& blocks(res.first->second.blocks());
      blocks.resize(block_numbers.size());
      std::list<DirectoryBlock>::iterator directory_block_iter = blocks.begin();
      for (std::vector<uint32_t>::iterator block_number_iter = block_numbers.begin();
          block_number_iter != block_numbers.end(); ++block_number_iter, ++directory_block_iter)
	directory_block_iter->read_block(*block_number_iter, directory_block_iter);
    }
    cache.close();
    std::cout << " done\n";
  }
}

static void print_directory_inode(int inode)
{
  init_directories();
  int first_block = dir_inode_to_block(inode);
  if (first_block == -1)
  {
    std::cout << "There is no directory block associated with inode " << inode << ".\n";
    return;
  }
  std::cout << "The first block of the directory is " << first_block << ".\n";
  inode_to_directory_type::iterator iter = inode_to_directory.find(inode);
  assert(iter != inode_to_directory.end());
  all_directories_type::iterator directory_iter = iter->second;
  std::cout << "Inode " << inode << " is directory \"" << directory_iter->first << "\".\n";
  if (commandline_dump_names)
    dump_names();
  else
  {
    Directory& directory(directory_iter->second);
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
	directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      std::cout << "Directory block " << directory_block_iter->block() << ":\n";
      std::cout << "          .-- File type in dir_entry (r=regular file, d=directory, l=symlink)\n";
      std::cout << "          |          .-- D: Deleted ; R: Reallocated\n";
      std::cout << "Indx Next |  Inode   | Deletion time                        Mode        File name\n";
      std::cout << "==========+==========+----------------data-from-inode------+-----------+=========\n";
      directory_block_iter->print();  
    }
  }
}

//-----------------------------------------------------------------------------
//
// Individual files
//
// Map files to a single inode.
//

typedef std::map<std::string, int> path_to_inode_map_type;
path_to_inode_map_type path_to_inode_map;

struct JournalData {
  int last_tag_sequence;
  JournalData(int lts) : last_tag_sequence(lts) { }
};

class Sorter {
  private:
    int M_sequence;
    int M_index;
    DirectoryBlock* M_directory_block;
  public:
    Sorter(int sequence, int index, DirectoryBlock& directory_block) : M_sequence(sequence), M_index(index), M_directory_block(&directory_block) { }
    int sequence(void) const { return M_sequence; }
    int index(void) const { return M_index; }
    DirectoryBlock& directory_block(void) const { return *M_directory_block; }
    friend bool operator<(Sorter const& s1, Sorter const& s2) { return s1.M_sequence > s2.M_sequence; }
};

typedef std::map<int, std::vector<std::vector<DirEntry>::iterator> > inode_to_dir_entry_type;
inode_to_dir_entry_type inode_to_dir_entry;

void init_files(void)
{
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  init_directories();

  bool show_inode_dirblock_table = !commandline_inode_dirblock_table.empty();
  all_directories_type::iterator show_inode_dirblock_table_iter;
  if (show_inode_dirblock_table)
    show_inode_dirblock_table_iter = all_directories.find(commandline_inode_dirblock_table);

  // Run over all directories.
  for (all_directories_type::iterator directory_iter = all_directories.begin(); directory_iter != all_directories.end(); ++directory_iter)
  {
    Directory& directory(directory_iter->second);

    // Find all non-journal blocks and fill journal_data_map.
    typedef std::map<int, JournalData> journal_data_map_type;
    journal_data_map_type journal_data_map;
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      if (is_in_journal(directory_block.block()))
        continue;
      // Find related journal information.
      JournalData journal_data(0);
      block_to_descriptors_map_type::iterator iter = block_to_descriptors_map.find(directory_block.block());
      if (iter != block_to_descriptors_map.end())
      {
        std::vector<Descriptor*>& descriptors(iter->second);
	for (std::vector<Descriptor*>::reverse_iterator descriptor_iter = descriptors.rbegin(); descriptor_iter != descriptors.rend(); ++descriptor_iter)
	{
	  Descriptor& descriptor(**descriptor_iter);
	  if (!journal_data.last_tag_sequence && descriptor.descriptor_type() == dt_tag)
	    journal_data.last_tag_sequence = descriptor.sequence();
	  if (journal_data.last_tag_sequence)
	    break;
	}
      }
      journal_data_map.insert(journal_data_map_type::value_type(directory_block.block(), journal_data));
    }
    // Add journal blocks too.
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      if (!is_in_journal(directory_block.block()))
        continue;
      assert(is_journal(directory_block.block()));
      block_in_journal_to_descriptors_map_type::iterator descriptors_iter = block_in_journal_to_descriptors_map.find(directory_block.block());
      assert(descriptors_iter != block_in_journal_to_descriptors_map.end());
      Descriptor& descriptor(*descriptors_iter->second);
      assert(descriptor.descriptor_type() == dt_tag);
      //DescriptorTag& descriptor_tag(static_cast<DescriptorTag&>(descriptor));
      //journal_data_map_type::iterator iter = journal_data_map.find(descriptor_tag.block());
      //if (iter != journal_data_map.end())
      //  continue;	// Refers to a block we already have.
      journal_data_map.insert(journal_data_map_type::value_type(directory_block.block(), JournalData(descriptor.sequence())));
    }

    // Run over all directoy blocks and dir_entries and fill DirEntry::M_directory
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      for (std::vector<DirEntry>::iterator dir_entry_iter = directory_block.dir_entries().begin();
          dir_entry_iter != directory_block.dir_entries().end(); ++dir_entry_iter)
      {
        DirEntry& dir_entry(*dir_entry_iter);
	dir_entry.M_directory = &directory;
      }
    }

    // Count the number of different filenames and directory blocks in this directory.
    int number_of_directory_blocks = 0;
    int number_of_files = 0;
    typedef std::map<std::string, int> filename_to_index_map_type;
    filename_to_index_map_type filename_to_index_map;
    // Run over all directoy blocks and dir_entries.
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      journal_data_map_type::iterator iter = journal_data_map.find(directory_block.block());
      if (iter == journal_data_map.end())
        continue;
      // Count the number of directory blocks.
      ++number_of_directory_blocks;
      for (std::vector<DirEntry>::iterator dir_entry_iter = directory_block.dir_entries().begin();
          dir_entry_iter != directory_block.dir_entries().end(); ++dir_entry_iter)
      {
        DirEntry& dir_entry(*dir_entry_iter);
	if (dir_entry.zero_inode || dir_entry.reallocated)
	  continue;
	if (dir_entry.M_file_type == EXT3_FT_DIR)
	  continue;
	// Count the number of different files.
	std::pair<filename_to_index_map_type::iterator, bool> res = filename_to_index_map.insert(filename_to_index_map_type::value_type(dir_entry.M_name, number_of_files));
	if (res.second)
	  ++number_of_files;
        // Fill inode_to_dir_entry
	inode_to_dir_entry_type::iterator iter2 = inode_to_dir_entry.find(dir_entry.M_inode);
	if (iter2 == inode_to_dir_entry.end())
	  inode_to_dir_entry.insert(inode_to_dir_entry_type::value_type(dir_entry.M_inode, std::vector<std::vector<DirEntry>::iterator>(1, dir_entry_iter)));
	else
	  iter2->second.push_back(dir_entry_iter);
      }
    }
    assert((size_t)number_of_files == filename_to_index_map.size());

    // Create a two-dimensional array of number_of_directory_blocks x number_of_files.
    std::vector<std::vector<int> > file_dirblock_matrix(number_of_directory_blocks, std::vector<int>(number_of_files, 0));
    std::vector<std::string> index_to_filename(number_of_files);
    // Fill the array with inode numbers corresponding to filename / directory block.
    int dirblock_index = -1;
    size_t longest_filename_size = 19;
    // Run over all directoy blocks and dir_entries.
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      journal_data_map_type::iterator iter = journal_data_map.find(directory_block.block());
      if (iter == journal_data_map.end())
        continue;
      ++dirblock_index;
      for (std::vector<DirEntry>::iterator dir_entry_iter = directory_block.dir_entries().begin();
          dir_entry_iter != directory_block.dir_entries().end(); ++dir_entry_iter)
      {
        DirEntry& dir_entry(*dir_entry_iter);
	if (dir_entry.zero_inode || dir_entry.reallocated)
	  continue;
	if (dir_entry.M_file_type == EXT3_FT_DIR)
	  continue;
	int inode = dir_entry.M_inode;
	// Get our filename index.
	int filename_index = filename_to_index_map[dir_entry.M_name];
	index_to_filename[filename_index] = dir_entry.M_name;
	// Find the size of the longest filename.
	longest_filename_size = std::max(longest_filename_size, dir_entry.M_name.size());
	// Fill the array.
	file_dirblock_matrix[dirblock_index][filename_index] = inode;
      }
    }

    std::vector<Sorter> sort_array;
    dirblock_index = -1;
    for (std::list<DirectoryBlock>::iterator directory_block_iter = directory.blocks().begin();
        directory_block_iter != directory.blocks().end(); ++directory_block_iter)
    {
      DirectoryBlock& directory_block(*directory_block_iter);
      journal_data_map_type::iterator iter = journal_data_map.find(directory_block.block());
      if (iter == journal_data_map.end())
        continue;
      ++dirblock_index;
      sort_array.push_back(Sorter(iter->second.last_tag_sequence, dirblock_index, directory_block));
    }
    assert(sort_array.size() == (size_t)number_of_directory_blocks);
    std::sort(sort_array.begin(), sort_array.end());

    if (show_inode_dirblock_table && directory_iter == show_inode_dirblock_table_iter)
    {
      std::cout << "Possible inodes for files in \"" << directory_iter->first << "\":\n";
      // Print a header.
      std::cout << std::right << std::setw(longest_filename_size) << "Directory block nr:";
      for (std::vector<Sorter>::iterator iter = sort_array.begin(); iter != sort_array.end(); ++iter)
      {
	DirectoryBlock& directory_block(iter->directory_block());
	std::cout << " |" << std::setfill(' ') << std::setw(7) << directory_block.block();
      }
      std::cout << '\n';
      int prev_sequence = max_sequence;
      std::cout << std::right << std::setw(longest_filename_size) << "Last tag sequence: ";
      for (std::vector<Sorter>::iterator iter = sort_array.begin(); iter != sort_array.end(); ++iter)
      {
	//DirectoryBlock& directory_block(iter->directory_block());
	int sequence = iter->sequence();
	assert(sequence <= prev_sequence);
	std::cout << " |" << std::setfill(' ') << std::setw(7) << sequence;
	prev_sequence = sequence;
      }
      std::cout << '\n';
      std::cout << std::string(longest_filename_size, '-');
      for (int dirblock_index = 0; dirblock_index < number_of_directory_blocks; ++dirblock_index)
        std::cout << "-+-------";
      std::cout << '\n';
      // Print the array.
      for (int filename_index = 0; filename_index < number_of_files; ++filename_index)
      {
	std::string filename = index_to_filename[filename_index];
	std::cout << std::setfill(' ') << std::left << std::setw(longest_filename_size) << filename;
	for (int dirblock_index = 0; dirblock_index < number_of_directory_blocks; ++dirblock_index)
	{
	  int inode = file_dirblock_matrix[sort_array[dirblock_index].index()][filename_index];
	  if (inode == 0)
	    std::cout << " |       ";
	  else
	    std::cout << " |" << std::setfill(' ') << std::right << std::setw(7) << inode;
	}
	std::cout << '\n';
      }
    }

    // Fill path_to_inode_map.
    for (int filename_index = 0; filename_index < number_of_files; ++filename_index)
    {
      std::string full_path = directory_iter->first;
      if (!full_path.empty())
        full_path += '/';
      full_path += index_to_filename[filename_index];
      int inode = 0;
      for (int dirblock_index = 0; dirblock_index < number_of_directory_blocks; ++dirblock_index)
        if ((inode = file_dirblock_matrix[sort_array[dirblock_index].index()][filename_index]))
	  break;
      if (inode == 0)
        continue;
      path_to_inode_map.insert(path_to_inode_map_type::value_type(full_path, inode));
    }
  }
}

static void dump_names(void)
{
  init_files();
  std::list<std::string> paths;
  for (all_directories_type::iterator iter = all_directories.begin(); iter != all_directories.end(); ++iter)
    paths.push_back(iter->first);
  for (path_to_inode_map_type::iterator iter = path_to_inode_map.begin(); iter != path_to_inode_map.end(); ++iter)
    paths.push_back(iter->first);
  paths.sort();
  for (std::list<std::string>::iterator iter = paths.begin(); iter != paths.end(); ++iter)
    if (!iter->empty())
    {
      if (commandline_restore_all)
	restore_file(*iter);
      else
	std::cout << *iter << '\n';
    }
}

static void show_journal_inodes(int inodenr)
{
  std::vector<std::pair<int, Inode> > inodes;
  get_inodes_from_journal(inodenr, inodes);
  std::cout << "Copies of inode " << inodenr << " found in the journal:\n";
  uint32_t last_mtime = std::numeric_limits<uint32_t>::max();
  for (std::vector<std::pair<int, Inode> >::iterator iter = inodes.begin(); iter != inodes.end(); ++iter)
  {
    Inode& inode(iter->second);
    if (inode.mtime() != last_mtime)
    {
      last_mtime = inode.mtime();
      std::cout << "\n--------------Inode " << inodenr << "-----------------------\n";
      print_inode(inode);
    }
  }
}

enum get_undeleted_inode_type {
  ui_no_inode,
  ui_real_inode,
  ui_journal_inode,
  ui_inode_too_old
};

get_undeleted_inode_type get_undeleted_inode(int inodenr, Inode& inode, int* sequence = NULL)
{
  Inode& real_inode(get_inode(inodenr));
  if (real_inode.dtime() == 0)
  {
    inode = real_inode;
    return ui_real_inode;
  }
  std::vector<std::pair<int, Inode> > inodes;
  get_inodes_from_journal(inodenr, inodes);
  for (std::vector<std::pair<int, Inode> >::iterator iter = inodes.begin(); iter != inodes.end(); ++iter)
  {
    Inode& journal_inode(iter->second);
    if (journal_inode.dtime() == 0)
    {
      inode = journal_inode;
      if (sequence)
	*sequence = iter->first;
      return ui_journal_inode;
    }
    else if (commandline_after && (time_t)journal_inode.dtime() < commandline_after)
      return ui_inode_too_old;
  }
  return ui_no_inode;
}

void show_hardlinks(void)
{
  init_files();
  for (all_directories_type::iterator iter = all_directories.begin(); iter != all_directories.end(); ++iter)
  {
  }
  typedef std::map<int, std::vector<path_to_inode_map_type::iterator> > inodes_type;
  inodes_type inodes;
  for (path_to_inode_map_type::iterator iter = path_to_inode_map.begin(); iter != path_to_inode_map.end(); ++iter)
  {
    struct stat statbuf;
    if (lstat(iter->first.c_str(), &statbuf) == -1)
    {
      int error = errno;
      if (error != ENOENT)
      {
	std::cout << std::flush;
	std::cerr << "WARNING: lstat: " << iter->first << ": " << strerror(error) << std::endl;
      }
    }
    else if (!S_ISDIR(statbuf.st_mode))
    {
      std::pair<inodes_type::iterator, bool> res = inodes.insert(inodes_type::value_type(iter->second, std::vector<path_to_inode_map_type::iterator>()));
      res.first->second.push_back(iter);
    }
    else
    {
      std::cout << std::flush;
      std::cerr << "WARNING: lstat: " << iter->first << ": is a directory" << std::endl;
    }
  }
  for (inodes_type::iterator iter = inodes.begin(); iter != inodes.end(); ++iter)
  {
    if (iter->second.size() > 1)
    {
      std::cout << "Inode " << iter->first << ":\n";
      for (std::vector<path_to_inode_map_type::iterator>::iterator iter3 = iter->second.begin(); iter3 != iter->second.end(); ++iter3)
      {
	std::string::size_type slash = (*iter3)->first.find_last_of('/');
	assert(slash != std::string::npos);
	std::string dirname = (*iter3)->first.substr(0, slash);
        all_directories_type::iterator iter5 = all_directories.find(dirname);
	assert(iter5 != all_directories.end());
        std::cout << "  " << (*iter3)->first << " (" << iter5->second.inode_number() << ")\n";
      }
#if 0
      // Try to figure out which directory it belongs to.
      inode_to_dir_entry_type::iterator iter2 = inode_to_dir_entry.find(iter->first);
      assert(iter2 != inode_to_dir_entry.end());
      Inode inode;
      int sequence;
      get_undeleted_inode_type res = get_undeleted_inode(iter->first, inode, &sequence);
      if (res == ui_no_inode)
      {
        std::cout << "ok: no inode\n";
      }
      else if (res == ui_inode_too_old)
      {
        std::cout << "ok: inode too old\n";
      }
      else if (res == ui_real_inode)
      {
	for (std::vector<std::vector<DirEntry>::iterator>::iterator iter4 = iter2->second.begin(); iter4 != iter2->second.end(); ++iter4)
	{
	  DirEntry& dir_entry(**iter4);
	  int dirblocknr = dir_entry.M_directory_iterator->block();
	  int group = block_to_group(super_block, dirblocknr);;
	  unsigned int bit = dirblocknr - first_data_block(super_block) - group * blocks_per_group(super_block);
	  assert(bit < 8U * block_size_);
	  struct bitmap_ptr bmp = get_bitmap_mask(bit);
	  assert(block_bitmap[group]);
	  bool allocated = (block_bitmap[group][bmp.index] & bmp.mask);
	  if (allocated)
	    std::cout << "ok: " << dir_entry.M_directory->inode_number() << '/' << dir_entry.M_name << '\n';
	}
      }
      else if (res == ui_journal_inode)
      {
        Transaction& transaction(sequence_transaction_map.find(sequence)->second);
	for (std::vector<std::vector<DirEntry>::iterator>::iterator iter4 = iter2->second.begin(); iter4 != iter2->second.end(); ++iter4)
	{
	  DirEntry& dir_entry(**iter4);
	  int dirblocknr = dir_entry.M_directory_iterator->block();
	  if (transaction.contains_tag_for_block(dirblocknr))
	    std::cout << "ok: " << dir_entry.M_directory->inode_number() << '/' << dir_entry.M_name << '\n';
        }
      }
#endif
    }
  }
}

struct Data {
  std::ostream& out;
  int remaining_size;

  Data(std::ostream& out_, uint32_t remaining_size_) : out(out_), remaining_size(remaining_size_) { }
};

void restore_file_action(int blocknr, void* ptr)
{
  Data& data(*reinterpret_cast<Data*>(ptr));
  static unsigned char block_buf[EXT3_MAX_BLOCK_SIZE];
  get_block(blocknr, block_buf);
  int len = std::min(data.remaining_size, block_size_);
  data.out.write((char const*)block_buf, len);
  assert(data.out.good());
  data.remaining_size -= len;
}

mode_t inode_mode_to_mkdir_mode(uint16_t mode)
{
  mode_t result = 0;
  if ((mode & 04000))
    result |= S_ISUID;
  if ((mode & 02000))
    result |= S_ISGID;
  if ((mode & 01000))
    result |= S_ISVTX;
  if ((mode & 0400))
    result |= S_IRUSR;
  if ((mode & 0200))
    result |= S_IWUSR;
  if ((mode & 0100))
    result |= S_IXUSR;
  if ((mode & 040))
    result |= S_IRGRP;
  if ((mode & 020))
    result |= S_IWGRP;
  if ((mode & 010))
    result |= S_IXGRP;
  if ((mode & 04))
    result |= S_IROTH;
  if ((mode & 02))
    result |= S_IWOTH;
  if ((mode & 01))
    result |= S_IXOTH;
  return result;
}

extern "C" int lutimes (char const*, struct timeval const [2]);

char const* mode_str(int16_t i_mode)
{
  switch ((i_mode & 0xf000))
  {
    case 0x1000:
      return "FIFO";
    case 0x2000:
      return "Character device";
    case 0x4000:
      return "Directory";
    case 0x6000:
      return "Block device";
    case 0x8000:
      return "Regular file";
    case 0xA000:
      return "Symbolic link";
    case 0xC000:
      return "UNIX socket";
  }
  // To prevent a compiler warning.
  return "*UNKNOWN*";
}

void restore_file(std::string const& outfile)
{
  assert(!outfile.empty());
  assert(outfile[0] != '/');
  init_files();
  int inodenr;
  path_to_inode_map_type::iterator inode_iter = path_to_inode_map.find(outfile);
  if (inode_iter != path_to_inode_map.end())
    inodenr = inode_iter->second;
  else
  {
    all_directories_type::iterator directory_iter = all_directories.find(outfile);
    if (directory_iter == all_directories.end())
    {
      std::cout << "Cannot find an inode number for file \"" << outfile << "\".\n";
      return;
    }
    inodenr = directory_iter->second.inode_number();
  }
  Inode real_inode = get_inode(inodenr);
  std::string::size_type slash = outfile.find_last_of('/');
  if (slash != std::string::npos)
  {
    std::string dirname = outfile.substr(0, slash);
    struct stat statbuf;
    if (lstat((outputdir + dirname).c_str(), &statbuf) == -1)
    {
      int error = errno;
      if (error != ENOENT)
      {
	std::cout << std::flush;
	std::cerr << "WARNING: lstat: " << (outputdir + dirname) << ": " << strerror(error) << std::endl;
	std::cout << "Failed to recover " << outfile << '\n';
	return;
      }
      else
        restore_file(dirname);
    }
    else if (!S_ISDIR(statbuf.st_mode))
    {
      std::cout << std::flush;
      std::cerr << "ERROR: Failed to recover " << outfile << ": " << (outputdir + dirname) << " exists but is not a directory!\n";
      exit(EXIT_FAILURE);
    }
  }
  std::string outputdir_outfile = outputdir + outfile;
  if (is_directory(real_inode))
  {
    if (mkdir(outputdir_outfile.c_str(), inode_mode_to_mkdir_mode(real_inode.mode())) == -1 && errno != EEXIST)
    {
      perror("mkdir");
      std::cout << std::flush;
      std::cerr << "Could not create directory " << outputdir_outfile << '\n';
      exit(EXIT_FAILURE);
    }
    struct utimbuf ub;
    ub.actime = real_inode.atime();
    ub.modtime = real_inode.ctime();
    if (utime(outputdir_outfile.c_str(), &ub) == -1)
    {
      int error = errno;
      std::cout << "WARNING: Failed to set access and modification time on " << outputdir_outfile << ": " << strerror(error) << '\n';
    }
  }
  else
  {
    Inode inode;
    get_undeleted_inode_type res = get_undeleted_inode(inodenr, inode);
    if (res != ui_real_inode && res != ui_journal_inode)
    {
      if (res == ui_no_inode)
	std::cout << "Cannot find an undeleted inode for file \"" << outfile << "\".\n";
      else
        std::cout << "Not undeleting \"" << outfile << "\" because it was deleted before " << commandline_after << " (" << inode.ctime() << ")\n";
      return;
    }
    assert(inode.dtime() == 0);
    if (is_regular_file(inode))
    {
      std::ofstream out;
      out.open(outputdir_outfile.c_str());
      if (!out)
      {
	std::cout << "Failed to open \"" << outputdir_outfile << "\".\n";
	return;
      }
      Data data(out, inode.size());
      std::cout << "Restoring " << outfile << '\n';
      bool reused_or_corrupted_indirect_block8 = iterate_over_all_blocks_of(inode, restore_file_action, &data);
      assert(out.good());
      out.close();
      if (reused_or_corrupted_indirect_block8)
      {
        std::cout << "WARNING: Failed to restore " << outfile << ": encountered a reused or corrupted (double/triple) indirect block!\n";
	// FIXME: file should be renamed.
      }
      if (chmod(outputdir_outfile.c_str(), inode_mode_to_mkdir_mode(inode.mode())) == -1)
      {
        int error = errno;
	std::cout << std::flush;
	std::cout << "WARNING: failed to set file mode on " << outputdir_outfile << '\n';
	errno = error;
	perror("chmod");
      }
      struct utimbuf ub;
      ub.actime = inode.atime();
      ub.modtime = inode.ctime();
      if (utime(outputdir_outfile.c_str(), &ub) == -1)
      {
	int error = errno;
	std::cout << "WARNING: Failed to set access and modification time on " << outputdir_outfile << ": " << strerror(error) << '\n';
	return;
      }
    }
    else if (is_symlink(inode))
    {
      std::ostringstream symlink_name;
      int len = print_symlink(symlink_name, inode);
      if (len == 0)
      {
        std::cout << "WARNING: Failed to recover " << outfile << ": symlink has zero length!\n";
	return;
      }
      else
      {
        if (symlink(symlink_name.str().c_str(), outputdir_outfile.c_str()) == -1)
	{
	  int error = errno;
	  std::cout << std::flush;
	  std::cout << "WARNING: symlink: " << outputdir_outfile << ": " << strerror(error) << '\n';
	  return;
	}
	struct timeval tvp[2];
	tvp[0].tv_sec = inode.atime();
	tvp[0].tv_usec = 0;
	tvp[1].tv_sec = inode.ctime();
	tvp[1].tv_usec = 0;
        if (lutimes(outputdir_outfile.c_str(), tvp) == -1)
	{
	  int error = errno;
	  std::cout << "WARNING: Failed to set access and modification time on " << outputdir_outfile << ": " << strerror(error) << '\n';
	  return;
	}
      }
    }
    else
    {
      std::cout << "WARNING: Not recovering \"" << outfile << "\", which is a " << mode_str(inode.mode()) << '\n';
      return;
    }
  }
}

