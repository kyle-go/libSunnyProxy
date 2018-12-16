#pragma once
#include <iostream>
#include <functional>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;

namespace sunny {

    // align by 1 byte
#pragma pack(push)
#pragma pack(1)
    /* the member variables transfer with big endian */
    struct protocol_header {
        enum {
            tcp_connect = 0,
            tcp_disconnect,
            tcp_data
        };

        unsigned char type;
        unsigned char zipped; /* not support yet */
        uint32_t sid;   /* socket id */
        uint32_t len;   /* Subsequent data length, cannot over than data_max_length */
    };
#pragma pack(pop)

    enum {
        data_max_length = 1024 * 8,
        queue_max_num = 64
    };

    template<class T, class C>
    static bool starts_with(const T& src, C w) {
        return src.compare(0, T(w).length(), T(w).c_str()) == 0;
    }

    template<class T>
    static T sub_string(const T& src, const T& s, const T& e) {
        auto pos1 = src.find(s);
        if (pos1 == T::npos) {
            return T();
        }
        auto pos2 = src.find(e, pos1 + s.length());
        if (pos2 == T::npos) {
            return T();
        }
        return src.substr(pos1 + s.length(), pos2 - pos1 - s.length());
    }
    template<class T, class C, class D>
    static T sub_string(const T& src, C s, D e) {
        return sub_string(src, T(s), T(e));
    }

    inline void socket_error(const boost::system::error_code& ec, char const* what) {
        std::cerr << what << ": " << ec.message() << "\n";
    }

    class socket_base :
        public boost::noncopyable,
        public std::enable_shared_from_this<socket_base>
    {
    public:
        using status_callback = std::function<void(int)>;

        /*use read/wirte, after call do_connect() success. */
        explicit socket_base(boost::asio::io_context& ioc)
            : socket_(ioc)
        { }

        /* donot call do_connect(), use read/wirte directly. */
        explicit socket_base(tcp::socket socket)
            : socket_(std::move(socket))
        {
#ifdef _DEBUG
            is_connected_ = true;
#endif
        }

        virtual ~socket_base() {}

        void do_connect(const tcp::resolver::results_type& endpoints, status_callback cb_connect) {
#ifdef _DEBUG
            assert(!is_connected_);
            assert(!socket_.is_open());
#endif
            auto self(shared_from_this());
            boost::asio::async_connect(socket_, endpoints,
                [this, self, cb_connect](const boost::system::error_code& ec, const tcp::endpoint& ep)
            {
                if (ec) {
                    socket_error(ec, "socket_base::on_connect()");
                    cb_connect(ec.value());
                    return;
                }
#ifdef _DEBUG
                is_connected_ = true;
#endif
                cb_connect(0);
            });
        }

        std::vector<unsigned char>& get_read_buffer() {
#ifdef _DEBUG
            assert(is_connected_);
            assert(read_ready_);
#endif
            return already_read_;
        }

        void do_read_some(status_callback cb_read_some) {
#ifdef _DEBUG
            assert(is_connected_);
            assert(read_ready_);
            read_ready_ = false;
#endif
            auto self(shared_from_this());
            socket_.async_read_some(boost::asio::buffer(read_buffer_, data_max_length),
                [this, self, cb_read_some](boost::system::error_code ec, std::size_t length)
            {
#ifdef _DEBUG
                read_ready_ = true;
#endif
                if (ec) {
                    socket_error(ec, "socket_base::read_some()");
                    cb_read_some(ec.value());
                    return;
                }
                assert(length > 0);
                std::vector<unsigned char> tmp(read_buffer_, read_buffer_ + length);
                std::swap(tmp, already_read_);
                cb_read_some(0);
            });
        }

        bool do_read(std::size_t len, status_callback cb_read) {
            if(len > data_max_length) {
                return false;
            }
#ifdef _DEBUG
            assert(is_connected_);
            assert(read_ready_);
            read_ready_ = false;
            assert(cb_read);
            should_read_len_ = len;
#endif
            auto self(shared_from_this());
            boost::asio::async_read(socket_, boost::asio::buffer(read_buffer_, len),
            [this, self, cb_read](const boost::system::error_code& ec, std::size_t length)
            {
#ifdef _DEBUG
                read_ready_ = true;
#endif
                if (ec) {
                    socket_error(ec, "socket_base::read()");
                    cb_read(ec.value());
                    return;
                }
#ifdef _DEBUG
                assert(length == should_read_len_);
#endif
                std::vector<unsigned char> tmp(read_buffer_, read_buffer_ + length);
                std::swap(tmp, already_read_);
                cb_read(0);
            });
            return true;
        }

        void do_read_until(const std::string& until, status_callback cb_read) {
#ifdef _DEBUG
            assert(is_connected_);
            assert(read_ready_);
            assert(cb_read);
            read_ready_ = false;
#endif
            auto self(shared_from_this());
            std::shared_ptr<boost::asio::streambuf> buffer = std::make_shared<boost::asio::streambuf>();
            boost::asio::async_read_until(socket_, *buffer, until,
                [this, self, cb_read, buffer](const boost::system::error_code& ec, std::size_t length)
            {
#ifdef _DEBUG
                read_ready_ = true;
#endif
                if (ec) {
                    socket_error(ec, "socket_base::do_read_until()");
                    cb_read(ec.value());
                    return;
                }
                std::ostringstream ss;
                ss << buffer;
                std::string data = ss.str();

                assert(length <= data.length());
                std::vector<unsigned char> tmp(data.begin(), data.end());
                std::swap(tmp, already_read_);
                cb_read(0);
            });
        }

        bool do_write(const std::vector<unsigned char>& buf, status_callback cb_write) {
            if(buf.size() > data_max_length) {
                return false;
            }
#ifdef _DEBUG
            assert(is_connected_);
            assert(write_ready_);
            write_ready_ = false;
            assert(cb_write);
            should_write_len_ = buf.size();
#endif
            memcpy(write_buffer_, buf.data(), buf.size());
            auto self(shared_from_this());
            boost::asio::async_write(socket_, boost::asio::buffer(write_buffer_, buf.size()),
             [this, self, cb_write](const boost::system::error_code& ec, std::size_t length)
             {
#ifdef _DEBUG
                 write_ready_ = true;
#endif
                 if (ec) {
                     socket_error(ec, "socket_base::write()");
                     cb_write(ec.value());
                     return;
                 }
#ifdef _DEBUG
                 assert(length == should_write_len_);
#endif
                 cb_write(0);
             });
            return true;
        }

    private:
#ifdef _DEBUG
        bool read_ready_ = true;
        bool write_ready_ = true;
        bool is_connected_ = false;
        std::size_t should_write_len_ = 0;
        std::size_t should_read_len_ = 0;
#endif
        tcp::socket socket_;
        std::vector<unsigned char> already_read_;
        unsigned char read_buffer_[data_max_length] = { 0 };
        unsigned char write_buffer_[data_max_length] = { 0 };
    };
}
