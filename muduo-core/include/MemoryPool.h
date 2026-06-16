#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "noncopyable.h"

/**
 * MemoryPool —— 线程局部固定大小内存块池
 *
 * 设计要点：
 *  - 单线程使用（每个 EventLoop 持有一个自己的 MemoryPool），无需锁
 *  - 空闲块通过侵入式链表（intrusive free-list）管理
 *  - 分配时优先从 free-list 取，free-list 为空时 malloc 新块
 *  - 释放的块直接挂回 free-list，不归还给系统（减少 malloc 调用）
 *  - 每块按 CACHE_LINE 对齐，避免 false sharing
 *
 * 使用场景：
 *  替换 Buffer 底层 std::vector<char> 的动态分配，减少 malloc/free 次数
 */
class MemoryPool : noncopyable
{
public:
    static const size_t CACHE_LINE = 64;

    explicit MemoryPool(size_t blockSize = 65536)  // 默认 64KB
        : blockSize_(alignSize(blockSize))
        , freeList_(nullptr)
        , totalBlocks_(0)
    {
    }

    ~MemoryPool()
    {
        // 回收所有已分配的 block（遍历 free-list 和 allocated-list）
        Block *cur = freeList_;
        while (cur)
        {
            Block *next = cur->next;
            std::free(static_cast<void *>(cur));
            cur = next;
        }
        cur = allocatedList_;
        while (cur)
        {
            Block *next = cur->next;
            std::free(static_cast<void *>(cur));
            cur = next;
        }
    }

    // 分配一个 block，返回对齐后的地址
    void *allocate()
    {
        if (freeList_)
        {
            Block *block = freeList_;
            freeList_ = block->next;
            block->next = nullptr;
            // 加入 allocated-list 便于析构时统一回收
            block->nextAllocated = allocatedList_;
            allocatedList_ = block;
            return reinterpret_cast<char *>(block) + sizeof(Block);
        }

        void *raw = std::malloc(blockSize_);
        if (!raw)
        {
            return nullptr;
        }
        // 用 block 头部的空间存放 Block 元数据（在分配出的内存前段）
        Block *block = static_cast<Block *>(raw);
        block->next = nullptr;
        block->nextAllocated = allocatedList_;
        allocatedList_ = block;
        ++totalBlocks_;
        return reinterpret_cast<char *>(raw) + sizeof(Block);
    }

    // 归还 block 到 free-list
    void deallocate(void *ptr)
    {
        if (!ptr)
        {
            return;
        }
        char *raw = reinterpret_cast<char *>(ptr) - sizeof(Block);
        Block *block = reinterpret_cast<Block *>(raw);

        // 从 allocated-list 中移除
        removeFromAllocated(block);

        // 挂回 free-list
        block->next = freeList_;
        block->nextAllocated = nullptr;
        freeList_ = block;
    }

    size_t blockSize() const { return blockSize_; }
    size_t totalBlocks() const { return totalBlocks_; }

private:
    // 侵入式块元数据 —— 放在每个分配块的开头
    struct Block
    {
        Block *next;            // free-list / allocated-list 指针
        Block *nextAllocated;   // 仅 allocated-list 使用
    };

    static size_t alignSize(size_t size)
    {
        return (size + CACHE_LINE - 1) & ~(CACHE_LINE - 1);
    }

    void removeFromAllocated(Block *target)
    {
        Block **pp = &allocatedList_;
        while (*pp)
        {
            if (*pp == target)
            {
                *pp = target->nextAllocated;
                return;
            }
            pp = &((*pp)->nextAllocated);
        }
    }

    const size_t blockSize_;
    Block *freeList_;        // 空闲块链表
    Block *allocatedList_;   // 已分配块链表（仅用于析构时回收）
    size_t totalBlocks_;     // 历史上总共分配过的块数（不含归还）
};
