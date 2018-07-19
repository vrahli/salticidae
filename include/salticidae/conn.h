/**
 * Copyright (c) 2018 Cornell University.
 *
 * Author: Ted Yin <tederminant@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SALTICIDAE_CONN_H
#define _SALTICIDAE_CONN_H

#include <cassert>
#include <cstdint>
#include <event2/event.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <exception>

#include "salticidae/type.h"
#include "salticidae/ref.h"
#include "salticidae/event.h"
#include "salticidae/util.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"

namespace salticidae {

class RingBuffer {
    struct buffer_entry_t {
        bytearray_t data;
        bytearray_t::iterator offset;
        buffer_entry_t(bytearray_t &&_data): data(std::move(_data)) {
            offset = data.begin();
        }

        buffer_entry_t(buffer_entry_t &&other) {
            size_t _offset = other.offset - other.data.begin();
            data = std::move(other.data);
            offset = data.begin() + _offset;
        }

        buffer_entry_t(const buffer_entry_t &other): data(other.data) {
            offset = data.begin() + (other.offset - other.data.begin());
        }

        size_t length() const { return data.end() - offset; }
    };

    std::list<buffer_entry_t> ring;
    size_t _size;

    public:
    RingBuffer(): _size(0) {}
    ~RingBuffer() { clear(); }

    void swap(RingBuffer &other) {
        std::swap(ring, other.ring);
        std::swap(_size, other._size);
    }

    RingBuffer(const RingBuffer &other):
        ring(other.ring), _size(other._size) {}

    RingBuffer(RingBuffer &&other):
        ring(std::move(other.ring)), _size(other._size) {
        other._size = 0;
    }

    RingBuffer &operator=(RingBuffer &&other) {
        if (this != &other)
        {
            RingBuffer tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }
 
    RingBuffer &operator=(const RingBuffer &other) {
        if (this != &other)
        {
            RingBuffer tmp(other);
            tmp.swap(*this);
        }
        return *this;
    }
   
    void push(bytearray_t &&data) {
        _size += data.size();
        ring.push_back(buffer_entry_t(std::move(data)));
    }
    
    bytearray_t pop(size_t len) {
        bytearray_t res;
        auto i = ring.begin();
        while (len && i != ring.end())
        {
            size_t copy_len = std::min(i->length(), len);
            res.insert(res.end(), i->offset, i->offset + copy_len);
            i->offset += copy_len;
            len -= copy_len;
            if (i->offset == i->data.end())
                i++;
        }
        ring.erase(ring.begin(), i);
        _size -= res.size();
        return std::move(res);
    }
    
    size_t size() const { return _size; }
    
    void clear() {
        ring.clear();
        _size = 0;
    }
};

class ConnPoolError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** The connection pool. */
class ConnPool {
    public:
    class Conn;
    using conn_t = RcObj<Conn>;
    /** The abstraction for a bi-directional connection. */
    class Conn {
        public:
        enum ConnMode {
            ACTIVE, /**< the connection is established by connect() */
            PASSIVE, /**< the connection is established by accept() */
        };
    
        private:
        size_t seg_buff_size;
        conn_t self_ref;
        int fd;
        ConnPool *cpool;
        ConnMode mode;
        NetAddr addr;

        RingBuffer send_buffer;
        RingBuffer recv_buffer;

        Event ev_read;
        Event ev_write;
        Event ev_connect;
        /** does not need to wait if true */
        bool ready_send;
    
        void recv_data(evutil_socket_t, short);
        void send_data(evutil_socket_t, short);
        void conn_server(evutil_socket_t, short);
        void try_conn(evutil_socket_t, short);

        public:
        friend ConnPool;
        Conn(): self_ref(this) {}
    
        virtual ~Conn() {
            SALTICIDAE_LOG_INFO("destroyed connection %s", std::string(*this).c_str());
        }

        conn_t self() { return self_ref; }
        operator std::string() const;
        int get_fd() const { return fd; }
        const NetAddr &get_addr() const { return addr; }
        ConnMode get_mode() const { return mode; }
        RingBuffer &read() { return recv_buffer; }
        void set_seg_buff_size(size_t size) { seg_buff_size = size; }

        void write(bytearray_t &&data) {
            send_buffer.push(std::move(data));
            if (ready_send)
                send_data(fd, EV_WRITE);
        }

        void move_send_buffer(conn_t other) {
            send_buffer = std::move(other->send_buffer);
        }

        void terminate();

        protected:
        /** close the connection and free all on-going or planned events. */
        virtual void close() {
            ev_read.clear();
            ev_write.clear();
            ev_connect.clear();
            ::close(fd);
            fd = -1;
        }

        virtual void on_read() = 0;
        virtual void on_setup() = 0;
        virtual void on_teardown() = 0;
    };
    
    private:
    int max_listen_backlog;
    double try_conn_delay;
    double conn_server_timeout;
    size_t seg_buff_size;
    std::unordered_map<int, conn_t> pool;
    int listen_fd;
    Event ev_listen;

    void accept_client(evutil_socket_t, short);
    conn_t add_conn(conn_t conn);

    protected:
    EventContext eb;
    virtual conn_t create_conn() = 0;
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(try_conn_delay);
    }

    public:
    friend Conn;
    ConnPool(const EventContext &eb,
            int max_listen_backlog = 10,
            double try_conn_delay = 2,
            double conn_server_timeout = 2,
            size_t seg_buff_size = 4096):
        max_listen_backlog(max_listen_backlog),
        try_conn_delay(try_conn_delay),
        conn_server_timeout(conn_server_timeout),
        seg_buff_size(seg_buff_size),
        eb(eb) {}

    ~ConnPool() {
        for (auto it: pool)
        {
            conn_t conn = it.second;
            conn->close();
        }
    }

    ConnPool(const ConnPool &) = delete;
    ConnPool(ConnPool &&) = delete;

    /** create an active mode connection to addr */
    conn_t create_conn(const NetAddr &addr);
    /** setup and start listening */
    void listen(NetAddr listen_addr);
};

}

#endif
