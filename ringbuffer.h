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
ȡ��ACE��round_up, �����ֽڶ���
*/
inline size_t round_up(size_t len, uint ALIGN_WORDB = 8)
{
    return (len + ALIGN_WORDB - 1) & ~(ALIGN_WORDB - 1);
}

/*
��������
*/
inline uint fast_mod(uint x, uint mod)
{
    return x >= mod ? x - mod : x;
    //return x & (mod - 1);   //�����죬Ҫ�� mod ��2��N�η�
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
    //����ṹ���sizeof��8
    uint offset_;       //��ringbuffer�е�λ��
    uint data_len_;     //ռ�ݵĳ��ȣ���roundup��8�ı���; ���λΪ1��ʾ����
    char data[];        //���ݵ�ַ
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
    //���ܼ򵥵��� avaliable_bytes() == length()���ж�ringbuffer����
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
        ע�������>=, ����Ҫ��1�����ӳ�����֤�����write_offset != gc_offset
        write_offset == gc_offset ��ʾringbufferΪ��
        */
        if (block_size >= avaliable_bytes()) {  
            return nullptr;
        }

        //�����������ڴ��, ��������򽫺������Ϊ����
        uint continuous = buf_len_ - write_offset_;
        if (continuous < block_size) {
            assert(continuous >= sizeof(Block));        //Blockֻռ8�ֽڣ���buf_����8�ı���
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
            //�ϲ���������ݿ� ...
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
    uint buf_len_;      //����Ϊ8�ı���
    uint write_offset_; //д��λ��
    uint gc_offset_;    //���յ�λ��
};
//////////////////////////////////////////////////////////////////////////

/*
�� Ring_Buffer_Pool ֮��ÿ��Block��Ҫ��һ��index����ָ����������һ��Ring Buffer�ġ�
������Ч���ƺ��½����٣�
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
    ����Чʱ��˵�����޿ռ�ɷ��䡣
    һ�������Ԥ�Ƶ�buffer̫С��; 
    һ���������Ҫ����ʱ��socket��(��Щ������socket�����ڴ��޷�����)
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
Ring_Buffer������һ����ڴ��pool, ��������һ��pool��Ϊ��̬�����ڴ�
*/
class Ring_Buffer_Pool
{
public:
    Ring_Buffer_Pool(uint ring_buffer_count, uint ring_buffer_bytes) 
        : count_(ring_buffer_count)
        , bytes_(ring_buffer_bytes)
        , index_(0)
    {
        //assert(count_ >= 2);  // ֻ1���ֱ����Ring_Buffer��
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

            //��ǰRing Buffer����, ��ô�����������е�Ring Buffer���ͷŵ�
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
                    break;  //������Ч��Ring Buffer�Ͳ���������
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
