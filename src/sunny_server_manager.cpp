#include "sunny_server_manager.h"
#include <thread>
#include <map>
#include <queue>
#include <tuple>
#include <vector>
#include <string>
#include <mutex>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;
using namespace sunny;


class server_manager::impl : public boost::noncopyable {
    friend class server_manager;
    class client : public socket_base {
    public:
        client(
            boost::asio::io_context& ioc,
            std::weak_ptr<impl> weak_impl,
            uint32_t client_id)
            : ioc_(ioc), weak_impl_(weak_impl), client_id_(client_id), socket_base(ioc)
        {
            printf("server_manager::client().\n");
        }

        ~client() {
            printf("~server_manager::client().\n");
        }

        void on_connect(const std::string& http_data) {
            auto self(shared_from_this());
            auto connect_finish = [this, self, http_data](int status, bool is_connect = false) {
                if (status != 0) {
                    send_disconnect();
                    return;
                }

                // connect 隧道
                // https://tools.ietf.org/html/draft-luotonen-web-proxy-tunneling-01
                if (is_connect) {
                    std::string str = "HTTP/1.0 200 Connection established\r\nProxy-agent: Nginx-Proxy/1.0\r\n\r\n";
                    std::vector<unsigned char> v(str.begin(), str.end());
                    auto tuple_data = std::make_tuple(protocol_header::tcp_connect, v);
                    {
                        std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                        recv_buffers_.push(tuple_data);
                        send_to_main_socket(tuple_data);
                    }
                    read_some();
                    return;
                }

                std::vector<unsigned char>v(http_data.begin(), http_data.end());
                do_write(v, [this, self](int status) {
                    if (status != 0) {
                        send_disconnect();
                        return;
                    }
                    auto tuple_data = std::make_tuple(protocol_header::tcp_connect, std::vector<unsigned char>());
                    {
                        std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                        recv_buffers_.push(tuple_data);
                        send_to_main_socket(tuple_data);
                    }
                    read_some();
                });
            };

            auto get_port = [](const std::string& data)->std::string {
                auto first_line = sub_string(data, " ", " ");
                if (first_line.empty()) {
                    return "80";
                }
                auto pos = first_line.find("http://");
                if (pos != std::string::npos) {
                    pos += sizeof("http://") - 1;
                    pos = first_line.find(":", pos);
                    if (pos != std::string::npos) {
                        return first_line.substr(pos + 1);
                    }
                    return "80";
                }

                pos = first_line.find("https://");
                if (first_line.find("https://") != std::string::npos) {
                    pos += sizeof("https://") - 1;
                    pos = first_line.find(":", pos);
                    if (pos != std::string::npos) {
                        return first_line.substr(pos + 1);
                    }
                    return "443";
                }

                pos = first_line.find(":");
                if (pos != std::string::npos) {
                    return first_line.substr(pos + 1);
                }
                return "80";
            };

            std::string url = sub_string(http_data, "Host: ", "\r\n");
            std::string port = get_port(http_data);
            auto pos = url.find(":");
            if (pos != std::string::npos) {
                url = url.substr(0, pos);
            }

            tcp::resolver::query query(url, port.c_str());
            // create shared_ptr<tcp::resolver> to stop resolver_'s destruction before the callback
            std::shared_ptr<tcp::resolver> resolver_ = std::make_shared<tcp::resolver>(ioc_);
            resolver_->async_resolve(query,
                [this, self, resolver_, connect_finish, http_data](const boost::system::error_code& ec, tcp::resolver::results_type results)
            {
                if (ec) {
                    send_disconnect();
                    return;
                }

                printf("DATA:%s\n", http_data.c_str());
                bool is_connect = starts_with(http_data, "CONNECT ");
                do_connect(results, [this, self, connect_finish, is_connect](int status) {
                    connect_finish(status, is_connect);
                });
            });
        }

        void notify_main_socket_complete() {
#ifdef _DEBUG
            assert(!send_ready_);
            send_ready_ = true;
#endif
            std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
            recv_buffers_.pop();
            auto recv_buffers_size = recv_buffers_.size();
            if (recv_buffers_size > 0) {
                send_to_main_socket(recv_buffers_.front());
            }

            if (recv_buffers_size <= queue_max_num / 3) {
                enable_read_some();
            }
        }

