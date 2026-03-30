/**
 * @file buffer_pool.cpp
 * @brief Buffer pool manager implementation
 */

#include "buffer/buffer_pool.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace agent_mem_io {

// =============================================================================
// AlignedBuffer Implementation
// =============================================================================

AlignedBuffer::AlignedBuffer(Size size)
    : data_(nullptr)
    , size_(size)
{
    if (size > 0) {
        // Round up to alignment
        Size aligned_size = ((size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;
        
        void* ptr = nullptr;
        int ret = posix_memalign(&ptr, ALIGNMENT, aligned_size);
        if (ret != 0) {
            throw std::bad_alloc();
        }
        
        data_ = static_cast<char*>(ptr);
        size_ = aligned_size;
        std::memset(data_, 0, size_);
    }
}

AlignedBuffer::~AlignedBuffer() {
    if (data_) {
        free(data_);
        data_ = nullptr;
    }
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : data_(other.data_)
    , size_(other.size_)
{
    other.data_ = nullptr;
    other.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
        if (data_) {
            free(data_);
        }
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

// =============================================================================
// TwoQueueEvictionPolicy Implementation
// =============================================================================

TwoQueueEvictionPolicy::TwoQueueEvictionPolicy(Size max_pages, double hot_queue_ratio)
    : max_pages_(max_pages)
    , hot_queue_max_size_(static_cast<Size>(max_pages * hot_queue_ratio))
{
}

void TwoQueueEvictionPolicy::on_access(PageId page_id, uint32_t in_degree) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Update in-degree
    in_degree_map_[page_id] = in_degree;
    
    // If in warm queue, promote to hot queue
    if (is_in_warm_queue(page_id)) {
        promote_to_hot_queue(page_id);
    } else if (is_in_hot_queue(page_id)) {
        // Move to front of hot queue (LRU)
        hot_queue_.erase(hot_queue_map_[page_id]);
        hot_queue_.push_front(page_id);
        hot_queue_map_[page_id] = hot_queue_.begin();
    }
}

void TwoQueueEvictionPolicy::on_add(PageId page_id, uint32_t in_degree) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Add to warm queue (new pages go to warm queue first)
    warm_queue_.push_back(page_id);
    warm_queue_map_[page_id] = std::prev(warm_queue_.end());
    in_degree_map_[page_id] = in_degree;
}

void TwoQueueEvictionPolicy::on_remove(PageId page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_in_hot_queue(page_id)) {
        hot_queue_.erase(hot_queue_map_[page_id]);
        hot_queue_map_.erase(page_id);
    } else if (is_in_warm_queue(page_id)) {
        warm_queue_.erase(warm_queue_map_[page_id]);
        warm_queue_map_.erase(page_id);
    }
    
    in_degree_map_.erase(page_id);
}

PageId TwoQueueEvictionPolicy::get_eviction_candidate() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Try to evict from warm queue first (FIFO)
    if (!warm_queue_.empty()) {
        PageId candidate = warm_queue_.front();
        
        // Check if hot queue is over capacity
        if (hot_queue_.size() >= hot_queue_max_size_) {
            // Need to evict from hot queue instead
            return evict_from_hot_queue();
        }
        
        warm_queue_.pop_front();
        warm_queue_map_.erase(candidate);
        in_degree_map_.erase(candidate);
        return candidate;
    }
    
    // Fall back to hot queue
    if (!hot_queue_.empty()) {
        return evict_from_hot_queue();
    }
    
    return INVALID_PAGE_ID;
}

bool TwoQueueEvictionPolicy::is_in_hot_queue(PageId page_id) const {
    return hot_queue_map_.find(page_id) != hot_queue_map_.end();
}

bool TwoQueueEvictionPolicy::is_in_warm_queue(PageId page_id) const {
    return warm_queue_map_.find(page_id) != warm_queue_map_.end();
}

Size TwoQueueEvictionPolicy::hot_queue_size() const {
    return hot_queue_.size();
}

Size TwoQueueEvictionPolicy::warm_queue_size() const {
    return warm_queue_.size();
}

void TwoQueueEvictionPolicy::update_in_degree(PageId page_id, uint32_t in_degree) {
    std::lock_guard<std::mutex> lock(mutex_);
    in_degree_map_[page_id] = in_degree;
}

void TwoQueueEvictionPolicy::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    hot_queue_.clear();
    hot_queue_map_.clear();
    warm_queue_.clear();
    warm_queue_map_.clear();
    in_degree_map_.clear();
}

