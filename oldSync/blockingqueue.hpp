#ifndef BLOCKINGQUEUE_HPP
#define BLOCKINGQUEUE_HPP

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

// Based on https://www.justsoftwaresolutions.co.uk/threading/implementing-a-thread-safe-queue-using-condition-variables.html
template<typename Data>
class BlockingQueue {
private:
    std::queue<Data>            queue;
    mutable std::mutex        queue_mutex;
    size_t                queue_limit;

    bool                        is_closed = false;

    std::condition_variable   new_item_or_closed_event;
    std::condition_variable   item_removed_event;
    std::function<void (int)> limitReachedFunc;
    int notifLimit = 0;

public:
    BlockingQueue(size_t size_limit=0) : queue_limit(size_limit)
    {}

    void setLimitReachedFunc(std::function<void (int)> f, int l)
    {
        limitReachedFunc = f;
        notifLimit = l;
    }

    void push(const Data& data)
    {
        bool notify = false;
        {
            std::unique_lock lock(queue_mutex);
            if (queue_limit > 0) {
                while (queue.size() >= queue_limit) {
                    item_removed_event.wait(lock);

                    if (is_closed)
                        throw std::runtime_error("Closed");
                }
            }

            queue.push(data);

            if (notifLimit > 0 && queue.size() == notifLimit)
                notify = true;
            new_item_or_closed_event.notify_one();
        }

        if (notify && limitReachedFunc)
            limitReachedFunc(notifLimit);
    }

   /* bool try_push(const Data& data)
    {
        std::unique_lock lock(queue_mutex);
        if (queue_limit > 0) {
            if (queue.size() >= queue_limit) {
                return false;
            }
        }
        assert (!is_closed);
        queue.push(data);

        new_item_or_closed_event.notify_one();
        return true;
    }*/

    void close()
    {
        std::unique_lock lock(queue_mutex);
        is_closed = true;

        new_item_or_closed_event.notify_all();
        item_removed_event.notify_all();
    }

    bool pop(Data &popped_value)
    {
        bool isZero = false;
        {
            std::unique_lock lock(queue_mutex);
            while (queue.empty()) {
                if (is_closed) {
                    return false;
                }
                new_item_or_closed_event.wait(lock);
            }

            popped_value = queue.front();
            queue.pop();
            isZero = queue.empty();
            item_removed_event.notify_one();
        }

        if (isZero && limitReachedFunc)
            limitReachedFunc(0);
        return true;
    }

   /* bool try_pop(Data &popped_value)
    {
        std::unique_lock lock(queue_mutex);
        if (queue.empty()) {
            return false;
        }

        popped_value = queue.front();
        queue.pop();
        item_removed_event.notify_one();
        return true;
    }*/

    bool empty() const
    {
        std::unique_lock lock(queue_mutex);
        return queue.empty();
    }

    bool closed() const
    {
        std::unique_lock lock(queue_mutex);
        return is_closed;
    }

    size_t limit() const
    {
        std::unique_lock lock(queue_mutex);
        return queue_limit;
    }

    void set_limit(size_t limit)
    {
        std::unique_lock lock(queue_mutex);
        queue_limit = limit;
    }

    size_t size() const
    {
        std::unique_lock lock(queue_mutex);
        return queue.size();
    }

};


#endif //BLOCKINGQUEUE_HPP
