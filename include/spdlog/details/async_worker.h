#pragma once

#include "async_log_msg.h"
#include "mpmc_bounded_q.h"

#include "spdlog/async_logger.h"

#include <memory>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace spdlog {
namespace details {

class async_worker
{
private:
    using queue_type  = details::mpmc_bounded_queue<details::async_log_msg>;

    struct async_logger_data
    {
        using sink_vector = std::vector<std::shared_ptr<sinks::sink>>;

        queue_type*        queue;
        sink_vector const* sinks;
        std::string const  logger_name;
        formatter_ptr      formatter;
        log_err_handler    err_handler;

        // need an explicitly defined constructor to work with unordered_map::emplace,
        // since aggregate constructors don't work with perfect forwarding
        // (at least as the standard library does it)
        async_logger_data(queue_type& queue_, sink_vector const& sinks_, std::string name_, formatter_ptr formatter_, log_err_handler err_handler_) SPDLOG_NOEXCEPT
            : queue(&queue_)
            , sinks(&sinks_)
            , logger_name(std::move(name_))
            , formatter(std::move(formatter_))
            , err_handler(std::move(err_handler_))
        {}

        async_logger_data() = default;
        async_logger_data(async_logger_data&&) = default;
        async_logger_data(async_logger_data const&) = default;
        async_logger_data& operator=(async_logger_data&&) = default;
        async_logger_data& operator=(async_logger_data const&) = default;
    };

    using data_map_type = std::unordered_map<async_logger const*, async_logger_data>;

    std::atomic<bool>              _running;
    std::atomic<bool>              _lock_requested;

    std::mutex                     _registration_mutex;
    std::mutex                     _data_mutex;

    std::condition_variable        _data_condition_variable; // associated with _data_mutex

    data_map_type                  _data_map; // protected by _data_mutex

    std::atomic<std::size_t>       _waiting_for_data;
    std::size_t                    _active_workers; // protected by _data_mutex

    std::size_t const              _thread_count;
    std::unique_ptr<std::thread[]> _worker_threads;

public:
    // if thread_count is 0, the user is responsible for calling work_loop.
    // the callbacks will be copied for each thread created.
    async_worker(std::size_t thread_count, std::function<void()> worker_warmup_cb, std::function<void()> worker_teardown_cb) SPDLOG_NOEXCEPT
        : _running{ true }
        , _lock_requested{ false }
        , _waiting_for_data{ 0 }
        , _active_workers{ 0 }
        , _thread_count{ thread_count }
        , _worker_threads{ nullptr }
    {
        if (thread_count == 0)
            return;

        _worker_threads.reset(new std::thread[_thread_count]);
        for (std::size_t k = 0; k < _thread_count; ++k)
            _worker_threads[k] = std::thread{
                [this, worker_warmup_cb, worker_teardown_cb]
                {
                    if (worker_warmup_cb)
                        worker_warmup_cb();

                    this->work_loop();

                    if (worker_teardown_cb)
                        worker_teardown_cb();
                }
            };
    }

    ~async_worker() SPDLOG_NOEXCEPT
    {
        _running.store(false, std::memory_order_release);
        _data_condition_variable.notify_all();

        if (_worker_threads == nullptr)
            return;

        for (std::size_t k = 0; k < _thread_count; ++k)
            _worker_threads[k].join();

        // by the time this is called,
        // all loggers should be deregistered;
        // otherwise, undefined behavior is invoked
        // (i.e. using an object that is being destroyed).
        // However, if someone did mess up,
        // it's still better to log what we can.
        // Also, note that deregister_logger erases the elements,
        // which would break the for-loop, so it's not used here
        for (auto& data : _data_map)
        {
            auto write_pos = data.second.queue->enqueue_pos();
            while (process_next_message(data.second))
            {
                // this check ensures that even
                // if the queue is still be added to
                // (which would be undefined behavior),
                // the destructor will not deadlock
                if (data.second.queue->dequeue_pos() >= write_pos)
                    break;
            }
        }
    }

    void notify_new_data()
    {
        if (_waiting_for_data.load(std::memory_order_acquire) > 0)
            _data_condition_variable.notify_one(); // only one piece of new data available, so only wake up one thread
    }