        void add_send_buffer(const std::vector<unsigned char>& buf) {
            std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
            send_buffers_.push(buf);
            if (send_buffers_.size() == 1) {
                send_buffer(buf);
            }
        }

    private:
        void enable_read_some() {
            if (!is_reading_) {
                is_reading_ = true;
                read_some();
            }
        }

        void read_some() {
            assert(is_reading_);
            if (!is_reading_)
                return;
            auto self(shared_from_this());
            socket_base::do_read_some([this, self](int status) {
                if (status != 0) {
                    send_disconnect();
                    return;
                }
                auto buf = get_read_buffer();
                auto recv_buffers_size = 0;
                {
                    auto tuple_data = std::make_tuple(protocol_header::tcp_data, buf);
                    std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                    recv_buffers_.push(tuple_data);
                    recv_buffers_size = recv_buffers_.size();
                    if (recv_buffers_size == 1) {
                        send_to_main_socket(tuple_data);
                    }
                }

                if (recv_buffers_size < queue_max_num)
                    read_some();
                else
                    is_reading_ = false;
            });
        }

        void send_buffer(const std::vector<unsigned char>& buf) {
            auto self(shared_from_this());
            do_write(buf, [this, self](int status) {
                if (status != 0) {
                    send_disconnect();
                    return;
                }
                std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
                send_buffers_.pop();
                if (!send_buffers_.empty()) {
                    send_buffer(send_buffers_.front());
                }
            });
        }

        void send_to_main_socket(std::tuple<unsigned char, std::vector<unsigned char>> tuple_data) {
            unsigned char type = std::get<0>(tuple_data);
            std::vector<unsigned char>& buf = std::get<1>(tuple_data);
#ifdef _DEBUG
            assert(send_ready_);
            send_ready_ = false;
#endif
            assert(type <= protocol_header::tcp_data);
            std::shared_ptr<impl> shared_impl(weak_impl_.lock());
            if (!shared_impl)
                return;
            protocol_header header = { 0 };
            header.type = type;
            header.sid = client_id_;
            if (!buf.empty()) {
                header.len = buf.size();
            }
            std::vector<unsigned char> v((unsigned char*)&header,
                (unsigned char*)&header + sizeof(protocol_header));
            v.insert(v.end(), buf.begin(), buf.end());
            shared_impl->add_send_buffer(v);
        }

        inline void send_disconnect() {
            auto tuple_data = std::make_tuple(protocol_header::tcp_disconnect, std::vector<unsigned char>());
            std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
            recv_buffers_.push(tuple_data);
            if (recv_buffers_.size() == 1) {
                send_to_main_socket(tuple_data);
            }

            // use post to aviod deadlock.
            auto self(shared_from_this());
            boost::asio::post([this, self] {
                remove_self();
            });
        }

        void remove_self() {
            std::shared_ptr<impl> shared_impl(weak_impl_.lock());
            if (!shared_impl)
                return;
            auto self(shared_from_this());
            shared_impl->remove_client(client_id_);
        }

#ifdef _DEBUG
        bool send_ready_ = true;
#endif
        bool is_reading_ = true;
        std::mutex recv_buffers_mutex_;
        std::queue<std::tuple<unsigned char, std::vector<unsigned char>>> recv_buffers_;

        std::mutex send_buffers_mutex_;
        std::queue<std::vector<unsigned char>> send_buffers_;

        uint32_t client_id_;
        std::weak_ptr<impl> weak_impl_;
        boost::asio::io_context& ioc_;
    }; // end class client

public:
    impl() {
        printf("server::impl().\n");
    }

    ~impl() {
        printf("~server::impl().\n");
    }

    void create_io_context_thread() {
        if (vec_threads_.size() == 0) {
            auto& ioc = ioc_;
            vec_threads_.reserve(thread_count_);
            for (int i = 0; i < thread_count_; i++) {
                vec_threads_.emplace_back([&ioc]() {
                    printf("server::impl::io_context thread start...\n");
                    ioc.run();
                    printf("server::impl::io_context thread exit...\n");
                });
            }
        }
    }

    boost::asio::io_context& get_ioc() {
        return ioc_;
    }

    void set_callback(send_callback cb) {
        assert(cb);
        cb_send_buffer_ = cb;
    }

