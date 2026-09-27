#ifndef FS_EXT3_H
#define FS_EXT3_H

#include "fs/ext2/ext2.h"
#include <stdint.h>

#define EXT3_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT3_JOURNAL_MAGIC_NUMBER 0xc03b3998u

#define JBD_DESCRIPTOR_BLOCK 1
#define JBD_COMMIT_BLOCK 2
#define JBD_SUPERBLOCK_V1 3
#define JBD_SUPERBLOCK_V2 4
#define JBD_REVOKE_BLOCK 5

#define JBD_FLAG_ESCAPE 1
#define JBD_FLAG_SAME_UUID 2
#define JBD_FLAG_DELETED 4
#define JBD_FLAG_LAST_TAG 8

typedef struct {
  uint32_t h_magic;
  uint32_t h_blocktype;
  uint32_t h_sequence;
} __attribute__((packed)) jbd_header_t;

typedef struct {
  uint32_t t_blocknr;
  uint32_t t_flags;
} __attribute__((packed)) jbd_block_tag_t;

typedef struct {
  jbd_header_t s_header;
} __attribute__((packed)) jbd_descriptor_block_t;

typedef struct {
  jbd_header_t s_header;
  uint32_t s_blocksize;
  uint32_t s_maxlen;
  uint32_t s_first;
  uint32_t s_sequence;
  uint32_t s_start;
  uint32_t s_errno;
  uint32_t s_feature_compat;
  uint32_t s_feature_incompat;
  uint32_t s_feature_ro_compat;
  uint8_t s_uuid[16];
  uint32_t s_nr_users;
  uint32_t s_dynsuper;
  uint32_t s_max_transaction;
  uint32_t s_max_trans_data;
  uint8_t s_padding[44];
  uint8_t s_users[16 * 48];
} __attribute__((packed)) jbd_superblock_t;

void ext3_init_journal(ext2_mount_t *mnt);

int ext3_journal_start(ext2_mount_t *mnt);
int ext3_journal_stop(ext2_mount_t *mnt);
int ext3_journal_block(ext2_mount_t *mnt, uint32_t block_nr, const void *data);

#endif
