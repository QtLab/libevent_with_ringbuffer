/*
The MIT License (MIT)

Copyright (c) 2016 dragon<jianlinlong@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <assert.h>
#include <memory.h>


#define  uint unsigned int 


/*
取自ACE的round_up, 用于字节对齐
*/
inline size_t round_up(size_t len, uint ALIGN_WORDB = 8)
{
    return (len + ALIGN_WORDB - 1) & ~(ALIGN_WORDB - 1);
}

/*
快速求余
*/
inline uint fast_mod(uint x, uint mod)
{
    return x >= mod ? x - mod : x;
    //return x & (mod - 1);   //这个最快，要求 mod 是2的N次方
    //return x % mod;
}


struct Block {
    inline char* buffer() const {
        return (char*)data;
    }

    inline uint buffer_length() const {
        return data_len_;
    }

private:

    inline uint size() const {
        return buffer_length() + sizeof(Block);
    }

    inline bool is_idle() const {
        return data_len_ & (1 << 31);
    }

    inline void set_idle() {
        data_len_ |= (1 << 31);
    }

    inline uint idle_size() const {
        return idle_buffer_length() + sizeof(Block);
    }

    inline uint idle_buffer_length() const {
        return data_len_ & ~(1 << 31);
    }

private:
    //这个结构体的sizeof是8
    uint offset_;       //在ringbuffer中的位置
    uint data_len_;     //占据的长度，会roundup到8的倍数; 最高位为1表示空闲
    char data[];        //数据地址
    friend class Ring_Buffer;
    friend void ring_buffer_test();
};

class Ring_Buffer
{
public:
#if 0
    Ring_Buffer(char* buf, uint len) {
        init(buf, len);
    }
#endif

    Ring_Buffer(uint len) {
        uint x = round_up(len, 16);
        char *buf = new char[x];
        init(buf, x);
    }

    void init(char *buf, uint len) {
        buf_ = buf;
        buf_len_ = len;
        write_offset_ = gc_offset_ = 0;
    }

    ~Ring_Buffer()
    {
        delete [] buf_;
    }

    inline uint length() const {
        return buf_len_;
    }

    inline const char* buffer_ptr() const {
        return buf_;
    }

#if 0
    //idle := gc_offset_ == write_offset_
    //full := write_offset_ + 1 == gc_offset_
    //不能简单地用 avaliable_bytes() == length()来判断ringbuffer满了
    //inline bool is_full() const {
    //    return false;
    //}
#endif

    inline bool is_empty() const {
        return 0 == used_bytes();
    }

    Block* allocate(uint len) {
        uint size = round_up(len, 8);
        uint block_size = size + sizeof(Block);

        /*
        注意这里的>=, 至少要留1个格子出来保证分配后write_offset != gc_offset
        write_offset == gc_offset 表示ringbuffer为空
        */
        if (block_size >= avaliable_bytes()) {  
            return nullptr;
        }

        //分配连续的内存块, 如果不够则将后面的置为空闲
        uint continuous = buf_len_ - write_offset_;
        if (continuous < block_size) {
            assert(continuous >= sizeof(Block));        //Block只占8字节，而buf_又是8的倍数
            Block *b = allocate_block(continuous - sizeof(Block));
            b->set_idle();
            if (block_size > avaliable_bytes()) {
                return nullptr;
            }
        }

        return allocate_block(size);
    }

    void free(Block *b) {
        if (b->offset_ == gc_offset_) {
            gc_offset_ = fast_mod(gc_offset_ + b->size(), buf_len_);
            //合并后面的数据块 ...
            while (gc_offset_ != write_offset_) {
                Block *b = (Block*)(buf_ + gc_offset_);
                if (b->is_idle()) {
                    gc_offset_ = fast_mod(gc_offset_ + b->idle_size(), buf_len_);
                } else {
                    break;
                }
            }
        } else {
            b->set_idle();
        }
    }

    inline uint avaliable_bytes() const {
        return buf_len_ - used_bytes();
    }

    inline uint used_bytes() const {
        if (write_offset_ >= gc_offset_) {
            return write_offset_ - gc_offset_;
        } else {
            return gc_offset_ + buf_len_ - write_offset_;
        }
    }

private:

    Block* allocate_block(uint size) {
        Block *b = (Block*)(buf_ + write_offset_);
        b->data_len_ = size;
        b->offset_   = write_offset_;
        write_offset_ = fast_mod(write_offset_ + b->size(), buf_len_);
        return b;
    }

private:
    char *buf_;
    uint buf_len_;      //必须为8的倍数
    uint write_offset_; //写的位置
    uint gc_offset_;    //回收的位置
};
//////////////////////////////////////////////////////////////////////////

