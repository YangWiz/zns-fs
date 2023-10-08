#ifndef STOSYS_PROJECT_FILE_H_
#define STOSYS_PROJECT_FILE_H_
#include "allocator.hpp"
#pragma once

#include <mutex>

#include "inode.hpp"
#include "storage_layer.hpp"
#include "structures.h"

class StoFile {
 public:
  std::string name;
  StoFile(const ss_inode *inode, BlockManager *allocator);
  StoFile(StoInode *inode, BlockManager *allocator);
  ~StoFile();
  void write_to_disk();
  void write(size_t size, void *data);
  void read(size_t size, void *result);
  StoInode *inode;
  BlockManager *allocator;
};

#endif