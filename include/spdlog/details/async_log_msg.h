#pragma once

#include "spdlog/common.h"

#include <cstddef>
#include <string>
#include <utility>

namespace spdlog {
namespace details {

struct async_log_msg
{
    // async_msg_type        msg_type;
    level::level_enum     level;
    log_clock::time_point time;
    std::size_t           thread_id;
    std::size_t           msg_id;
    std::string           text;

    async_log_msg() = default;
    async_log_msg(async_log_msg&&) = default;
    async_log_msg(async_log_msg const&) = default;
    async_log_msg& operator=(async_log_msg&&) = default;
    async_log_msg& operator=(async_log_msg const&) = default;

    explicit async_log_msg(log_msg const& msg)
        // : msg_type(async_msg_type::log)
        : level{ msg.level }
        , time{ msg.time }
        , thread_id{ msg.thread_id }
        , msg_id{ msg.msg_id }
        , text(msg.raw.data(), msg.raw.size())
    {}
};

} // namespace details {
} // namespace spdlog
