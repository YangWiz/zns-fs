#include "allocator.hpp"

#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "structures.h"
#define Round_down(n, m) (n - (n % m))

BlockManager::BlockManager(user_zns_device *disk) {
  this->lba_size = disk->lba_size_bytes;
  // TODO(Zhiyang): Leave some spaces for imap metadata.
  this->imap_size = this->lba_size * IMAP_OCCUPY;
  this->disk = disk;
  this->capacity = disk->capacity_bytes;
  this->wp = WritePointer{
      .wp_lock = PTHREAD_RWLOCK_INITIALIZER,
      .position = imap_size,  // leave space for the imap.
  };
}

int BlockManager::append(void *buffer, uint32_t size, uint64_t *start_addr,
                         bool update) {
  // update operation should be atomic.
  /*
  check wheter the wp is on the block boundary.
  if on the boundary, append directly else
  read the block out then append.

  wp should be the multiple of block size, e.g. 4097 => 4096.
  needs to hold the lock when writing.
  */
  int ret = 0;
  pthread_rwlock_wrlock(&this->wp.wp_lock);
  uint64_t wp = this->get_current_position();

  // printf("current wp is %lx, size is %d\n", wp, size);
  uint32_t lba_size = this->disk->lba_size_bytes;
  *start_addr = wp;
  if (wp + size >= disk->capacity_bytes) {
    pthread_rwlock_unlock(&this->wp.wp_lock);
    return -1;
  }

  if (wp % lba_size == 0) {
    size_t padding_size = Round_up(size, lba_size);
    ret = zns_udevice_write(this->disk, wp, buffer, padding_size);
  } else {
    uint64_t wp_base = Round_down(wp, lba_size);
    uint64_t curr_data_size_in_block = wp - wp_base;
    size_t current_size = size + curr_data_size_in_block;
    size_t padding_size = current_size - (current_size % lba_size) + lba_size;

    char blocks[padding_size];
    ret = zns_udevice_read(this->disk, wp_base, blocks, lba_size);
    memcpy(blocks + curr_data_size_in_block, buffer, size);
    ret = zns_udevice_write(this->disk, wp_base, blocks, padding_size);
  }

  if (update) {
    this->update_current_position(wp + size);
  }
  pthread_rwlock_unlock(&this->wp.wp_lock);
  return ret;
}

int BlockManager::read(uint64_t lba, void *buffer, uint32_t size) {
  // printf("read\n");

  // printf("current wp is %lx, size is %d\n", wp, size);
  uint32_t lba_size = this->disk->lba_size_bytes;
  uint64_t padding_size;
  uint64_t wp_base = (lba / lba_size) * lba_size;
  uint64_t curr_data_size_in_block = lba - wp_base;
  uint64_t read_size = size + curr_data_size_in_block;
  if (read_size % lba_size != 0) {
    padding_size = (read_size / lba_size) * (lba_size) + lba_size;
  } else {
    padding_size = read_size;
  }
  char *buf = static_cast<char *>(calloc(1, padding_size));
  int ret = zns_udevice_read(this->disk, wp_base, buf, padding_size);

  if (ret != 0) {
    printf("error!\n");
  }

  memcpy(buffer, buf + curr_data_size_in_block, size);
  free(buf);
  return ret;
}

int BlockManager::write(uint64_t lba, void *buffer, uint32_t size) {
  // if no padding needed, only needs before part + buffer part.
  // if address is on the block boundary, no need before part.
  // if no padding and no boundary cross, write directly.
  int ret = 0;
  uint32_t lba_size = this->disk->lba_size_bytes;
  uint64_t padding_size;
  uint64_t wp_base = (lba / lba_size) * lba_size;
  uint64_t curr_data_size_in_block = lba - wp_base;

  bool padding;
  bool cross_bd;
  if (lba % lba_size == 0) {
    cross_bd = false;
  } else {
    cross_bd = true;
  }

  if ((size + curr_data_size_in_block) % lba_size != 0) {
    padding_size =
        ((size + curr_data_size_in_block) / lba_size) * (lba_size) + lba_size;
    padding = true;
  } else {
    padding_size = size;
    padding = false;
  }

  if (!padding & !cross_bd) {
    ret = zns_udevice_write(this->disk, lba, buffer, size);
  }
  if (padding & !cross_bd) {
    char after[lba_size];
    char new_blocks[padding_size];

    uint64_t after_base = ((size + lba) / lba_size) * lba_size;
    uint64_t after_index = size + lba - after_base;
    int ret1 = zns_udevice_read(this->disk, after_base, after, lba_size);

    memcpy(new_blocks, buffer, size);
    memcpy(new_blocks + size, after + after_index, padding_size - size);
    int ret2 = zns_udevice_write(this->disk, lba, new_blocks, padding_size);
    ret = ret1 & ret2;
  }
  if (!padding & cross_bd) {
    char before[lba_size];
    char new_blocks[padding_size];

    int ret1 = zns_udevice_read(this->disk, wp_base, before, lba_size);

    memcpy(new_blocks, before, curr_data_size_in_block);
    memcpy(new_blocks + curr_data_size_in_block, buffer, size);
    int ret2 = zns_udevice_write(this->disk, wp_base, new_blocks, padding_size);
    ret = ret1 & ret2;
  }
  if (padding & cross_bd) {
    char before[lba_size];
    char after[lba_size];
    char new_blocks[padding_size];

    uint64_t after_base = ((size + lba) / lba_size) * lba_size;
    uint64_t after_index = size + lba - after_base;
    int ret1 = zns_udevice_read(this->disk, wp_base, before, lba_size);
    int ret2 = zns_udevice_read(this->disk, after_base, after, lba_size);
    ret = ret1 & ret2;

    memcpy(new_blocks, before, curr_data_size_in_block);
    memcpy(new_blocks + curr_data_size_in_block, buffer, size);
    memcpy(new_blocks + curr_data_size_in_block + size, after + after_index,
           padding_size - size - curr_data_size_in_block);
    int ret3 = zns_udevice_write(this->disk, wp_base, new_blocks, padding_size);
    ret = ret & ret3;
  }

  return ret;
}

uint64_t BlockManager::get_current_position() {
  uint64_t ret = this->wp.position;
  return ret;
}

int BlockManager::update_current_position(uint64_t addr) {
  this->wp.position = addr;
  return 1;
}
