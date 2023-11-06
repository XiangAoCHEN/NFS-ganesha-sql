#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <unordered_map>
#include <list>
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include "applier/applier_config.h"
#include "applier/utility.h"

class PageAddressLru {
public:
    bool in_lru_; // 该page是否在LRU中
    space_id_t space_id_;
    page_id_t page_id_;
};

class SpaceReaderWriter {
public:
    SpaceReaderWriter(){

    }

    ~SpaceReaderWriter() {
        if (stream_.use_count() == 0 && stream_->is_open()) {
            stream_->close();
        }
    };

    bool writePage(const char *data, page_id_t page_id, size_t data_page_size_) {
      std::unique_lock lock(mutex_);
      if (!canOperate())
        return false;

      active_operations++;
      stream_->seekp(static_cast<std::streamoff>(page_id * data_page_size_));
      stream_->write(data, data_page_size_);
      active_operations--;

      return true;
    }

    bool readPage(char *data, page_id_t page_id, size_t data_page_size_) {
      std::unique_lock lock(mutex_);
      if (!canOperate())
        return false;

      active_operations++;
      stream_->seekp(static_cast<std::streamoff>(page_id * data_page_size_),
                     std::ios::beg);
      stream_->read(data, data_page_size_);
      active_operations--;
      return true;
    }

    uint32_t get_max_page_id(size_t data_page_size_) {
      std::unique_lock lock(mutex_);
      if (!canOperate())
        return false;

      active_operations++;
      stream_->seekp(0, std::ios_base::end);
      uint32_t max_page_id = (stream_->tellg() / data_page_size_) - 1;
      active_operations--;
      return max_page_id;
    }

    void Close() {
      std::unique_lock lock(mutex_);
      while (active_operations > 0) {
        // Optionally, use a condition_variable to wait here
      }
      if (stream_->is_open()) {
        stream_->close();
      }
    }

    explicit SpaceReaderWriter(std::string &file_name)
            : file_name_(file_name),
              stream_(std::make_shared<std::fstream>(file_name_, std::ios::binary | std::ios::out | std::ios::in)),
                deleted_flag(false){}
    bool canOperate() const {
      return !deleted_flag && stream_->is_open();
    }
    std::string file_name_{};
    std::shared_ptr<std::fstream> stream_{};
    bool deleted_flag;//for MLOG_FILE_DELETE

    std::mutex mutex_;
    int active_operations; // number of active read/write operations

    pthread_rwlock_t rw_lock_;
};

// 前置声明
class BufferPool;

class Page {
public:
    friend class BufferPool;

    enum class State {
        INVALID = 0, // 刚被初始化，data_还未被填充，不可用
        FROM_BUFFER = 1, // 刚从buffer pool中被创建出来
        FROM_DISK = 2, // 从磁盘中读上来的
    };

    Page();

    // Copy Constructor
    Page(const Page &other);

    ~Page();

    [[nodiscard]] lsn_t GetLSN() const {
        return mach_read_from_8(data_ + FIL_PAGE_LSN);
    }

    [[nodiscard]] uint32_t GetCheckSum() const {
        uint32_t checksum1 = mach_read_from_4(data_ + DATA_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM);
        uint32_t checksum2 = mach_read_from_4(data_ + FIL_PAGE_SPACE_OR_CHKSUM);
        assert(checksum1 == checksum2);
        return mach_read_from_4(data_ + FIL_PAGE_SPACE_OR_CHKSUM);
    }

    [[nodiscard]] uint32_t GetPageId() const {
        return mach_read_from_4(data_ + FIL_PAGE_OFFSET);
    }

    [[nodiscard]] uint32_t GetSpaceId() const {
        return mach_read_from_4(data_ + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
    }

    void Reset() {
        std::memset(data_, 0, DATA_PAGE_SIZE);
        state_ = State::INVALID;
    }

    // TODO
    void WriteCheckSum(uint32_t checksum);

    [[nodiscard]] unsigned char *GetData() const {
        return data_;
    }

    State GetState() {
        return state_;
    }

    void SetState(State state) {
        state_ = state;
    }

    void WritePageLSN(lsn_t lsn) {
        // 写page的lsn时，有两处地方需要写
        // 1. 头部的FIL_PAGE_LSN属性
        mach_write_to_8(FIL_PAGE_LSN + data_, lsn);
        // 2. 尾部的FIL_PAGE_END_LSN_OLD_CHKSUM属性的后4位
        mach_write_to_8(DATA_PAGE_SIZE
                        - FIL_PAGE_END_LSN_OLD_CHKSUM
                        + data_, lsn);
        dirty_ = true;
    }

    void PageLock() { mutex_.lock(); }

    void PageUnLock() { mutex_.unlock(); }

    void SetDirty(bool is_dirty) { dirty_ = is_dirty; }
private:
    byte *data_{nullptr};
    bool dirty_ {false};
    std::mutex mutex_{};
    State state_{State::INVALID};
};

// class RecvSpace{
// std::unordered_map<uint32_t, std::shared_ptr<SpaceReaderWriter> > space_IOs_;
// std::unordered_map<uint32_t, std::string > spaceID_2_fileName_;
//   pthread_rwlock_t io_rw_lock_;
//   pthread_rwlock_t map_rw_lock_;
// };

typedef std::unordered_map<uint32_t, std::shared_ptr<SpaceReaderWriter> > space_IOs_t;

class BufferPool {
public:


    BufferPool();

    ~BufferPool();

    // 在buffer pool中新建一个page
    Page *NewPage(space_id_t space_id, page_id_t page_id);

    // 从buffer pool中获取一个page，不存在的话从磁盘获取，如果磁盘上也没有，就返回nullptr
    Page *GetPage(space_id_t space_id, page_id_t page_id);

    static void ReleasePage(Page *page) { page->PageUnLock(); }
    bool WriteBack(space_id_t space_id, page_id_t page_id);

    bool WriteBackLock(space_id_t space_id, page_id_t page_id);

    // std::string GetFilename(space_id_t space_id) const {
    //     try {
    //         return space_id_2_file_name_.at(space_id).file_name_;
    //     } catch (std::exception &e) {
    //         return "";
    //     }
    // }

    void CopyPage(void *dest_buf, space_id_t space_id, page_id_t page_id);

    //==add
    std::pair<space_IOs_t::iterator, bool> InsertSpaceid2Filename(space_id_t space_id, std::string file_name);
    void ForceInsertSpaceid2Filename(space_id_t space_id, std::string file_name);
    bool DeleteSpaceidHash(space_id_t space_id);
  private:
    std::list<frame_id_t> lru_list_;

    // [space_id, page_id] -> iterator 快速定位1个page在 LRU 中的位置
    std::unordered_map<space_id_t, std::unordered_map<page_id_t, std::list<frame_id_t>::iterator>> hash_map_;
    Page *buffer_;
    // 数据目录的path
    std::string data_path_;

    // space_id -> file name的映射表
    // std::unordered_map<uint32_t, SpaceReaderWriter> space_id_2_file_name_;
    space_IOs_t space_IOs_;
    pthread_rwlock_t io_rw_lock_;


    // 指示buffer_中哪个frame是可以用的
    std::list<frame_id_t> free_list_;

    std::vector<PageAddressLru> frame_id_2_page_address_;

    // 按照LRU规则淘汰一些页面
    void Evict(int n);

    Page *ReadPageFromDisk(space_id_t space_id, page_id_t page_id);

    pthread_mutex_t lock_;

};

extern BufferPool buffer_pool;

