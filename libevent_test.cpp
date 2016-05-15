/*
The MIT License (MIT)

Copyright (c) 2016 dragon<jianlinlong@gmail.com>
some code from -->Jason Ish https://github.com/jasonish/libevent-examples/blob/master/echo-server/libevent_echosrv2.c

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

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <list>

#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent.h>
#include "xxsocket.h"
#include "ringbuffer.h"

using namespace std;
using namespace purelib::net;

void do_accept(evutil_socket_t listener, short event, void *arg);
void read_cb(struct bufferevent *bev, void *arg);
void error_cb(struct bufferevent *bev, short event, void *arg);
void write_cb(struct bufferevent *bev, void *arg);

void do_accept_simple(evutil_socket_t listener, short event, void *arg);
void read_simple();

//Ring_Buffer *g_buffer = nullptr;
Ring_Buffer_Pool *g_pool = nullptr;

class Client_Manager;
Client_Manager *g_client_manager = nullptr;
//////////////////////////////////////////////////////////////////////////

void do_accept(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = (struct event_base *)arg;
    while (true) {
        evutil_socket_t fd = ::accept(listener, NULL, NULL);
        if (fd < 0) {
            break;
        }
        struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
        bufferevent_setcb(bev, read_cb, NULL, error_cb, arg);
        bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);
    }
}

void read_cb(struct bufferevent *bev, void *arg)
{
    struct evbuffer *buf = bufferevent_get_input(bev);
    bufferevent_write_buffer(bev, buf);
}

void error_cb(struct bufferevent *bev, short event, void *arg)
{
    evutil_socket_t fd = bufferevent_getfd(bev);
    printf("fd = %u, ", fd);
    if (event & BEV_EVENT_TIMEOUT) {
        printf("Timed out\n"); //if bufferevent_set_timeouts() called
    }
    else if (event & BEV_EVENT_EOF) {
        printf("connection closed\n");
    }
    else if (event & BEV_EVENT_ERROR) {
        printf("some other error\n");
    }
    bufferevent_free(bev);
}
//////////////////////////////////////////////////////////////////////////

void read_simple_cb(evutil_socket_t fd, short event, void *arg);
void write_simple_cb(evutil_socket_t fd, short event, void *arg);


struct Write_Buf {
    Pool_Block     block_buf;  //只做echo server, 无需拷贝数据
    char *buf;
    uint len;
    uint offset;
};

struct Client {
    struct event ev_read;
    struct event ev_write;

    //假设消息体的第1个字段是总个包体的长度, int共4个字节
    struct Recv_Buf {
        Pool_Block/*Block* */ buf;
        union {
            unsigned int  command_len;
            char command_bytes[4];
        };
        int  recv_bytes;

        Recv_Buf() {
            reset();
        }

        void reset() {
            //buf = nullptr;
            buf.make_invalid();
            recv_bytes = 0;
            command_len = 0;
        }
    } recv_buf;

    std::list<Write_Buf*> write_list;

    uint access_time;       //最后访问时间
    std::list<Client*>::iterator list_iterator_;
};

int do_process_data(Client *c, evutil_socket_t fd, char *buf, uint buf_len);
void close_client(Client *c);


class Client_Manager {
public:
    ~Client_Manager() {
        for (auto client : online_client_) {
            delete client;
        }
        for (auto client: free_list_) {
            delete client;
        }
    }

public:
    Client* allocate() {
        Client *c = NULL;
        if (!free_list_.empty()) {
            c = free_list_.front();
            free_list_.pop_front();
        } else {
            c = new Client();
        }

        c->list_iterator_ = online_client_.insert(online_client_.end(), c);
        c->access_time = time(NULL);
        return c;
    }

    void free(Client *c) {
        online_client_.erase(c->list_iterator_);
        free_list_.push_front(c);
        if (free_list_.size() > 4 * 1024) {  //这里限制缓存个数
            Client *c = free_list_.back();
            free_list_.pop_back();
            delete c;
        }
    }

    void client_accessed(Client *c) {
        c->access_time = time(NULL);
        /*
        最近访问的客户端后放到尾部, 以便于快速判断socket超时
        这个list维护成本与超时的O(n)相比，谁的成本更高?
        */
        online_client_.erase(c->list_iterator_);
        c->list_iterator_ = online_client_.insert(online_client_.end(), c);
    }

