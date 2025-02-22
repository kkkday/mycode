// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#if !defined(ROCKSDB_LITE) && defined(OS_LINUX) && defined(LIBZBD)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/file_system.h"
#include "zbd_zenfs.h"
#include "db/version_edit.h"

namespace ROCKSDB_NAMESPACE {

class Buffer {
 public:
  int buffer_size_;
  int valid_size_;
  char* buffer_;

  explicit Buffer(void* data, int data_size, int valid_size, uint32_t block_size)
      :buffer_size_(data_size),
       valid_size_(valid_size)
       {
          int ret = posix_memalign((void**)&buffer_, block_size, data_size);
          if (ret) {
            fprintf(stderr, "failed allocating alignment write buffer\n");
            exit(1);
          }
          memcpy(buffer_, data, buffer_size_);
       };

  ~Buffer(){
    delete (char*)buffer_;
  }

};

class ZoneExtent {
 public:
  uint64_t start_;
  uint32_t length_;
  Zone* zone_;

  explicit ZoneExtent(uint64_t start, uint32_t length, Zone* zone);
  Status DecodeFrom(Slice* input);
  void EncodeTo(std::string* output);
};

class ZoneFile {
 protected:
  ZonedBlockDevice* zbd_;
  std::vector<ZoneExtent*> extents_;
  Zone* active_zone_;
  uint64_t extent_start_;
  uint64_t extent_filepos_;

  Env::WriteLifeTimeHint lifetime_;
  uint64_t fileSize;
  std::string filename_;
  uint64_t file_id_;
  uint32_t nr_synced_extents_;
  /*Append to Zone only After Finish() is called from table builer*/
  std::vector<Buffer*> full_buffer_;

 public:
  InternalKey smallest_;
  InternalKey largest_;
  int level_;

  std::atomic<bool> is_appending_;
  std::atomic<bool> marked_for_del_;
  bool should_flush_full_buffer_;
  bool is_sst_;
  uint64_t fno_;

  std::mutex extent_mtx_;
  std::atomic<bool> extent_writer;
  std::atomic<unsigned int> extent_reader;
  std::condition_variable extent_cv;

  IOStatus FullBuffer(void*, int, int);
  ZonedBlockDevice* get_zbd(){return zbd_;};

  Zone * GetActiveZone(){return active_zone_;};
  explicit ZoneFile(ZonedBlockDevice* zbd, std::string filename,
                    uint64_t file_id_);

  virtual ~ZoneFile();

  void CloseWR();
  IOStatus AppendBuffer();
  IOStatus Append(void* data, int data_size, int valid_size);
  IOStatus SetWriteLifeTimeHint(Env::WriteLifeTimeHint lifetime);
  std::string GetFilename();
  void Rename(std::string name);
  uint64_t GetFileSize();
  void SetFileSize(uint64_t sz);

  uint32_t GetBlockSize() { return zbd_->GetBlockSize(); }
  std::vector<ZoneExtent*> GetExtents() { return extents_; }
  Env::WriteLifeTimeHint GetWriteLifeTimeHint() { return lifetime_; }

  IOStatus PositionedRead(uint64_t offset, size_t n, Slice* result,
                          char* scratch, bool direct);
  ZoneExtent* GetExtent(uint64_t file_offset, uint64_t* dev_offset);
  void PushExtent();

  void ExtentReadLock();
  void ExtentReadUnlock();

  void ExtentWriteLock();
  void ExtentWriteUnlock();

  void EncodeTo(std::string* output, uint32_t extent_start);
  void EncodeUpdateTo(std::string* output) {
    EncodeTo(output, nr_synced_extents_);
  };
  void EncodeSnapshotTo(std::string* output) { EncodeTo(output, 0); };
  void MetadataSynced() { nr_synced_extents_ = extents_.size(); };

  Status DecodeFrom(Slice* input);
  Status MergeUpdate(ZoneFile* update);

  std::vector<ZoneExtent*>& GetExtentsList(){return extents_;};
  void UpdateExtents(std::vector<ZoneExtent*>& a){
      extents_ = a;
  };
  uint64_t GetID() { return file_id_; }
  size_t GetUniqueId(char* id, size_t max_size);

};

class ZonedWritableFile : public FSWritableFile {
 public:
  /* Interface for persisting metadata for files */
  class MetadataWriter {
   public:
    virtual ~MetadataWriter();
    virtual IOStatus Persist(ZoneFile* zoneFile) = 0;
  };

