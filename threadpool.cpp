#include "threadpool.h"
#define Thead_init_nums 4
Threadpool::Threadpool()
    : thread_init_nums(Thead_init_nums),
      idle_thread_nums(0),
      cur_thread_nums(0),
      thread_max_nums(20),
      tasks_max_nums(1024),
      task_nums(0),
      pool_mode(poolmode::mode_fixed),
      next_thread_id(Thead_init_nums),
      is_pool_running(false)
{
}

Threadpool::~Threadpool()
{
    // 1.宣布关闭
    {
        std::unique_lock<std::mutex> lock(task_queue_mtx);
        is_pool_running = false;
    }

    // 2.唤醒所有线程
    //  为什么要唤醒？
    //  因为有的线程正阻塞在 threadFunc 里的 not_empty.wait()。
    //  如果不叫醒，它们会一直睡，导致程序无法正常退出（死锁）。

    // 叫醒消费者：别睡了，起来看看，我们要关门了！
    not_empty.notify_all();

    // 叫醒生产者：别等了，别再提交任务了，我们要关门了！
    not_full.notify_all();

    for (auto &t : thread_list)
    {
        if (t->is_running() || t->is_joinable())
        {
            t->join();
        }
    }
}

void Threadpool::set_mode(poolmode mode)
{
    if (check_running_state())
        return;
    pool_mode = mode;
}

void Threadpool::set_init_thread_nums(int size)
{
    if (check_running_state())
        return;
    thread_init_nums = size;
}

void Threadpool::set_thread_max_nums(int size)
{
    if (check_running_state())
        return;
    if (pool_mode == poolmode::mode_cached)
        thread_max_nums = size;
}

void Threadpool::set_task_max_nums(int thread_hold)
{
    if (check_running_state())
        return;
    tasks_max_nums = thread_hold;
}

void Threadpool::start()
{
    is_pool_running = true;
    cur_thread_nums = thread_init_nums;

    // 创建线程
    for (int i = 0; i < thread_init_nums; i++)
    {
        // 我们要创建一个 thread 对象，但thread的构造函数需要一个函数对象func。
        // 我们希望线程执行的是threadpool::threadFunc这个成员函数。
        // 问题是：成员函数需要this指针才能调用。

        // 解决方案：使用 std::bind
        // 含义：把"当前对象的 threadFunc"打包成一个可以在外面调用的函数。
        // std::placeholders::_1是占位符，对应threadFunc(int thread_id)的参数。
        //&threadpool::threadFunc：我要调用的函数代码在这里。
        // this：这个函数是作用在我当前这个 threadpool 对象上的（不要去调别人的）。
        // std::placeholders::_1：这是最关键的！
        //  threadFunc 需要接收一个 int 参数。
        // 但是我们在 bind 的时候（在主线程里），还不想马上把这个 int 填死（比如填成 0 或 1）。 我们希望把这个决定权交给将来执行它的那个 Thread 对象。
        // 所以 _1 的意思是：“这里先留个空儿（挖个坑），等真正调用这个 func 的时候，把传进来的第 1 个参数填到这里。”
        auto function = std::bind(&(Threadpool::threadFunc), this, std::placeholders::_1);
        // 创建 unique_ptr 管理的 thread 对象
        // 参数 func 传给 thread 构造函数，i是线程 ID

        auto ptr = std::make_unique<Thread>(function, i);
        {
            std::unique_lock<std::mutex> lock(thread_list_mtx);
            thread_list.emplace_back(std::move(ptr));
        }
    }
    // 启动线程
    for (int i = 0; i < thread_init_nums; i++)
    {
        thread_list[i]->start();
        idle_thread_nums++;
    }
}

void Threadpool::threadFunc(int thread_id)
{
    auto my_thread = get_thread_by_id(thread_id);
    if (my_thread)
    {
        my_thread->set_running(true);
    }
    for (;;)
    {
        std::function<void()> task;
        // 临界区开始 (锁的保护范围)
        {
            // 1. 拿锁
            std::unique_lock<std::mutex> lock(task_queue_mtx);
            std::cout << "线程 " << thread_id << " 正在等待任务..." << std::endl;

            // 2. 等待条件变量
            bool timeout = !not_empty.wait_for(lock, std::chrono::seconds(60), [&]() -> bool
                                               { return !(is_pool_running && tasks.empty()); });

            if (timeout && tasks.empty())
            {
                if (pool_mode == poolmode::mode_cached && cur_thread_nums > thread_init_nums)
                {
                    cur_thread_nums--;
                    idle_thread_nums--;
                    if (my_thread)
                        my_thread->set_running(false);
                    break;
                }
            }
            // 3. 醒来后，先检查是不是因为“线程池关闭”而醒来的
            if (!is_pool_running && tasks.empty())
            {
                idle_thread_nums--;
                if (my_thread)
                    my_thread->set_running(false);
                break;
            }
            if (tasks.empty())
            {
                continue;
            }

            // 4. 取任务 (Pop)
            task = tasks.front();
            tasks.pop();
            task_nums--;
            idle_thread_nums--;

            // 5. 如果有人（提交者）因为队列满了在等，现在告诉他有空位了
            if (task_nums < tasks_max_nums)
            {
                not_full.notify_all();
            }
        }

        // 执行任务
        // 此时锁已经解开
        if (task) // 重载了 operator bool
        {
            std::cout << "线程 " << thread_id << "以此获取任务成功，开始执行..." << std::endl;
            task(); // 重载了 operator()
        }
        idle_thread_nums++;
    }
    std::cout << "=== [退出提示] 线程 " << thread_id << " 已经彻底销毁！ ===" << std::endl;
}

bool Threadpool::check_running_state() const
{
    return is_pool_running;
}

Thread *Threadpool::get_thread_by_id(int id)
{
    std::unique_lock<std::mutex> lock(thread_list_mtx);
    for (auto &t : thread_list)
    {
        if (t->get_id() == id)
        {
            return t.get();
        }
    }
    return nullptr;
}

void Threadpool::cleanup_dead_threads()
{
    std::unique_lock<std::mutex> lock(thread_list_mtx);
    auto it = thread_list.begin();
    while (it != thread_list.end())
    {
        if (!(*it)->is_running())
        {
            // 1. erase 会把这个 unique_ptr 从 vector 里移除。
            // 2. unique_ptr 失去所有权后，会自动触发 Thread 对象的析构函数 `~Thread()`。
            // 3. 于是，这个彻底死亡的线程被安全剥离，底层系统内存被操作系统回收！
            it = thread_list.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
