//
// Copyright(c) 2015 Gabi Melman.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)
//

#pragma once

// Async Logger implementation
// Use an async_sink (queue per logger) to perform the logging in a worker thread

#include "../async_logger.h"
#include "async_worker.h"

template<class It>
inline spdlog::async_logger::async_logger(const std::string &logger_name, const It &begin, const It &end, size_t queue_size,
    const async_overflow_policy overflow_policy, const std::function<void()> &worker_warmup_cb,
    const std::chrono::milliseconds &flush_interval_ms, const std::function<void()> &worker_teardown_cb)
    : async_logger(logger_name,
                   begin,
                   end,
                   queue_size,
                   overflow_policy,
                   std::make_shared<details::async_worker>(worker_warmup_cb, worker_teardown_cb)
                )
{
    (void)flush_interval_ms;
}

inline spdlog::async_logger::async_logger(const std::string &logger_name, sinks_init_list sinks_list, size_t queue_size,
    const async_overflow_policy overflow_policy, const std::function<void()> &worker_warmup_cb,
    const std::chrono::milliseconds &flush_interval_ms, const std::function<void()> &worker_teardown_cb)
    : async_logger(logger_name, sinks_list.begin(), sinks_list.end(), queue_size, overflow_policy, worker_warmup_cb, flush_interval_ms,
          worker_teardown_cb)
{}

inline spdlog::async_logger::async_logger(const std::string &logger_name, sink_ptr single_sink, size_t queue_size,
    const async_overflow_policy overflow_policy, const std::function<void()> &worker_warmup_cb,
    const std::chrono::milliseconds &flush_interval_ms, const std::function<void()> &worker_teardown_cb)
    : async_logger(
          logger_name, {std::move(single_sink)}, queue_size, overflow_policy, worker_warmup_cb, flush_interval_ms, worker_teardown_cb)
{}

template<class It>
spdlog::async_logger::async_logger(const std::string &name, const It &begin, const It &end, std::size_t queue_size, std::shared_ptr<details::async_worker> worker)
    : async_logger(name, begin, end, queue_size, async_overflow_policy::block_retry, std::move(worker))
{}

inline
spdlog::async_logger::async_logger(const std::string &name, sinks_init_list sinks, std::size_t queue_size, std::shared_ptr<details::async_worker> worker)
    : async_logger(name, sinks, queue_size, async_overflow_policy::block_retry, std::move(worker))
{}

inline
spdlog::async_logger::async_logger(const std::string &name, sink_ptr single_sink, std::size_t queue_size, std::shared_ptr<details::async_worker> worker)
    : async_logger(name, single_sink, queue_size, async_overflow_policy::block_retry, std::move(worker))
{}

template<class It>
spdlog::async_logger::async_logger(const std::string &name, const It &begin, const It &end, std::size_t queue_size, async_overflow_policy overflow_policy, std::shared_ptr<details::async_worker> worker)
    : base_type(name, begin, end)
    , _queue(queue_size)
    , _overflow_policy{ overflow_policy }
    , _async_worker(std::move(worker))
{
    _async_worker->register_logger(*this, this->_queue);
}

inline
spdlog::async_logger::async_logger(const std::string &name, sinks_init_list sinks, std::size_t queue_size, async_overflow_policy overflow_policy, std::shared_ptr<details::async_worker> worker)
    : async_logger(name, sinks.begin(), sinks.end(), queue_size, overflow_policy, std::move(worker))
{}

inline
spdlog::async_logger::async_logger(const std::string &name, sink_ptr single_sink, std::size_t queue_size, async_overflow_policy overflow_policy, std::shared_ptr<details::async_worker> worker)
    : async_logger(name, &single_sink, &single_sink + 1, queue_size, overflow_policy, std::move(worker))
{}

inline
spdlog::async_logger::~async_logger() SPDLOG_NOEXCEPT
{
    _async_worker->deregister_logger(*this);
}

inline void spdlog::async_logger::flush()
{
    auto write_pos = _queue.enqueue_pos();
    while (_queue.dequeue_pos() < write_pos)
        std::this_thread::yield();

    base_type::flush();
}

// Error handler
inline void spdlog::async_logger::set_error_handler(spdlog::log_err_handler err_handler)
{
    // this is thread-safe, but not atomic:
    // the async worker will see a different error handler for a short window
    _async_worker->update_error_handler(*this, err_handler);
    base_type::set_error_handler(std::move(err_handler));
}

inline spdlog::log_err_handler spdlog::async_logger::error_handler()
{
    // only override getter since setter is overridden
    // (for consistency and to allow a change of behavior later)
    return base_type::error_handler();
}

inline void spdlog::async_logger::_set_formatter(spdlog::formatter_ptr msg_formatter)
{
    // this is thread-safe, but not atomic:
    // the async worker will see a different formatter for a short window
    _async_worker->update_formatter(*this, msg_formatter);
    base_type::_set_formatter(std::move(msg_formatter));
}

inline void spdlog::async_logger::_set_pattern(const std::string &pattern, pattern_time_type pattern_time)
{
    _set_formatter(std::make_shared<pattern_formatter>(pattern, pattern_time));
}

inline void spdlog::async_logger::_sink_it(details::log_msg &msg)
{
#if defined(SPDLOG_ENABLE_MESSAGE_COUNTER)
    _incr_msg_counter(msg);
#endif

    details::async_log_msg async_msg(msg);

    if (!_queue.enqueue(std::move(async_msg)))
    {
        // this is constructed so that -Wswitch will trigger if a new enum value is added
        switch (_overflow_policy)
        {
        case async_overflow_policy::discard_log_msg:
            return;
        case async_overflow_policy::block_retry:
            break;
        }

        while (!_queue.enqueue(std::move(async_msg)))
            std::this_thread::yield();
    }

    _async_worker->notify_new_data();
}
