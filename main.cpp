#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <chrono>
#include <functional>
#include <atomic>
#include <unordered_map>
#include <optional>

enum class TaskStatus { Pending, Running, Done };
std::mutex cout_mutex;


class Task {
public:
    Task(int id, int exec_time_ms)
        : task_id(id), execution_time_ms(exec_time_ms) {}

    void operator()() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = TaskStatus::Running;
        }

        auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[Task " << task_id << "] Started. Will take " << execution_time_ms << " ms.\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(execution_time_ms));
        auto end = std::chrono::steady_clock::now();

        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = TaskStatus::Done;
        }

        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[Task " << task_id << "] Finished in " << duration << " ms.\n";
        }
    }

    int getId() const { return task_id; }

    int getDuration() const { return duration; }

    TaskStatus getStatus() const {
        std::lock_guard<std::mutex> lock(mutex);
        return status;
    }

private:
    int task_id;
    int execution_time_ms;
    int duration = 0;
    mutable std::mutex mutex;
    TaskStatus status = TaskStatus::Pending;
};

class TaskQueue {
public:
    TaskQueue(size_t max_size) : max_tasks(max_size) {}

    bool try_push(const std::shared_ptr<Task>& task) {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.size() >= max_tasks) return false;

        queue.push(task);

        if (queue.size() == max_tasks && !full_start_time.has_value()) {
            full_start_time = std::chrono::steady_clock::now();
        }

        cond_var.notify_one();
        return true;
    }

    bool pop(std::shared_ptr<Task>& task) {
        std::unique_lock<std::mutex> lock(mutex);
        cond_var.wait(lock, [&]() { return !queue.empty() || terminate_flag; });

        if (terminate_flag && queue.empty()) return false;

        task = queue.front();
        queue.pop();

        if (queue.size() < max_tasks && full_start_time.has_value()) {
            auto end = std::chrono::steady_clock::now();
            long full_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - *full_start_time).count();

            if (min_full_time == 0 || full_time < min_full_time)
                min_full_time = full_time;
            if (full_time > max_full_time)
                max_full_time = full_time;

            full_start_time.reset();
        }

        return true;
    }

    void terminate() {
        std::unique_lock<std::mutex> lock(mutex);
        terminate_flag = true;
        cond_var.notify_all();
    }

    long getMinFullTime() const { return min_full_time; }
    long getMaxFullTime() const { return max_full_time; }

private:
    std::queue<std::shared_ptr<Task>> queue;
    std::mutex mutex;
    std::condition_variable cond_var;
    size_t max_tasks;
    bool terminate_flag = false;

    std::optional<std::chrono::steady_clock::time_point> full_start_time;
    long min_full_time = 0;
    long max_full_time = 0;
};

class ThreadPool {
public:
    ThreadPool(size_t num_threads = 6, size_t queue_limit = 20)
        : queue(queue_limit), terminate_flag(false), completed_count(0) {
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this]() { worker_loop(); });
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    std::optional<int> add_task(int exec_time_ms) {
        int id = task_id_counter++;
        auto task = std::make_shared<Task>(id, exec_time_ms);

        if (!queue.try_push(task)) {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "[Task " << id << "] Dropped (queue full).\n";
            return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(map_mutex);
            task_map[id] = task;
        }

        return id;
    }

    TaskStatus get_task_status(int id) {
        std::lock_guard<std::mutex> lock(map_mutex);
        if (task_map.count(id)) {
            return task_map[id]->getStatus();
        }
        return TaskStatus::Pending;
    }

    std::optional<int> get_task_result(int id) {
        std::lock_guard<std::mutex> lock(map_mutex);
        if (task_map.count(id) && task_map[id]->getStatus() == TaskStatus::Done) {
            return task_map[id]->getDuration();
        }
        return std::nullopt;
    }

    void shutdown() {
        if (terminate_flag) return;
        terminate_flag = true;
        queue.terminate();
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    int get_completed_count() const {
        return completed_count.load();
    }

    long getQueueMaxFullTime() const {
        return queue.getMaxFullTime();
    }

    long getQueueMinFullTime() const {
        return queue.getMinFullTime();
    }

    void pause() {
        paused = true;
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[ThreadPool] Paused.\n";

    }

    void resume() {
        {
            std::lock_guard<std::mutex> lock(pause_mutex);
            paused = false;
        }
        pause_cv.notify_all();
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "[ThreadPool] Resumed.\n";
    }


private:
    void worker_loop() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(pause_mutex);
                pause_cv.wait(lock, [&]() { return !paused || terminate_flag; });
            }

            std::shared_ptr<Task> task;
            if (!queue.pop(task)) break;

            (*task)();
            completed_count++;
        }
    }


    TaskQueue queue;
    std::vector<std::thread> workers;
    std::atomic<bool> terminate_flag;
    std::atomic<int> task_id_counter = 0;
    std::atomic<int> completed_count = 0;

    std::unordered_map<int, std::shared_ptr<Task>> task_map;
    std::mutex map_mutex;

    std::atomic<bool> paused = false;
    std::mutex pause_mutex;
    std::condition_variable pause_cv;

};

int main() {
    constexpr int total_tasks = 60;
    constexpr int producer_count = 3;
    std::atomic<int> global_task_id = 0;
    std::atomic<int> dropped_count = 0;

    ThreadPool pool;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist_time(5000, 10000);
    std::uniform_int_distribution<> delay(100, 200);

    std::vector<std::thread> producers;

    for (int i = 0; i < producer_count; ++i) {
        producers.emplace_back([&]() {
            while (true) {
                int current_id = global_task_id.fetch_add(1);
                if (current_id >= total_tasks) break;

                int exec_time = dist_time(gen);
                auto id_opt = pool.add_task(exec_time);
                if (!id_opt) dropped_count++;

                std::this_thread::sleep_for(std::chrono::milliseconds(delay(gen)));
            }
        });
    }


    for (auto& t : producers) t.join();


    std::cout << "\nAll tasks submitted. Waiting for completion...\n";

    std::this_thread::sleep_for(std::chrono::seconds(2));
    pool.pause();
    std::cout << "\n[Main] Pool paused for 5 seconds...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));
    pool.resume();

    pool.shutdown();

    std::cout << "\n=== Report ===\n";
    std::cout << "Total submitted: " << total_tasks << "\n";
    std::cout << "Dropped: " << dropped_count.load() << "\n";
    std::cout << "Completed: " << pool.get_completed_count() << "\n";
    std::cout << "Max time queue was full: " << pool.getQueueMaxFullTime() << " ms\n";
    std::cout << "Min time queue was full: " << pool.getQueueMinFullTime() << " ms\n";

    return 0;
}
