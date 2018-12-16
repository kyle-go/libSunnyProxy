#include "sunny_client_manager.h"
#include <thread>
#include <map>
#include <queue>
#include <tuple>
#include <vector>
#include <string>
#include <mutex>
#include <boost/asio.hpp>

using namespace sunny;
using boost::asio::ip::tcp;

namespace sunny {
    auto add_reg_pac = [](const std::wstring& url) {
#ifdef WIN32
        HKEY hSubKey;
        LSTATUS result = RegOpenKeyEx(HKEY_CURRENT_USER,
            (LPTSTR)L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_WRITE, &hSubKey);
        if (result != ERROR_SUCCESS) {
            return false;
        }
        result = RegSetValueEx(hSubKey,
            L"AutoConfigURL",
            0,
            REG_SZ,
            (const BYTE*)url.c_str(),
            2 * url.length());
        RegCloseKey(hSubKey);
        return (result == ERROR_SUCCESS);
#else
        return false;
#endif
    };

    auto delete_reg_pac = []() {
#ifdef WIN32
        HKEY hSubKey;
        LSTATUS result = RegOpenKeyEx(HKEY_CURRENT_USER,
            (LPTSTR)L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", 0, KEY_SET_VALUE, &hSubKey);
        if (result != ERROR_SUCCESS) {
            return false;
        }

        result = RegDeleteValue(hSubKey, L"AutoConfigURL");
        RegCloseKey(hSubKey);
        return (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND);
#else
        return false;
#endif
    };
}


class client_manager::impl :
    public boost::noncopyable,
    public std::enable_shared_from_this<client_manager::impl>
{
    friend class client_manager;
    class client : public socket_base {
    public:
        client(
            tcp::socket socket,
            std::weak_ptr<impl> weak_impl,
            uint32_t client_id)
            : weak_impl_(weak_impl), client_id_(client_id), socket_base(std::move(socket))
        {
            printf("client_manager::client().\n");
        }

        ~client() {
            printf("~client_manager::client().\n");
        }

        void start()
        {
            auto self(shared_from_this());
            auto connect_url_port = [this, self](const std::string& str) {
                std::vector<unsigned char> data;
                unsigned short len = (unsigned short)str.length();
                data.push_back(*(unsigned char*)&len);
                data.push_back(*((unsigned char*)&len + 1));
                data.insert(data.end(), str.begin(), str.end());
                {
                    auto tuple_data = std::make_tuple(protocol_header::tcp_connect, data);
                    std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                    recv_buffers_.push(tuple_data);
                    send_to_main_socket(tuple_data);
                }
                read_some();
            };

            auto deal_with_data = [this, self, connect_url_port](const std::string& data) {
                std::string pac_content;
                {
                    std::shared_ptr<impl> shared_impl(weak_impl_.lock());
                    if (!shared_impl) {
                        return;
                    }
                    pac_content = shared_impl->get_pac_content();
                }

                // 处理本地pac路径
                auto host = sub_string(data, "Host: ", "\r\n");
                if (!host.empty()) {
                    if (host.find("localhost:") != std::string::npos
                        || host.find("127.0.0.1:") != std::string::npos) {
                        if (starts_with(data, "GET http://localhost:1080/proxy.pac ")
                            || starts_with(data, "GET http://127.0.0.1:1080/proxy.pac ")
                            || starts_with(data, "GET /proxy.pac"))
                        {
                            const std::string http_header = "HTTP/1.1 200 OK\r\nServer: nginx\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nContent-Length: " +
                                std::to_string(pac_content.length()) + "\r\nConnection: Close\r\n\r\n";
                            std::vector<unsigned char> v(http_header.begin(), http_header.end());
                            v.insert(v.end(), pac_content.begin(), pac_content.end());
                            do_write(v, [this, self](int) {
                                remove_self();
                            });
                            return;
                        }
                    }
                }
                connect_url_port(data);
                printf("Request:%s\n", data.c_str());

#ifdef _DEBUG
                auto pos = data.find(" https://");
                if (pos != std::string::npos
                    && pos <= 10
                    && !starts_with(data, "CONNECT "))
                {
                    assert(false);
                }
#endif
            };

#ifdef _DEBUG
            do_read(6, [this, self, deal_with_data](int status) {
                if (status != 0) {
                    remove_self();
                    return;
                }
                auto read_buffer = get_read_buffer();
                std::string prefix = std::string((char*)read_buffer.data(), read_buffer.size());
                auto find_it = std::find_if(prefix.begin(), prefix.end(), [](char c) {
                    return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '/');
                });
                assert(find_it == prefix.end());
                do_read_until("\r\n\r\n", [this, self, prefix, deal_with_data](int status) {
                    if (status != 0) {
                        remove_self();
                        return;
                    }
                    auto read_buffer = get_read_buffer();
                    std::string data = prefix + std::string((char*)read_buffer.data(), read_buffer.size());
                    deal_with_data(data);
                });
            });
#else
            do_read_until("\r\n\r\n", [this, self, deal_with_data](int status) {
                if (status != 0) {
                    remove_self();
                    return;
                }
                auto read_buffer = get_read_buffer();
                std::string data = std::string((char*)read_buffer.data(), read_buffer.size());
                deal_with_data(data);
            });
#endif
        }

        void notify_main_socket_complete() {
#ifdef _DEBUG
            assert(!send_ready_);
            send_ready_ = true;
#endif
            std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
            recv_buffers_.pop();
            auto recv_buffers_size = recv_buffers_.size();
            if (recv_buffers_size > 0 && is_connected_) {
                send_to_main_socket(recv_buffers_.front());
            }

            if (recv_buffers_size <= queue_max_num / 3) {
                enable_read_some();
            }
        }

        void add_send_buffer(const std::vector<unsigned char>& buf) {
            std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
            send_buffers_.push(buf);
            if (send_buffers_.size() == 1 && is_connected_) {
                send_buffer(buf);
            }
        }

        void set_connected() {
            is_connected_ = true;
            {
                std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                if (!recv_buffers_.empty()) {
                    send_to_main_socket(recv_buffers_.front());

                }
            }
            {
                std::lock_guard<std::mutex> lock_(send_buffers_mutex_);
                if (!send_buffers_.empty()) {
                    send_buffer(send_buffers_.front());

                }
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
                assert(buf.size() > 0);
                auto recv_buffers_size = 0;
                {
                    auto tuple_data = std::make_tuple(protocol_header::tcp_data, buf);
                    std::lock_guard<std::mutex> lock_(recv_buffers_mutex_);
                    recv_buffers_.push(tuple_data);
                    recv_buffers_size = recv_buffers_.size();
                    if (recv_buffers_size == 1 && is_connected_) {
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
            if (type == protocol_header::tcp_data) {
                assert(buf.size() > 0);
            }
#endif
            assert(type <= protocol_header::tcp_data);
            std::shared_ptr<impl> shared_impl(weak_impl_.lock());
            if (!shared_impl)
                return;
            protocol_header header = { 0 };
            header.type = type;
            header.sid = client_id_;
            if (buf.size() > 0) {
                header.len = buf.size();
            }
            std::vector<unsigned char> v((unsigned char*)&header,
                (unsigned char*)&header + sizeof(protocol_header));
            v.insert(v.end(), buf.begin(), buf.end());
            shared_impl->add_send_buffer(v);
        }

        void send_disconnect() {
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
        bool is_connected_ = false;
        bool is_reading_ = true;
        std::mutex recv_buffers_mutex_;
        std::queue<std::tuple<unsigned char, std::vector<unsigned char>>> recv_buffers_;

        std::mutex send_buffers_mutex_;
        std::queue<std::vector<unsigned char>> send_buffers_;

        uint32_t client_id_;
        std::weak_ptr<impl> weak_impl_;
    }; // end class client

public:
    explicit impl(unsigned short local_port)
        : acceptor_(ioc_, tcp::endpoint(tcp::v4(), local_port), false), local_port_(local_port)
    {
        printf("client::impl().\n");
    }

    ~impl() {
        printf("~client::impl().\n");
    }

    void start() {
        do_accept();
        auto& ioc = ioc_;
        vec_threads_.reserve(thread_count_);
        for (int i = 0; i < thread_count_; i++) {
            vec_threads_.emplace_back([&ioc]() {
                printf("client::impl::io_context thread start...\n");
                ioc.run();
                printf("client::impl::io_context thread exit...\n");
            });
        }
    }

    void set_callback(send_callback cb) {
        assert(cb);
        cb_send_buffer_ = cb;
    }

    void add_send_buffer(const std::vector<unsigned char>& buf) {
#ifdef _DEBUG
        if (buf[0] == protocol_header::tcp_data) {
            protocol_header *header = (protocol_header*)buf.data();
            assert(header->len > 0);
        }
#endif
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

    std::string get_pac_content() {
        return "function FindProxyForURL(url, host) {return \"PROXY 127.0.0.1:" + std::to_string(local_port_) + "; DIRECT\";}";
    }

    std::wstring get_pac_wurl() {
        return L"http://127.0.0.1:" + std::to_wstring(local_port_) + L"/proxy.pac?t=" + std::to_wstring(time(0));
    }

private:
    void do_accept()
    {
        auto self(shared_from_this());
        acceptor_.async_accept([this, self](const boost::system::error_code& ec, tcp::socket socket)
        {
            if (ec) {
                socket_error(ec, "client::impl::do_accept()");
                return;
            }

            auto client_id = client_id_index_++;
            std::shared_ptr<client> client;
            {
                std::lock_guard<std::mutex> lock_(clients_map_mutex_);
                client = std::make_shared<impl::client>(std::move(socket),
                    std::weak_ptr<client_manager::impl>(self), client_id);
                clients_map_[client_id] = client;
            }
            client->start();

            if (!ioc_.stopped()) {
                do_accept();
            }
            else {
                printf("client::impl::do_accept() exit().\n");
            }
        });
    }

    const int thread_count_ = std::thread::hardware_concurrency();
    std::vector<std::thread> vec_threads_;
    boost::asio::io_context ioc_{ thread_count_ };
    tcp::acceptor acceptor_;

    uint32_t client_id_index_ = 0;
    std::mutex clients_map_mutex_;
    std::map<uint32_t, std::shared_ptr<client>> clients_map_;

    std::mutex send_buffers_mutex_;
    std::queue<std::vector<unsigned char>> send_buffers_;

    send_callback cb_send_buffer_;
    uint32_t current_sending_user_;
    unsigned short local_port_;
};


client_manager::~client_manager() {
    if (impl_) {
        delete_reg_pac();
        impl_->acceptor_.close();
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
};


bool client_manager::start(unsigned short local_port, send_callback send) {
    assert(send);
    try {
        impl_ = std::make_shared<impl>(local_port);
        impl_->start();
    }
    catch (...) {
        return false;
    }
    impl_->set_callback(send);
    add_reg_pac(impl_->get_pac_wurl());
    return true;
}


void client_manager::send_complete(int status) {
    impl_->send_buffer_finished();
}


bool client_manager::recv(const unsigned char* buf, int len) {
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
        std::vector<unsigned char>buf(data, data + header->len);
        std::lock_guard<std::mutex> lock_(impl_->clients_map_mutex_);
        if (impl_->clients_map_.find(client_id) != std::end(impl_->clients_map_)) {
            impl_->clients_map_[client_id]->set_connected();
            if (!buf.empty())
                impl_->clients_map_[client_id]->add_send_buffer(buf);
        }
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
        std::vector<unsigned char>buf(data, data + header->len);
        std::lock_guard<std::mutex> lock_(impl_->clients_map_mutex_);
        if (impl_->clients_map_.find(client_id) != std::end(impl_->clients_map_))
            impl_->clients_map_[client_id]->add_send_buffer(buf);
        return true;
    }
    default:
        // unknown type, return false.
        assert(false);
        break;
    }
    assert(false);
    return false;
}
