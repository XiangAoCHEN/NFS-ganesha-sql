#include <cstdio>
#include <iostream>
#include <memory>
#include <random>
#include <cstring>
#include <cassert>
#include <utility>
#include "applier/buffer_pool.h"
#include "applier/log_log.h"

Page::Page() :
        data_(new unsigned char[DATA_PAGE_SIZE]),
        state_(State::INVALID) {

}

Page::~Page() {
    if (data_ != nullptr) {
        delete[] data_;
        data_ = nullptr;
    }
}

void Page::WriteCheckSum(uint32_t checksum) {
    mach_write_to_4(data_ + FIL_PAGE_SPACE_OR_CHKSUM, checksum);
    mach_write_to_4(data_ + DATA_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM, checksum);
    dirty_ = true;
}

Page::Page(const Page &other) :
        data_(new unsigned char[DATA_PAGE_SIZE]),
        state_(other.state_) {

    std::memcpy(data_, other.data_, DATA_PAGE_SIZE);
}

BufferPool::BufferPool() :
        lru_list_(),
        hash_map_(),
        buffer_(new Page[BUFFER_POOL_SIZE]),
        data_path_(DATA_FILE_PREFIX),
        space_id_2_file_name_(),
        recv_spaces_(),
        free_list_(), frame_id_2_page_address_(BUFFER_POOL_SIZE), lock_() {

    pthread_mutex_init(&lock_, nullptr);

    // 1. 构建映射表
    // std::vector<std::string> filenames;
    // TravelDirectory(data_path_, ".ibd", filenames);
    // byte page_buf[DATA_PAGE_SIZE];
    // std::ifstream ifs;
    // for (auto & filename : filenames) {
    //     ifs.open(filename, std::ios::binary | std::ios::in);
    //     ifs.read(reinterpret_cast<char *>(page_buf), DATA_PAGE_SIZE);
    //     uint32_t space_id = mach_read_from_4(page_buf + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
    //     ifs.close();
    //     std::cout << filename << "-> space_id: " << space_id << std::endl;
    //     space_id_2_file_name_.insert({space_id, PageReaderWriter(filename)});

    //     std::string ibd_name = filename;
    //     ibd_name = ibd_name.substr(ibd_name.rfind('/') + 1);
    //     DataPageGroup::Get().Insert(ibd_name, space_id);
    // }

    // 2. 初始化free_list_
    for (int i = 0; i < static_cast<int>(BUFFER_POOL_SIZE); ++i) {
        free_list_.emplace_back(i);
    }

}
// extract *.idb file name
std::string extractFilename(const std::string &fullPath) {
  // Find the last occurrence of '/' or '\' (platform-independent)
  size_t pos = fullPath.find_last_of("/\\");

  // If the path contains no '/' or '\', return the original string
  if (pos == std::string::npos) {
    return fullPath;
  }

  // Extract the filename part starting from the character after the last '/'
  return fullPath.substr(pos + 1);
}
std::pair<recv_spaces_t::iterator, bool> BufferPool::InsertSpaceid2Filename(space_id_t space_id, std::string file_name) {
  
  // <map iterator, insert_flag>
  // if key exists, insert_flag == false
    std::pair<recv_spaces_t::iterator, bool> p =
        recv_spaces_.insert(std::make_pair(
            space_id, std::make_shared<PageReaderWriter>(file_name)));

#ifdef DEBUG_MLOG_FILE_NAME
    //print
    for(auto it:recv_spaces_){
        printf("%d -> %s\n",it.first,it.second->file_name_.c_str());
    }
#endif
    return p;
}
void BufferPool::ForceInsertSpaceid2Filename
(   space_id_t space_id, 
    std::string file_name)
{
    // absolute file path
    std::string abs_file_path(DATA_FILE_PREFIX);
    std::string newFilename = extractFilename(file_name); // extract *.ibd
    abs_file_path.append("/").append(newFilename);

    PthreadMutexGuard guard(lock_);
    
    // filename -> spaceid
    DataPageGroup::Get().Insert(abs_file_path, space_id);

    // spaceid -> filename
    recv_spaces_[space_id] =
        std::make_shared<PageReaderWriter>(abs_file_path);

    //     auto it = recv_spaces_.find(space_id);
    //     if(it==recv_spaces_.end()){// key donot exist
    // #ifdef DEBUG_MLOG_FILE_NAME
    //       printf("** insert a new spaceid->filename\n");
    // #endif
    //       // This will insert a new key-value pair if the key doesn't exist
    //       // and have no effect if the key already exists.
    //       recv_spaces_.emplace(space_id, PageReaderWriter(abs_file_path));
    //     }else{
    // #ifdef DEBUG_MLOG_FILE_NAME
    //       printf("** update an existing spaceid->filename\n");
    // #endif
    //       // This will update the value if the key exists
    //       // or insert a new key-value pair if it doesn't exist.
    //       recv_spaces_[space_id] = PageReaderWriter(abs_file_path);
    //     }
    // #ifdef DEBUG_MLOG_FILE_NAME
    //     // print
    //     printf("** print recv_spaces_: size=%ld pair\n",recv_spaces_.size());
    //     for (auto it : recv_spaces_) {
    //         printf("** %d -> %s\n", it.first, it.second.file_name_.c_str());
    //     }
    // #endif
}
bool BufferPool::DeleteSpaceidHash(space_id_t space_id) {
    if(recv_spaces_.find(space_id)!=recv_spaces_.end()){
        int delete_num = recv_spaces_.erase(space_id);
        std::cout<<"*** delete_num="<<delete_num<<"\n";
        if(delete_num==1)return true;
    }
    return false;
}
BufferPool::~BufferPool() {
  if (buffer_ != nullptr) {
    delete[] buffer_;
    buffer_ = nullptr;
  }
  pthread_mutex_unlock(&lock_);
}