    void register_logger(async_logger& logger, queue_type& queue)
    {
        lock_data();
        auto was_empty = _data_map.empty();

        _data_map.emplace(std::piecewise_construct,
            std::forward_as_tuple(&logger),
            std::forward_as_tuple(
                queue,
                logger.sinks(),
                logger.name(),
                logger.formatter(),
                logger.error_handler()
            )
        );
        
        unlock_data();

        if (was_empty)
            _data_condition_variable.notify_one(); // there should only ever be one waiter
    }

    void deregister_logger(async_logger const& logger)
    {
        lock_data();

        // TODO push a message with a future/promise and wait on it to avoid locking for so long (pointer to promise will keep it trivially destructible)
        auto it = _data_map.find(&logger);
        if (it != _data_map.end())
        {
            auto write_pos = it->second.queue->enqueue_pos();
            while (process_next_message(it->second))
            {
                // should never happen, since logger only calls this method in destructor,
                // and this would imply that new messages have been added since the destructor was called,
                // which is undefined behavior (and obviously bad).
                // However, this ensures the deregister operation is wait-free
                if (it->second.queue->dequeue_pos() >= write_pos)
                    break;
            }
        }
        
        unlock_data();
    }

    void update_error_handler(async_logger const& logger, log_err_handler err_handler)
    {
        lock_data();

        auto it = _data_map.find(&logger);
        if (it != _data_map.end())
            it->second.err_handler = std::move(err_handler);
        
        unlock_data();
    }

    void update_formatter(async_logger const& logger, formatter_ptr msg_formatter)
    {
        lock_data();

        auto it = _data_map.find(&logger);
        if (it != _data_map.end())
            it->second.formatter = std::move(msg_formatter);

        unlock_data();
    }

private:
    void lock_data()
    {
        // prevent setting _lock_requested from multiple threads
        _registration_mutex.lock();

        _lock_requested.store(true, std::memory_order_release);
        _data_mutex.lock();
        _lock_requested.store(false, std::memory_order_release);
    }

    void unlock_data()
    {
        // order is important, because _registration_mutex is acquired first in lock_data method
        _data_mutex.unlock();
        _registration_mutex.unlock();
    }

    void work_loop()
    {
        std::unique_lock<std::mutex> lock(_data_mutex);

        while (_running.load(std::memory_order_acquire))
        {
            std::size_t count = 0;

            for (auto& log_data : _data_map)
            {
                if (process_next_message(log_data.second))
                    count += 1;
            }

            if (_lock_requested.load(std::memory_order_acquire))
            {
                lock.unlock();

                while (_lock_requested.load(std::memory_order_acquire))
#if defined(__GNUC__) || defined(__clang__)
                    __builtin_ia32_pause();
#else
                    ; // Windows provides YieldProcessor, but that would require bringing in Windows.h
#endif

                lock.lock();
                
                if (_data_map.empty())
                    _data_condition_variable.wait(lock,
                        [this]
                        {
                            return !_data_map.empty()
                                || !_running.load(std::memory_order_acquire);
                        }
                    );
            }
            else if (count == 0)
            {
                _waiting_for_data.store(true, std::memory_order_release);

                // predicate is checked before waiting,
                // so if data became available since setting _waiting_for_data and notify was already called,
                // this won't cause a deadlock
                _data_condition_variable.wait(lock,
                    [this]
                    {
                        return _data_available.load(std::memory_order_acquire)
                            || !_running.load(std::memory_order_acquire);
                    }
                );

                // order is important here: since the waiting flag is checked
                // before the new data flag is set, if we clear the waiting flag
                // before clearing the new data flag, we can be sure that no
                // writer will set the new_data flag again before we request it
                _waiting_for_data.store(false, std::memory_order_release);
                _data_available.store(false, std::memory_order_relaxed); // this thread is the only reader, so relaxed memory order
            }
        }
    }

    bool process_next_message(async_logger_data& data)
    {
        try
        {
            details::async_log_msg async_msg;
            if (!data.queue->dequeue(async_msg))
                return false;

            details::log_msg msg(&data.logger_name, async_msg.level);
            msg.time      = async_msg.time;
            msg.thread_id = async_msg.thread_id;
            msg.msg_id    = async_msg.msg_id;

            msg.raw << async_msg.text;

            data.formatter->format(msg);

            for (auto&& sink : *data.sinks)
                if (sink->should_log(msg.level))
                    sink->log(msg);
        }
        catch (std::exception const& e)
        {
            data.err_handler(e.what());
        }
        catch (...)
        {
            data.err_handler("Unknown exeption in async logger worker loop.");
        }

        return true;
    }
};

} // namespace detail
} // namespace spdlog