    void add_send_buffer(const std::vector<unsigned char>& buf) {
        std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
        send_buffers_.push(buf);
        if (send_buffers_.size() == 1) {
            protocol_header *header = (protocol_header*)buf.data();
            current_sending_user_ = header->sid;
            cb_send_buffer_(buf.data(), buf.size());
        }
    }

    void send_buffer_finished() {
        {
            std::lock_guard<std::mutex> lock_(clients_map_mutex_);
            if (clients_map_.find(current_sending_user_) != std::end(clients_map_))
                clients_map_[current_sending_user_]->notify_main_socket_complete();
        }

        std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
        send_buffers_.pop();
        if (!send_buffers_.empty()) {
            auto buf = send_buffers_.front();
            protocol_header *header = (protocol_header*)buf.data();
            current_sending_user_ = header->sid;
            cb_send_buffer_(buf.data(), buf.size());
        }
    }

    void remove_client(uint32_t client_id) {
        std::lock_guard<std::mutex> lock_(clients_map_mutex_);
        if (clients_map_.find(client_id) != std::end(clients_map_))
            clients_map_.erase(client_id);
    }

private:
    const int thread_count_ = std::thread::hardware_concurrency();
    std::vector<std::thread> vec_threads_;
    boost::asio::io_context ioc_{ thread_count_ };

    std::mutex clients_map_mutex_;
    std::map<uint32_t, std::shared_ptr<client>> clients_map_;

    std::mutex send_buffers_mutex_;
    std::queue<std::vector<unsigned char>> send_buffers_;

    send_callback cb_send_buffer_;
    uint32_t current_sending_user_;
};


server_manager::~server_manager() {
    if (impl_) {
        impl_->ioc_.stop();
        for (auto& t : impl_->vec_threads_) {
            t.join();
        }
#ifdef _DEBUG
        assert(impl_->ioc_.stopped());
#endif
        int time_count = 0;
        while (!impl_->ioc_.stopped() && time_count < 50) {
            time_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}


void server_manager::start(send_callback send) {
    assert(send);
    impl_ = std::make_shared<impl>();
    impl_->set_callback(send);
}


void server_manager::send_complete(int status) {
    impl_->send_buffer_finished();
}


bool server_manager::recv(const unsigned char* buf, int len) {
    protocol_header* header = (protocol_header*)buf;
    uint32_t client_id = header->sid;
    const unsigned char* data = buf + sizeof(protocol_header);

    assert(header->len + sizeof(protocol_header) == len);
    if (header->len + sizeof(protocol_header) != len) {
        return false;
    }

    assert(header->len <= data_max_length);
    if (header->len > data_max_length) {
        return false;
    }

    switch (header->type)
    {
    case protocol_header::tcp_connect:
    {
        printf("recv tcp_connect.\n");
        /*
        +---------+---------+
        | req len | REQUEST |
        +---------+---------+
        |    2    |    len  |
        +---------+---------+
        */
        unsigned short iplen = *(unsigned short*)data;
        std::string req((char*)data + 2, iplen);

        std::shared_ptr<impl::client> client;
        {
            std::lock_guard<std::mutex> lock_(impl_->clients_map_mutex_);
            assert(impl_->clients_map_.find(client_id) == std::end(impl_->clients_map_));
            assert(header->len == 2 + iplen);
            client = std::make_shared<impl::client>(impl_->get_ioc(), std::weak_ptr<impl>(impl_), client_id);
            impl_->clients_map_[client_id] = client;
        }
        client->on_connect(req);
        impl_->create_io_context_thread();
        return true;
    }
    break;
    case protocol_header::tcp_disconnect:
    {
        printf("recv tcp_disconnect.\n");
        impl_->remove_client(client_id);
        return true;
    }
    break;
    case protocol_header::tcp_data:
    {
        printf(">");
        std::vector<unsigned char> v(data, data + header->len);
        std::lock_guard<std::mutex> lock_(impl_->clients_map_mutex_);
        if (impl_->clients_map_.find(client_id) != std::end(impl_->clients_map_))
            impl_->clients_map_[client_id]->add_send_buffer(v);
        return true;
    }
    break;
    default:
        // unknown type, return false.
        assert(false);
        break;
    }
    assert(false);
    return false;
}