/*
用 Ring_Buffer_Pool 之后，每个Block需要多一个index用于指出是属于哪一个Ring Buffer的。
这样子效率似乎下降不少？
*/
struct Pool_Block
{
    Pool_Block()
        :Pool_Block(nullptr, -1)
    {}

    Pool_Block(Block* b, uint index)
        : pool_index_(index)
        , block_(b)
    {}

    void make_invalid() {
        pool_index_ = -1;
        block_ = nullptr;
    }

    /*
    当无效时，说明已无空间可分配。
    一种情况是预计的buffer太小了; 
    一种情况是需要清理超时的socket了(这些很慢的socket导致内存无法回收)
    */
    inline bool is_valid() const {
        return nullptr != block_;
    }

    inline char* buffer() const {
        return block_->buffer();
    }

    inline uint buffer_length() const {
        return block_->buffer_length();
    }

private:
    uint   pool_index_;
    Block *block_;
    friend class Ring_Buffer_Pool;
    friend void ring_buffer_pool_test();
};

/*
Ring_Buffer本就是一块大内存的pool, 现在又用一个pool是为动态分配内存
*/
class Ring_Buffer_Pool
{
public:
    Ring_Buffer_Pool(uint ring_buffer_count, uint ring_buffer_bytes) 
        : count_(ring_buffer_count)
        , bytes_(ring_buffer_bytes)
        , index_(0)
    {
        //assert(count_ >= 2);  // 只1块就直接用Ring_Buffer啦
        pool_ = new Ring_Buffer*[count_];
        memset(pool_, 0x00, count_ * sizeof(Ring_Buffer*));
    }

    ~Ring_Buffer_Pool() {
        for (uint i = 0; i < count_; ++i) {
            delete pool_[i];
        }
        delete[] pool_;
    }

    Pool_Block allocate(uint len) {
        Block *b = nullptr;
        uint old_index = index_;
        do 
        {
            if (!pool_[index_]) {
                pool_[index_] = new Ring_Buffer(bytes_);
            }
            b = pool_[index_]->allocate(len);
            if (b) {
                break;
            }
            index_ = fast_mod(index_ + 1, count_);
        } while (index_ != old_index);
        return Pool_Block(b, index_);
    }

    void free(Pool_Block& pb) {
        uint idx = pb.pool_index_;
        if (idx < count_ && pool_[idx]) {
            pool_[idx]->free(pb.block_);

            //当前Ring Buffer空闲, 那么后面连续空闲的Ring Buffer都释放掉
            if (pool_[idx]->is_empty()) {
                uint old_idx = idx;
                while ( (idx = fast_mod(idx + 1, count_)) != old_idx ) {
                    if (!pool_[idx] || idx == index_) {
                        continue;
                    }
                    if (pool_[idx]->is_empty()) {
                        delete pool_[idx];
                        pool_[idx] = nullptr;
                        continue;
                    }
                    break;  //遇到有效的Ring Buffer就不再清理了
                }
            }
        }
        pb.make_invalid();
    }

private:
    uint count_;
    uint bytes_;
    uint index_;
    Ring_Buffer **pool_;
};
//////////////////////////////////////////////////////////////////////////

void ring_buffer_test() {
#if 0
    Block block;
    auto b1 = block.is_idle();
    block.set_idle();
    auto b2 = block.is_idle();

    Ring_Buffer buffer(200);
    Block *x1 = buffer.allocate(8);
    Block *x2 = buffer.allocate(5);
    Block *x4 = buffer.allocate(18);
    Block *x3 = buffer.allocate(12);

    auto s1 = buffer.used_bytes();
    auto s2 = buffer.avaliable_bytes();

    buffer.free(x1);
    buffer.free(x3);
    buffer.free(x2);
    buffer.free(x4);

    auto s1x = buffer.used_bytes();
    auto s2x = buffer.avaliable_bytes();
#endif
}

void ring_buffer_pool_test() {
#if 0
    Ring_Buffer_Pool pool(4, 200);
    Pool_Block px1 = pool.allocate(100);
    Pool_Block px2 = pool.allocate(100);
    Pool_Block px3 = pool.allocate(100);
    Pool_Block px4 = pool.allocate(100);

    Pool_Block pxx1 = pool.allocate(60);
    Pool_Block pxx2 = pool.allocate(60);
    Pool_Block pxx3 = pool.allocate(60);
    Pool_Block pxx4 = pool.allocate(60);

    pool.free(pxx2);
    pool.free(pxx1);
    pool.free(pxx4);
    pool.free(pxx3);

    pool.free(px1);
    pool.free(px3);
    pool.free(px4);
    pool.free(px2);

    //uint u1 = pool.pool_[0]->used_bytes();
    //uint u2 = pool.pool_[1]->used_bytes();
    //uint u3 = pool.pool_[2]->used_bytes();
    //uint u4 = pool.pool_[3]->used_bytes();
#endif
}

#endif
