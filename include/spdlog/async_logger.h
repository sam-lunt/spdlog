//
// Copyright(c) 2015 Gabi Melman.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)
//

#pragma once

// Very fast asynchronous logger (millions of logs per second on an average desktop)
// Uses pre allocated lockfree queue for maximum throughput even under large number of threads.
// Creates a single back thread to pop messages from the queue and log them.
//
// Upon each log write the logger:
//    1. Checks if its log level is enough to log the message
//    2. Push a new copy of the message to a queue (or block the caller until space is available in the queue)
//    3. will throw spdlog_ex upon log exceptions
// Upon destruction, logs all remaining messages in the queue before destructing..

#include "common.h"
#include "logger.h"

#include "details/async_log_msg.h"
#include "details/mpmc_bounded_q.h"

#include <cstddef>
#include <memory>
#include <string>
#include <chrono>
#include <functional>

namespace spdlog {
namespace details {

class async_worker;

} // namespace details

class async_logger SPDLOG_FINAL : public logger
{
    using base_type = logger;

private:
    using queue_type = details::mpmc_bounded_queue<details::async_log_msg>;
    using worker_ptr = std::shared_ptr<details::async_worker>;

    queue_type                  _queue;
    async_overflow_policy const _overflow_policy;
    worker_ptr                  _async_worker;

public:
    ~async_logger() SPDLOG_NOEXCEPT;

    template<class It>
    async_logger(const std::string &name, const It &begin, const It &end, std::size_t queue_size, std::shared_ptr<details::async_worker>);
    async_logger(const std::string &name, sinks_init_list sinks,          std::size_t queue_size, std::shared_ptr<details::async_worker>);
    async_logger(const std::string &name, sink_ptr single_sink,           std::size_t queue_size, std::shared_ptr<details::async_worker>);

    template<class It>
    async_logger(const std::string &name, const It &begin, const It &end, std::size_t queue_size, async_overflow_policy, std::shared_ptr<details::async_worker>);
    async_logger(const std::string &name, sinks_init_list sinks,          std::size_t queue_size, async_overflow_policy, std::shared_ptr<details::async_worker>);
    async_logger(const std::string &name, sink_ptr single_sink,           std::size_t queue_size, async_overflow_policy, std::shared_ptr<details::async_worker>);

    template<class It>
    SPDLOG_DEPRECATED
    async_logger(const std::string &logger_name, const It &begin, const It &end, size_t queue_size,
        const async_overflow_policy = async_overflow_policy::block_retry,
        const std::function<void()> &worker_warmup_cb = nullptr,
        const std::chrono::milliseconds &flush_interval_ms = std::chrono::milliseconds::zero(),
        const std::function<void()> &worker_teardown_cb = nullptr);

    SPDLOG_DEPRECATED
    async_logger(const std::string &logger_name, sinks_init_list sinks, size_t queue_size,
        const async_overflow_policy = async_overflow_policy::block_retry,
        const std::function<void()> &worker_warmup_cb = nullptr,
        const std::chrono::milliseconds &flush_interval_ms = std::chrono::milliseconds::zero(),
        const std::function<void()> &worker_teardown_cb = nullptr);

    SPDLOG_DEPRECATED
    async_logger(const std::string &logger_name, sink_ptr single_sink, size_t queue_size,
        const async_overflow_policy = async_overflow_policy::block_retry,
        const std::function<void()> &worker_warmup_cb = nullptr,
        const std::chrono::milliseconds &flush_interval_ms = std::chrono::milliseconds::zero(),
        const std::function<void()> &worker_teardown_cb = nullptr);

    // Block until all messages in the queue at the start of the function are removed
    void flush() override;

    // Error handler
    void set_error_handler(log_err_handler) override;
    log_err_handler error_handler() override;

protected:
    void _sink_it(details::log_msg &msg) override;
    void _set_formatter(spdlog::formatter_ptr msg_formatter) override;
    void _set_pattern(const std::string &pattern, pattern_time_type pattern_time) override;
};

} // namespace spdlog

#include "details/async_logger_impl.h"