void TwoQueueEvictionPolicy::promote_to_hot_queue(PageId page_id) {
    // Remove from warm queue
    warm_queue_.erase(warm_queue_map_[page_id]);
    warm_queue_map_.erase(page_id);
    
    // Add to hot queue
    hot_queue_.push_front(page_id);
    hot_queue_map_[page_id] = hot_queue_.begin();
    
    // If hot queue is over capacity, demote oldest to warm queue
    if (hot_queue_.size() > hot_queue_max_size_) {
        PageId demoted = hot_queue_.back();
        hot_queue_.pop_back();
        hot_queue_map_.erase(demoted);
        
        warm_queue_.push_back(demoted);
        warm_queue_map_[demoted] = std::prev(warm_queue_.end());
    }
}

PageId TwoQueueEvictionPolicy::evict_from_hot_queue() {
    if (hot_queue_.empty()) {
        return INVALID_PAGE_ID;
    }
    
    // Graph-aware: prefer to evict low in-degree nodes
    PageId best_candidate = INVALID_PAGE_ID;
    uint32_t min_in_degree = UINT32_MAX;
    
    // Check last few entries (LRU order)
    auto it = hot_queue_.end();
    for (int i = 0; i < 10 && it != hot_queue_.begin(); ++i) {
        --it;
        PageId candidate = *it;
        uint32_t in_deg = in_degree_map_[candidate];
        if (in_deg < min_in_degree) {
            min_in_degree = in_deg;
            best_candidate = candidate;
        }
    }
    
    if (best_candidate != INVALID_PAGE_ID) {
        hot_queue_.erase(hot_queue_map_[best_candidate]);
        hot_queue_map_.erase(best_candidate);
        in_degree_map_.erase(best_candidate);
    }
    
    return best_candidate;
}

// =============================================================================
// BufferPoolManager Implementation
// =============================================================================

BufferPoolManager::BufferPoolManager(const BufferPoolConfig& config)
    : config_(config)
{
    // Initialize frames
    frames_.resize(config_.max_pages);
    
    // Initialize free frame list
    for (int i = static_cast<int>(config_.max_pages) - 1; i >= 0; --i) {
        free_frames_.push_back(i);
    }
    
    // Initialize eviction policy
    eviction_policy_ = std::make_unique<TwoQueueEvictionPolicy>(
        config_.max_pages,
        config_.hot_queue_ratio
    );
}

BufferPoolManager::~BufferPoolManager() {
    // Free all allocated buffers
    for (auto& frame : frames_) {
        if (frame.data) {
            free_aligned_buffer(frame.data);
        }
    }
}

char* BufferPoolManager::get_page(NodeId node_id, PageId page_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return nullptr;
    }
    
    int frame_idx = it->second;
    PageFrame& frame = frames_[frame_idx];
    
    // Update access statistics
    frame.access_count++;
    frame.last_access_time = std::chrono::system_clock::now().time_since_epoch().count();
    
    // Notify eviction policy
    eviction_policy_->on_access(page_id, frame.in_degree);
    
    hit_count_++;
    return frame.data;
}

char* BufferPoolManager::get_or_load_page(
    NodeId node_id,
    PageId page_id,
    std::function<void(char* buffer, PageId page_id)> load_func
) {
    // Try to get existing page
    char* data = get_page(node_id, page_id);
    if (data) {
        return data;
    }
    
    // Need to load page
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    // Double-check after acquiring write lock
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        miss_count_++;
        return frames_[it->second].data;
    }
    
    // Find a free frame
    int frame_idx = find_free_frame();
    if (frame_idx < 0) {
        // Need to evict
        if (!evict_page()) {
            miss_count_++;
            return nullptr;  // Buffer pool full
        }
        frame_idx = find_free_frame();
    }
    
    // Allocate buffer if needed
    PageFrame& frame = frames_[frame_idx];
    if (!frame.data) {
        frame.data = allocate_aligned_buffer(config_.page_size);
    }
    
    // Load data
    load_func(frame.data, page_id);
    
    // Update frame metadata
    frame.page_id = page_id;
    frame.node_id = node_id;
    frame.is_valid = true;
    frame.is_dirty = false;
    frame.access_count = 1;
    frame.last_access_time = std::chrono::system_clock::now().time_since_epoch().count();
    
    // Add to page table
    page_table_[page_id] = frame_idx;
    
    // Notify eviction policy
    eviction_policy_->on_add(page_id, frame.in_degree);
    
    miss_count_++;
    return frame.data;
}

