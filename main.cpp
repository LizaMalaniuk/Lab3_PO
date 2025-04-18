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

// Тип статусу задачі
enum class TaskStatus { Pending, Running, Done };

// Клас задачі
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
        std::cout << "[Task " << task_id << "] Started. Will take " << execution_time_ms << " ms.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(execution_time_ms));
        auto end = std::chrono::steady_clock::now();

        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = TaskStatus::Done;
        }

        std::cout << "[Task " << task_id << "] Finished in " << duration << " ms.\n";
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

// Черга задач з обмеженням
class TaskQueue {
public:
    TaskQueue(size_t max_size) : max_tasks(max_size) {}

    bool try_push(const std::shared_ptr<Task>& task) {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.size() >= max_tasks) return false;
        queue.push(task);
        cond_var.notify_one();
        return true;
    }

    bool pop(std::shared_ptr<Task>& task) {
        std::unique_lock<std::mutex> lock(mutex);
        cond_var.wait(lock, [&]() { return !queue.empty() || terminate_flag; });

        if (terminate_flag && queue.empty()) return false;

        task = queue.front();
        queue.pop();
        return true;
    }

    void terminate() {
        std::unique_lock<std::mutex> lock(mutex);
        terminate_flag = true;
        cond_var.notify_all();
    }

private:
    std::queue<std::shared_ptr<Task>> queue;
    std::mutex mutex;
    std::condition_variable cond_var;
    size_t max_tasks;
    bool terminate_flag = false;
};

// Пул потоків
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

    // Додати задачу
    std::optional<int> add_task(int exec_time_ms) {
        int id = task_id_counter++;
        auto task = std::make_shared<Task>(id, exec_time_ms);

        if (!queue.try_push(task)) {
            std::cout << "[Task " << id << "] Dropped (queue full).\n";
            return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(map_mutex);
            task_map[id] = task;
        }

        return id;
    }

    // Отримати статус задачі
    TaskStatus get_task_status(int id) {
        std::lock_guard<std::mutex> lock(map_mutex);
        if (task_map.count(id)) {
            return task_map[id]->getStatus();
        }
        return TaskStatus::Pending;
    }

    // Отримати результат виконання задачі
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

private:
    void worker_loop() {
        while (true) {
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
};


int main() {
    constexpr int total_tasks = 60;
    std::atomic<int> dropped_count = 0;

    ThreadPool pool;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist_time(5000, 10000);
    std::uniform_int_distribution<> delay(100, 200);

    std::vector<int> task_ids;

    for (int i = 0; i < total_tasks; ++i) {
        int exec_time = dist_time(gen);
        auto id_opt = pool.add_task(exec_time);
        if (!id_opt) {
            dropped_count++;
        } else {
            task_ids.push_back(*id_opt);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(delay(gen)));
    }

    std::cout << "\nAll tasks submitted. Waiting for completion...\n";
    pool.shutdown();

    std::cout << "\n=== Report ===\n";
    std::cout << "Total submitted: " << total_tasks << "\n";
    std::cout << "Dropped: " << dropped_count.load() << "\n";
    std::cout << "Completed: " << pool.get_completed_count() << "\n";

    for (int id : task_ids) {
        auto result = pool.get_task_result(id);
        if (result)
            std::cout << "Task " << id << " duration: " << *result << " ms\n";
    }

    return 0;
}
// TIP See CLion help at <a
// href="https://www.jetbrains.com/help/clion/">jetbrains.com/help/clion/</a>.
//  Also, you can try interactive lessons for CLion by selecting
//  'Help | Learn IDE Features' from the main menu.