Page *BufferPool::NewPage(space_id_t space_id, page_id_t page_id) {
    PthreadMutexGuard guard(lock_);
    if (hash_map_.find(space_id) != hash_map_.end()
        && hash_map_[space_id].find(page_id) != hash_map_[space_id].end()) {
        std::cerr << "the page(space_id = " << space_id
                  << ", page_id = " << page_id << ") was already in buffer pool"
                  << std::endl;
        return nullptr;
    }
    if (free_list_.empty()) {
        // buffer pool 空间不够
        Evict(64);
    }
    // 从free list申请一个buffer frame
    frame_id_t frame_id = free_list_.front();
    free_list_.pop_front();

    assert(frame_id_2_page_address_[frame_id].in_lru_ == false);

    // 初始化申请到的buffer frame
    buffer_[frame_id].Reset();
    buffer_[frame_id].SetState(Page::State::FROM_BUFFER);

    // 新创建的page加入lru list
    lru_list_.emplace_front(frame_id);
    hash_map_[space_id][page_id] = lru_list_.begin();

    // 初始化frame_id_2_page_address_
    frame_id_2_page_address_[frame_id].in_lru_ = true;
    frame_id_2_page_address_[frame_id].space_id_ = space_id;
    frame_id_2_page_address_[frame_id].page_id_ = page_id;

    auto *page = &buffer_[frame_id];
    page->PageLock();
    return page;
}

void BufferPool::Evict(int n) {
    for (int i = 0; i < n; ++i) {
        if (lru_list_.empty()) return;
        // 把 buffer frame 从 LRU List 中移除
        frame_id_t frame_id = lru_list_.back();
        space_id_t space_id = frame_id_2_page_address_[frame_id].space_id_;
        page_id_t page_id = frame_id_2_page_address_[frame_id].page_id_;

        // 写回
        Page &page = buffer_[frame_id];
        page.PageLock();
        if (page.dirty_) {
            WriteBack(space_id, page_id);
        }
        page.PageUnLock();
        lru_list_.pop_back();
        assert(frame_id_2_page_address_[frame_id].in_lru_ == true);
        frame_id_2_page_address_[frame_id].in_lru_ = false;

        hash_map_[space_id].erase(page_id);

        // 把 buffer frame 归还 Free List
        free_list_.push_back(frame_id);

        page.SetState(Page::State::INVALID);
    }
}

Page *BufferPool::GetPage(space_id_t space_id, page_id_t page_id) {
    PthreadMutexGuard guard(lock_);
    // if (space_id_2_file_name_.find(space_id) == space_id_2_file_name_.end()) {
    //     std::cerr << "invalid space_id(" << space_id << ")" << std::endl;
    //     return nullptr;
    // }
    if (recv_spaces_.find(space_id) == recv_spaces_.end()) {
      std::cerr << "invalid space_id(" << space_id << ")" << std::endl;
      return nullptr;
    }

    // 该 page 已经被lru缓存了
    if (hash_map_.find(space_id) != hash_map_.end()
        && hash_map_[space_id].find(page_id) != hash_map_[space_id].end()) {
        auto iter = hash_map_[space_id][page_id];
        auto frame_id = *hash_map_[space_id][page_id];

        // 提升到lru list的队头
        lru_list_.erase(iter);
        lru_list_.emplace_front(frame_id);
        hash_map_[space_id][page_id] = lru_list_.begin();

        auto *page = &buffer_[frame_id];
        page->PageLock();
        return page;
    }

    // 不在buffer pool中，从磁盘读
    // TODO 假定所有的Page在磁盘上都是存在的
    return ReadPageFromDisk(space_id, page_id);
}