bool BufferPoolManager::pin_page(PageId page_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    PageFrame& frame = frames_[it->second];
    frame.is_pinned = true;
    frame.pin_count++;
    return true;
}

bool BufferPoolManager::unpin_page(PageId page_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }
    
    PageFrame& frame = frames_[it->second];
    if (frame.pin_count > 0) {
        frame.pin_count--;
    }
    if (frame.pin_count == 0) {
        frame.is_pinned = false;
    }
    return true;
}

void BufferPoolManager::mark_dirty(PageId page_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frames_[it->second].is_dirty = true;
    }
}

Size BufferPoolManager::flush_dirty_pages(
    std::function<void(const char* buffer, PageId page_id)> flush_func
) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    Size count = 0;
    for (auto& frame : frames_) {
        if (frame.is_valid && frame.is_dirty && flush_func) {
            flush_func(frame.data, frame.page_id);
            frame.is_dirty = false;
            count++;
        }
    }
    return count;
}

bool BufferPoolManager::contains(PageId page_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return page_table_.find(page_id) != page_table_.end();
}

Size BufferPoolManager::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return page_table_.size();
}

Size BufferPoolManager::max_size() const {
    return config_.max_pages;
}

double BufferPoolManager::get_hit_rate() const {
    uint64_t total = hit_count_ + miss_count_;
    if (total == 0) return 0.0;
    return static_cast<double>(hit_count_) / static_cast<double>(total);
}

uint64_t BufferPoolManager::get_hit_count() const {
    return hit_count_;
}

uint64_t BufferPoolManager::get_miss_count() const {
    return miss_count_;
}

void BufferPoolManager::reset_stats() {
    hit_count_ = 0;
    miss_count_ = 0;
}

Size BufferPoolManager::get_memory_usage() const {
    return size() * config_.page_size;
}

void BufferPoolManager::update_in_degree(NodeId node_id, uint32_t in_degree) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    PageId page_id = calculate_page_id(node_id);
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frames_[it->second].in_degree = in_degree;
        eviction_policy_->update_in_degree(page_id, in_degree);
    }
}

void BufferPoolManager::prefetch_pages(
    const std::vector<PageId>& page_ids,
    std::function<void(char* buffer, PageId page_id)> load_func
) {
    for (PageId page_id : page_ids) {
        if (!contains(page_id)) {
            get_or_load_page(INVALID_NODE_ID, page_id, load_func);
        }
    }
}

void BufferPoolManager::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    
    page_table_.clear();
    
    for (auto& frame : frames_) {
        frame.is_valid = false;
        frame.is_dirty = false;
        frame.pin_count = 0;
        frame.is_pinned = false;
    }
    
    free_frames_.clear();
    for (int i = static_cast<int>(config_.max_pages) - 1; i >= 0; --i) {
        free_frames_.push_back(i);
    }
    
    eviction_policy_->clear();
    reset_stats();
}

char* BufferPoolManager::allocate_aligned_buffer(Size size) {
    void* ptr = nullptr;
    int ret = posix_memalign(&ptr, ALIGNMENT, size);
    if (ret != 0) {
        throw std::bad_alloc();
    }
    return static_cast<char*>(ptr);
}

void BufferPoolManager::free_aligned_buffer(char* buffer) {
    if (buffer) {
        free(buffer);
    }
}

int BufferPoolManager::find_free_frame() {
    if (free_frames_.empty()) {
        return -1;
    }
    int frame_idx = free_frames_.back();
    free_frames_.pop_back();
    return frame_idx;
}

bool BufferPoolManager::evict_page(
    std::function<void(const char* buffer, PageId page_id)> flush_func
) {
    PageId victim = eviction_policy_->get_eviction_candidate();
    if (victim == INVALID_PAGE_ID) {
        return false;
    }
    
    auto it = page_table_.find(victim);
    if (it == page_table_.end()) {
        return false;
    }
    
    int frame_idx = it->second;
    PageFrame& frame = frames_[frame_idx];
    
    // Don't evict pinned pages
    if (frame.is_pinned) {
        return false;
    }
    
    // Flush if dirty
    if (frame.is_dirty && flush_func) {
        flush_func(frame.data, frame.page_id);
    }
    
    // Remove from page table
    page_table_.erase(victim);
    
    // Reset frame
    frame.is_valid = false;
    frame.is_dirty = false;
    frame.page_id = INVALID_PAGE_ID;
    frame.node_id = INVALID_NODE_ID;
    
    // Add back to free list
    free_frames_.push_back(frame_idx);
    
    return true;
}

}  // namespace agent_mem_io