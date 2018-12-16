#pragma once
#include "sunny_common.h"
#include <functional>
#include <memory>

namespace sunny {
    using send_callback = std::function<void(const unsigned char*, std::size_t)>;

    class client_manager : public boost::noncopyable {
        class impl;
    public:
        client_manager() {};
        ~client_manager();

        // send is an asynchronous callback function
        bool start(unsigned short local_port, send_callback send);

        void send_complete(int status);

        bool recv(const unsigned char* buf, int len);

    private:
        std::shared_ptr<impl> impl_;
    };
}