  explicit ZonedWritableFile(ZonedBlockDevice* zbd, bool buffered,
                             ZoneFile* zoneFile,
                             MetadataWriter* metadata_writer = nullptr);
  virtual ~ZonedWritableFile();
  
  virtual IOStatus Append(const Slice& data, const IOOptions& options,
                          IODebugContext* dbg) override;
  virtual IOStatus Append(const Slice& data, const IOOptions& opts,
                          const DataVerificationInfo& /* verification_info */,
                          IODebugContext* dbg) override {
    return Append(data, opts, dbg);
  }
  virtual IOStatus PositionedAppend(const Slice& data, uint64_t offset,
                                    const IOOptions& options,
                                    IODebugContext* dbg) override;
  virtual IOStatus PositionedAppend(
      const Slice& data, uint64_t offset, const IOOptions& opts,
      const DataVerificationInfo& /* verification_info */,
      IODebugContext* dbg) override {
    return PositionedAppend(data, offset, opts, dbg);
  }
  virtual IOStatus Truncate(uint64_t size, const IOOptions& options,
                            IODebugContext* dbg) override;
  virtual IOStatus Close(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Flush(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override;
  virtual IOStatus RangeSync(uint64_t offset, uint64_t nbytes,
                             const IOOptions& options,
                             IODebugContext* dbg) override;
  virtual IOStatus Fsync(const IOOptions& options,
                         IODebugContext* dbg) override;
  bool use_direct_io() const override { return !buffered; }
  bool IsSyncThreadSafe() const override { return true; };
  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }
  void SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) override;
  void ShouldFlushFullBuffer();
  void SetMinMaxKeyAndLevel(const Slice&, const Slice&, const int);
 private:
  IOStatus BufferedWrite(const Slice& data);
  IOStatus FlushBuffer();

  bool buffered;
  char* buffer;
  size_t buffer_sz;
  uint32_t block_sz;
  uint32_t buffer_pos;
  uint64_t wp;
  int write_temp;

  ZoneFile* zoneFile_;
  MetadataWriter* metadata_writer_;

  std::mutex buffer_mtx_;
};

class ZonedSequentialFile : public FSSequentialFile {
 private:
  ZoneFile* zoneFile_;
  uint64_t rp;
  bool direct_;

 public:
  explicit ZonedSequentialFile(ZoneFile* zoneFile, const FileOptions& file_opts)
      : zoneFile_(zoneFile), rp(0), direct_(file_opts.use_direct_reads) {}

  IOStatus Read(size_t n, const IOOptions& options, Slice* result,
                char* scratch, IODebugContext* dbg) override;
  IOStatus PositionedRead(uint64_t offset, size_t n, const IOOptions& options,
                          Slice* result, char* scratch,
                          IODebugContext* dbg) override;
  IOStatus Skip(uint64_t n);

  bool use_direct_io() const override { /*return target_->use_direct_io(); */
    return true;
  }

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t /*offset*/, size_t /*length*/) override {
    return IOStatus::OK();
  }
};

class ZonedRandomAccessFile : public FSRandomAccessFile {
 private:
  ZoneFile* zoneFile_;
  bool direct_;

 public:
  explicit ZonedRandomAccessFile(ZoneFile* zoneFile,
                                 const FileOptions& file_opts)
      : zoneFile_(zoneFile), direct_(file_opts.use_direct_reads) {}

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override;

  IOStatus MultiRead(FSReadRequest* /*reqs*/, size_t /*num_reqs*/,
                     const IOOptions& /*options*/,
                     IODebugContext* /*dbg*/) override {
    return IOStatus::IOError("Not implemented");
  }

  IOStatus Prefetch(uint64_t /*offset*/, size_t /*n*/,
                    const IOOptions& /*options*/,
                    IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  bool use_direct_io() const override { return true; }

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t /*offset*/, size_t /*length*/) override {
    return IOStatus::OK();
  }

  size_t GetUniqueId(char* id, size_t max_size) const override;
};

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX) && defined(LIBZBD)