Page *BufferPool::ReadPageFromDisk(space_id_t space_id, page_id_t page_id) {

    if (free_list_.empty()) {
        // buffer pool 空间不够
        Evict(64);
    }

    // 从free list中分配一个frame，从磁盘读取page，填充这个frame
    frame_id_t frame_id = free_list_.front();
    // auto fs = space_id_2_file_name_[space_id].stream_;
    // auto fs = recv_spaces_[space_id].stream_;

    // assert(fs->is_open());
    assert(recv_spaces_[space_id]->canOperate());
    // fs->seekg(0, std::ios_base::end);
    // auto max_page_id = (fs->tellg() / DATA_PAGE_SIZE) - 1;
    auto max_page_id = recv_spaces_[space_id]->get_max_page_id(DATA_PAGE_SIZE);

    // 磁盘上还没有这个page
    if (page_id > max_page_id) {
//        assert(false);
        return nullptr;
    }

    // fs->seekg(static_cast<std::streamoff>(page_id * DATA_PAGE_SIZE), std::ios::beg);
    // fs->read(reinterpret_cast<char *>(buffer_[frame_id].GetData()), DATA_PAGE_SIZE);
    recv_spaces_[space_id]->read(
        reinterpret_cast<char *>(buffer_[frame_id].GetData()), page_id,
        DATA_PAGE_SIZE);
    buffer_[frame_id].SetState(Page::State::FROM_DISK);
    free_list_.pop_front();

    assert(frame_id_2_page_address_[frame_id].in_lru_ == false);

    // 将page放入lru list
    frame_id_2_page_address_[frame_id].space_id_ = space_id;
    frame_id_2_page_address_[frame_id].page_id_ = page_id;
    frame_id_2_page_address_[frame_id].in_lru_ = true;
    lru_list_.emplace_front(frame_id);
    hash_map_[space_id][page_id] = lru_list_.begin();
    auto *page = &buffer_[frame_id];
    page->PageLock();
    return page;
}

bool BufferPool::WriteBack(space_id_t space_id, page_id_t page_id) {
    // 找找看是不是在buffer pool中
    if (hash_map_.find(space_id) != hash_map_.end() && hash_map_[space_id].find(page_id) != hash_map_[space_id].end()) {
        frame_id_t frame_id = *(hash_map_[space_id][page_id]);

        auto *page_data = buffer_[frame_id].GetData();
        assert(mach_read_from_4(page_data + FIL_PAGE_OFFSET) == page_id);
        // auto fs = space_id_2_file_name_[space_id].stream_;
        // auto fs = recv_spaces_[space_id].stream_;
        // fs->seekp(static_cast<std::streamoff>(page_id * DATA_PAGE_SIZE));
        // fs->write(reinterpret_cast<char *>(page_data), DATA_PAGE_SIZE);
        recv_spaces_[space_id]->write(reinterpret_cast<char *>(page_data),
                                     page_id, DATA_PAGE_SIZE);
        return true;
    }
    return false;
}

bool BufferPool::WriteBackLock(space_id_t space_id, page_id_t page_id) {
    PthreadMutexGuard guard(lock_);
    // 找找看是不是在buffer pool中
    if (hash_map_.find(space_id) != hash_map_.end() && hash_map_[space_id].find(page_id) != hash_map_[space_id].end()) {
        frame_id_t frame_id = *(hash_map_[space_id][page_id]);
        auto *page_data = buffer_[frame_id].GetData();
        assert(mach_read_from_4(page_data + FIL_PAGE_OFFSET) == page_id);
        recv_spaces_[space_id]->write(reinterpret_cast<char *>(page_data),
                                     page_id, DATA_PAGE_SIZE);
        // auto fs = space_id_2_file_name_[space_id].stream_;
        // auto fs = recv_spaces_[space_id].stream_;
        // fs->seekp(static_cast<std::streamoff>(page_id * DATA_PAGE_SIZE));
        // fs->write(reinterpret_cast<char *>(buffer_[frame_id].GetData()),
        // DATA_PAGE_SIZE);
        return true;
    }
    return false;
}

void BufferPool::CopyPage(void *dest_buf, space_id_t space_id, page_id_t page_id) {
    Page *page = GetPage(space_id, page_id);
    std::memcpy(dest_buf, page->data_, DATA_PAGE_SIZE);
    ReleasePage(page);
}

BufferPool buffer_pool;

