#include "../src/sunny_server_manager.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <boost/asio.hpp>

using boost::asio::ip::tcp;
enum { max_length = sunny::data_max_length + sizeof(sunny::protocol_header) };

#ifdef ENCRYPT_PROTOCOL
namespace common {
    struct rc4 {
        static void codec(const std::string& key, unsigned char *data, int len) {
            RC4_STATE s;
            rc4_setup(&s, (unsigned char*)key.c_str(), key.length());

            int x, y, *m, a, b;
            x = s.x;
            y = s.y;
            m = s.m;

            for (int i = 0; i < len; i++)
            {
                x = (unsigned char)(x + 1); a = m[x];
                y = (unsigned char)(y + a);
                m[x] = b = m[y];
                m[y] = a;
                data[i] ^= m[(unsigned char)(a + b)];
            }
            s.x = x;
            s.y = y;
        }

        static std::string& codec(const std::string& key, std::string& data) {
            codec(key, (unsigned char*)data.c_str(), data.length());
            return data;
        }

    private:
        typedef struct _rc4_state
        {
            int x, y, m[256];
        }RC4_STATE;

        static void rc4_setup(RC4_STATE *s, unsigned char *key, int length)
        {
            int j, k, *m, a;
            s->x = 0;
            s->y = 0;
            m = s->m;

            for (int i = 0; i < 256; i++)
            {
                m[i] = i;
            }

            j = k = 0;
            for (int i = 0; i < 256; i++)
            {
                a = m[i];
                j = (unsigned char)(j + a + key[k]);
                m[i] = m[j]; m[j] = a;
                if (++k >= length) k = 0;
            }
        }
    };
}
auto rc4_key = "Hello,Hello,Hey-20181211";
#endif // ENCRYPT_PROTOCOL

class session
    : public std::enable_shared_from_this<session>
{
public:
    explicit session(tcp::socket socket)
        : socket_(std::move(socket))
    { }

    ~session() {
        printf("~session() called.\n");
    }

    void start()
    {
        auto self(shared_from_this());
        manager_ = new sunny::server_manager();
        auto send_ = [this, self](const unsigned char* buf, std::size_t len) {
            std::string data((char*)buf, len);
#ifdef ENCRYPT_PROTOCOL
            common::rc4::codec(rc4_key, data);
#endif
            memcpy(write_data_, data.data(), len);
                boost::asio::async_write(socket_, boost::asio::buffer(write_data_, len),
                [this, self](const boost::system::error_code& ec, std::size_t length)
            {
                if (manager_)
                    manager_->send_complete(ec.value());
            });
        };
        manager_->start(send_);

        do_read();
    }

private:
    void some_error(int) {
        printf("some_error() called.\n");

        delete manager_;
        manager_ = nullptr;
    };

    void do_read()
    {
        auto self(shared_from_this());
        boost::asio::async_read(socket_,
            boost::asio::buffer(read_data_, sizeof(sunny::protocol_header)),
            [this, self](boost::system::error_code ec, std::size_t length)
        {
            if (ec) {
                some_error(0);
                return;
            }
            assert(length == sizeof(sunny::protocol_header));
            std::string data((char*)read_data_, sizeof(sunny::protocol_header));
#ifdef ENCRYPT_PROTOCOL
            common::rc4::codec(rc4_key, data);
#endif
            sunny::protocol_header* header = (sunny::protocol_header*)data.data();
            assert(header->len <= sunny::data_max_length);
            if (header->len == 0) {
                manager_->recv((unsigned char*)data.data(), sizeof(sunny::protocol_header));
                do_read();
            }
            else {
                boost::asio::async_read(socket_, 
                    boost::asio::buffer(read_data_ + sizeof(sunny::protocol_header), header->len),
                    [this, self](const boost::system::error_code& ec, std::size_t bytes_transferred)
                {
                    std::string data((char*)read_data_, sizeof(sunny::protocol_header) + bytes_transferred);
#ifdef ENCRYPT_PROTOCOL
                    common::rc4::codec(rc4_key, data);
#endif
                    manager_->recv((unsigned char*)data.data(), sizeof(sunny::protocol_header) + bytes_transferred);
                    do_read();
                });
            }
        });
    }

    tcp::socket socket_;
    unsigned char read_data_[max_length] = { 0 };
    unsigned char write_data_[max_length] = { 0 };
    sunny::server_manager *manager_ = nullptr;
};

class server
{
public:
    server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        do_accept();
    }

private:
    void do_accept()
    {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket)
        {
            if (!ec)
            {
                std::make_shared<session>(std::move(socket))->start();
            }

            do_accept();
        });
    }

    tcp::acceptor acceptor_;
};

int main(int argc, char* argv[])
{
    try
    {
        boost::asio::io_context io_context;
        server s(io_context, static_cast<unsigned short>(9527));

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code const&, int) {
            // Stop the `io_context`. This will cause `run()`
            // to return immediately, eventually destroying the
            // `io_context` and all of the sockets in it.
            printf("Caught Ctrl+C, exiting now...\n");
            io_context.stop();
        });
        io_context.run();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}
