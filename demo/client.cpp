#include "../src/sunny_client_manager.h"
#include <cstdlib>
#include <string>
#include <iostream>
#include <boost/asio.hpp>
#include <locale.h>

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
#endif

int main()
{
    sunny::client_manager *p = new sunny::client_manager();
    sunny::client_manager &client = *p;
    try
    {
        boost::asio::io_context ioc;
        tcp::socket socket(ioc);
        tcp::resolver resolver(ioc);
        boost::asio::connect(socket, resolver.resolve("127.0.0.1", "9527"));
        std::cout << "connected!" << std::endl;

        unsigned char* buffer_read = new unsigned char[max_length];
        unsigned char* buffer_write = new unsigned char[max_length];

        using callback_func = std::function<void(const boost::system::error_code&, std::size_t)>;
        callback_func read_callback = [buffer_read, &socket, &client, &read_callback](
            const boost::system::error_code& ec, std::size_t bytes_transferred) {
            if (ec) {
                sunny::socket_error(ec, "read_callback");
                return;
            }

            std::string data((char*)buffer_read, sizeof(sunny::protocol_header));
#ifdef ENCRYPT_PROTOCOL
            common::rc4::codec(rc4_key, data);
#endif
            assert(bytes_transferred == sizeof(sunny::protocol_header));
            sunny::protocol_header* header = (sunny::protocol_header*)data.data();
            if (header->len > sunny::data_max_length) {
                printf("BUG!!!!");
            }
            assert(header->len <= sunny::data_max_length);

            if (header->len == 0) {
                client.recv((unsigned char*)data.data(), sizeof(sunny::protocol_header));
                boost::asio::async_read(socket, boost::asio::buffer(buffer_read, sizeof(sunny::protocol_header)), read_callback);
            }
            else {
                boost::asio::async_read(socket, boost::asio::buffer(buffer_read + sizeof(sunny::protocol_header), header->len),
                    [buffer_read, &client, &socket, &read_callback](const boost::system::error_code& ec, std::size_t bytes_transferred)
                {
                    std::string data((char*)buffer_read, bytes_transferred + sizeof(sunny::protocol_header));
#ifdef ENCRYPT_PROTOCOL
                    common::rc4::codec(rc4_key, data);
#endif

                    client.recv((unsigned char*)data.data(), sizeof(sunny::protocol_header) + bytes_transferred);
                    boost::asio::async_read(socket, boost::asio::buffer(buffer_read, sizeof(sunny::protocol_header)), read_callback);
                });
            }
        };
        boost::asio::async_read(socket, boost::asio::buffer(buffer_read, sizeof(sunny::protocol_header)), read_callback);

        // set send buffer callback
        if (!client.start(10800, [&socket, &client, buffer_write](const unsigned char* buf, std::size_t len) {
            std::string data((char*)buf, len);
#ifdef ENCRYPT_PROTOCOL
            common::rc4::codec(rc4_key, data);
#endif
            memcpy(buffer_write, data.data(), len);
            boost::asio::async_write(socket, boost::asio::buffer(buffer_write, len),
                [&client](const boost::system::error_code& ec, std::size_t bytes_transferred)
            {
                client.send_complete(ec.value());
                if (ec) {
                    printf("async_write failed.\n");
                }
            });
        })) {
            printf("fatal error:port is used.");
            return -1;
        }

        ioc.run();

        delete[] buffer_read;
        delete[] buffer_write;
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