    //to do:: 加入移除超时socket 的功能
    void clear_timeout_client() {
        auto now = time(NULL);
        for (auto it = online_client_.begin(); it != online_client_.end(); ) {
            auto c = *it++;
            if (c->access_time + 3 * 60 < now) { //超时啦
                close_client(c); //好丑的实现方式！现只考虑逻辑正确，其它先不管了
            } else {
                break;
            }
        }
    }

private:
    typedef std::list<Client*> Client_List;
    Client_List online_client_;     //在线用户
    Client_List free_list_;         //缓存
};



void do_accept_simple(evutil_socket_t listener, short event, void *arg)
{
    struct event_base *base = (struct event_base *)arg;
    while (true) {
        evutil_socket_t fd = ::accept(listener, NULL, NULL);
        if (fd < 0) {
            break;
        }
        evutil_make_socket_nonblocking(fd);

        Client *client = g_client_manager->allocate();
        client->recv_buf.reset();
        event_assign(&client->ev_read,  base, fd, EV_READ | EV_PERSIST, read_simple_cb, client);
        event_assign(&client->ev_write, base, fd, EV_WRITE | EV_PERSIST, write_simple_cb, client);
        event_add(&client->ev_read, NULL);
    }
}

void close_client(Client *c)
{
    xxsocket socket((int)c->ev_read.ev_fd);  //for close
    g_pool->free(c->recv_buf.buf);

    for (auto w: c->write_list) {
        g_pool->free(w->block_buf);
        delete w;
    }

    event_del(&c->ev_read);
    event_del(&c->ev_write);
    g_client_manager->free(c);
    socket.close();
}

void read_simple_cb(evutil_socket_t fd, short event, void *arg)
{
    Client *client = (Client*)arg;
    Client::Recv_Buf *recvbuf = &client->recv_buf;
    xxsocket socket((int)fd);
    int err, n, read_len;

    //接收包体长度。假设报文的前4个字节是包体长度；如果是TLV等格式，则需另外修改
    if (recvbuf->recv_bytes < 4) {
        read_len = 4 - recvbuf->recv_bytes;
        n = socket.recv_i(&recvbuf->command_bytes + recvbuf->recv_bytes, read_len);
        if (n <= 0) goto check_disconnect;

        recvbuf->recv_bytes += n;
        if (n != read_len) {
            socket.detach();
            return;
        }

        recvbuf->command_len = ntohl(recvbuf->command_len);
        if (recvbuf->command_len >= 1 * 1024 * 1024 ||  //报文太长了, 关闭socket
            recvbuf->command_len < recvbuf->recv_bytes) {
            n = 0;
            goto check_disconnect;
        }
        recvbuf->buf = /*g_buffer*/g_pool->allocate(recvbuf->command_len);
        if (!recvbuf->buf.is_valid()) {
            //to do::该清理超时的socket啦, 或者需要给g_pool分配更多内存
            //...
            //暂时就将socket关闭
            n = 0;
            goto check_disconnect; 
        }
    }

    g_client_manager->client_accessed(client);

    //n = socket.recv_i(recvbuf->buf->buffer(), recvbuf->command_len - recvbuf->recv_bytes);
    n = socket.recv_i(recvbuf->buf.buffer(), recvbuf->command_len - recvbuf->recv_bytes);
    if (n > 0) {
        if ( (recvbuf->recv_bytes += n) == recvbuf->command_len ) {
            //这里刚好凑够一个报文，不多不少
            //处理消息体: recvbuf->buf->buffer(), recvbuf->command_len
            do_process_data(client, fd, recvbuf->buf.buffer(), recvbuf->command_len);
            if (recvbuf->buf.is_valid()) {
                g_pool->free(recvbuf->buf);
            }
            recvbuf->reset();
#if 0
            //这里使用阻塞模式啦，修改成： https://github.com/jasonish/libevent-examples/blob/master/echo-server/libevent_echosrv2.c
            //int s = socket.send_n(recvbuf->buf->buffer(), recvbuf->command_len, 5);
            int s = socket.send_n(recvbuf->buf.buffer(), recvbuf->command_len, 5);
            if (s + 1 < n) {
                printf("received %d but send %d bytes, something wrong?", n, s + 1);
            }
            /*g_buffer*/g_pool->free(recvbuf->buf);
            recvbuf->reset();
#endif
        }

        socket.detach();
        return;
    }

check_disconnect: 
    if (n == 0) {
        printf("socket disconnected!fd=%d", fd);
    }
    else if (-1 == n) {
        err = socket.get_last_errno();
        if (socket.err_rw_retriable(n)){
            socket.detach();
            return;
        }

        printf("socket failure, close socket %d", fd);
    }
    close_client(client);
    socket.detach();
}

void write_simple_cb(evutil_socket_t fd, short event, void *arg)
{
    // from:: https://github.com/jasonish/libevent-examples/blob/master/echo-server/libevent_echosrv2.c
    Client *client = (Client *)arg;
    std::list<Write_Buf*>& write_list = client->write_list;

    if (write_list.empty()) {
        event_del(&client->ev_write);
        return;
    }

    xxsocket socket((int)fd);
    int len;

    /* Pull the first item off of the write queue. We probably
    * should never see an empty write queue, but make sure the
    * item returned is not NULL. */
    Write_Buf *bufferq = write_list.front();

    /* Write the buffer.  A portion of the buffer may have been
    * written in a previous write, so only write the remaining
    * bytes. */
    len = socket.send_i(bufferq->buf + bufferq->offset, bufferq->len - bufferq->offset);
    socket.detach();
    if (len == -1) {
        int err = socket.get_last_errno();
        if (socket.err_rw_retriable(err)) {
            /* The write was interrupted by a signal or we
            * were not able to write any data to it,
            * reschedule and return. */
            event_add(&client->ev_write, NULL);
            return;
        } else {
            /* Some other socket error occurred, exit. */
            socket = (int)fd;  //for close socket
            close_client(client);
            socket.detach();
            return;
        }
    }
    else if ((bufferq->offset + len) < bufferq->len) {
        /* Not all the data was written, update the offset and
        * reschedule the write event. */
        bufferq->offset += len;
        event_add(&client->ev_write, NULL);
        return;
    }

    /* The data was completely written, remove the buffer from the
    * write queue. */
    g_pool->free(bufferq->block_buf);  //
    delete bufferq;
    write_list.pop_front();
}

int do_process_data(Client *c, evutil_socket_t fd, char *buf, uint buf_len)
{
    Write_Buf *bufferq = new Write_Buf();
    bufferq->buf = buf;
    bufferq->len = buf_len;
    bufferq->block_buf = c->recv_buf.buf;
    c->recv_buf.buf.make_invalid();

    c->write_list.push_back(bufferq);
    event_add(&c->ev_write, NULL);
    return 0;
}

//这里实现定时器
typedef struct {
    struct timeval tv;
    struct event *ev;
} timer_param_t;

void timer_task(int fd, short events, void * ctx) {
    timer_param_t * param = (timer_param_t *)ctx;
    g_client_manager->clear_timeout_client();
    evtimer_add(param->ev, &param->tv);
}

//////////////////////////////////////////////////////////////////////////


int main(int argc, char *argv[])
{
    //g_buffer = new Ring_Buffer(1024 * 1024 * 16);  //16M
    g_pool = new Ring_Buffer_Pool(4, 1024 * 1024 * 16);
    g_client_manager = new Client_Manager();

    xxsocket socket;

    if (!socket.open()) {
        printf("open socket failed!");
        return -1;
    }
    socket.set_optval(SOL_SOCKET, SO_REUSEADDR, 1);

    if (0 != socket.bind("0.0.0.0", 9999)) {
        printf("bind socket failed!");
        return -2;
    }
    if (0 != socket.listen()) {
        printf("listen failed!");
        return -3;
    }

    printf("listening on port 9999");
    socket.set_nonblocking(true);

    struct event_base *base = event_base_new();
    assert(base != NULL);

    //定时器，清时超时的socket
    timer_param_t * param = new timer_param_t();
    param->ev = evtimer_new(base, timer_task, param);
    param->tv.tv_sec = 3 * 60; //每3分钟触发
    evtimer_add(param->ev, &param->tv);

    struct event *listen_event = event_new(base, (evutil_socket_t)socket, EV_READ | EV_PERSIST, do_accept_simple, (void*)base);
    event_add(listen_event, NULL);
    event_base_dispatch(base);

    printf("The End.");
    return 0;
}
