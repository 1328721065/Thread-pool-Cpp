#pragma once
#include <vector>
#include <memory>
#include <queue>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <iostream>

// 接收任意数据的类型
class Any
{
public:
    // === 接收数据 ===
    template <typename T>
    Any(T data)
    {
        //<Derive<T>>在使用drive时要指明T的类型，在编译时才能知道对象占用多少内存
        base_ptr = std::make_unique<Derive<T>>(data);
    }
    Any() = default;
    ~Any() = default;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    // === 取数据 ===
    // 必须显式告诉它你要取什么类型
    template <typename T>
    T any_cast()
    {
        // static_cast	     编译期		基础类型转换，类层次间的逻辑转换
        // dynamic_cast	     运行期		多态：基类指针转派生类指针 (带检查)
        // const_cast	     编译期	    去掉 const 属性
        // reinterpret_cast	 编译期		指针转整数，底层内存操作
        Derive<T> *ptr = dynamic_cast<Derive<T> *>(base_ptr.get());
        if (ptr == nullptr)
        {
            throw std::runtime_error("类型转换失败！"); // 例如构造时传入的int，使用any_cast却传入long
        }
        return ptr->data;
    }

private:
    class base
    {
    public:
        // 虚析构函数（为了能正确删除派生类）
        virtual ~base() = default;
    };
    template <typename T>
    class Derive : public base
    {
    public:
        Derive(T mdata) : data(mdata) {}
        T data; // 真正存数据的地方
    };

private:
    std::unique_ptr<base> base_ptr;
};

// 实现一个信号量类
class Semahore
{

public:
    Semahore(int limit = 0) : res_limit(limit) {}
    ~Semahore() = default;
    Semahore(const Semahore &) = delete;            // 禁用拷贝构造
    Semahore &operator=(const Semahore &) = delete; // 禁用赋值构造

    void wait() // 获取一个信号量资源
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&]() -> bool
                { return res_limit > 0; });
        res_limit--;
    }
    void post() // 增加一个信号量资源
    {
        std::unique_lock<std::mutex> lock(mtx);
        res_limit++;
        cv.notify_all();
    }

private:
    int res_limit;
    std::mutex mtx;
    std::condition_variable cv;
};

// 实现接收提交到线程池的task任务执行完成之后的返回值Result
class Result
{
public:
    Result() : state(std::make_shared<SharedState>()) {}
    ~Result() = default;

    // === 供工作线程调用 ===
    void set_val(Any val)
    {
        state->any = std::move(val);
        state->is_ready = true;
        state->sem.post();
    }

    // === 供主线程调用 ===
    template <typename T>
    T get()
    {
        if (!state->is_ready)
        {
            state->sem.wait();
        }
        return state->any.any_cast<T>();
    }

private:
    struct SharedState
    {
        Any any;      // 任务返回值
        Semahore sem; // 线程通信信号量
        std::atomic<bool> is_ready{false};
    };
    std::shared_ptr<SharedState> state;
};

enum poolmode
{
    mode_fixed,  // 固定数量的线程
    mode_cached, // 线程数量可动态增长
};

class Thread
{
public:
    // 定义一个函数类型别名：接收一个 int 参数(线程id)，返回 void
    using thread_func = std::function<void(int)>;

    Thread(thread_func func, int no)
        : func(func),
          thread_id(no)
    {
    }
    // 线程对象销毁时，如果底层的 std::thread 还在运行，程序会直接奔溃
    ~Thread()
    {
        // joinable不是在问“这个线程能不能加入到某个组织里”
        // 而是std::thread对象现在是不是和一个正在运行的线程关联着
        // 如果是，那我就必须处理它（这里选择 detach），否则程序会崩溃。
        if (thread_.joinable())
        {
            thread_.detach();
        }
    }
    void start()
    {
        // 变参模板构造,std::thread的构造函数大概长这样
        // template<class Function, class... Args>
        // explicit thread(Function&& f, Args&&... args);
        // Function&& f接收任何“可调用”的东西（函数指针、Lambda、std::function、仿函数）
        // Args&&... args：这是一个参数包,塞给第1个参数去执行
        thread_ = std::thread(func, thread_id);
    }
    int get_id() const
    {
        return thread_id;
    }
    bool is_running() const { return running; }
    void set_running(bool b) { running = b; }
    bool is_joinable() const { return thread_.joinable(); }
    void join()
    {
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

private:
    std::atomic_bool running{false};
    thread_func func;
    std::thread thread_;
    int thread_id; // 自定义 ID
};

class Threadpool
{
public:
    Threadpool();
    ~Threadpool();

    void set_mode(poolmode mode);            // 设置线程池模式
    void set_init_thread_nums(int size);     // 设置初始的线程数量
    void set_thread_max_nums(int size);      // 设置线程上限的阈值
    void set_task_max_nums(int thread_hold); // 设置任务队列上限的阈值
    void start();                            // 开启线程池

    template <typename Func, typename... Args>
    Result sumbit_task(Func &&func, Args &&...args)
    {
        // cleanup_dead_threads();

        // 推导函数的返回值类型
        using ReturnType = decltype(func(args...));

        // 把函数和参数绑在一起，变成一个无参对象
        // <>是类型包，提供了每个参数在原始调用时的类型（含引用信息）。
        // () 是值包，提供了每个参数的实际值。
        auto bound_task = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);

        // 拿一个空的取件码
        Result res;

        // 默认情况下，lambda的operator()是const的，即不能修改按值捕获的变量（即使这些变量本身不是 const 的，但在 lambda 内部它们被视为 const）。
        // 加上 mutable 之后，Lambda内部的res就不再是const了，可以调用 res.set_val()
        auto wrapper_task = [bound_task, res]() mutable
        {
            if constexpr (std::is_void<ReturnType>::value)
            {
                bound_task(); // ① 工作线程真正执行用户的任务
                // 任务执行完后，立刻把空数据塞进盲盒并发出通知
                // (因为现在参数 task 是 void()，没有返回值，所以传个空的 Any 进去)
                res.set_val(Any());
            }
            else
            {
                ReturnType ret = bound_task();
                res.set_val(Any(ret));
            }
        };

        {
            std::unique_lock<std::mutex> lock(task_queue_mtx);

            // 1. 等待队列不满 (Wait if Full)
            // 逻辑：如果 (任务数量 >= 最大阈值)，就解锁睡觉。
            // 醒来条件：消费者取走了任务，发出了 not_full 的通知。
            // 注意：这里用的是 task_nums (原子变量) 或 tasks.size() 都可以，因为我们在锁里。
            if (!not_full.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                                   { return task_nums < tasks_max_nums; }))
            {
                std::cerr << "任务队列已满，提交任务失败！" << std::endl;
                return res;
            }

            // 2.双重检查
            if (!is_pool_running)
            {
                return res;
            }

            // 3.放入任务
            tasks.push(wrapper_task);
            task_nums++;

            // 4. 通知消费者
            not_empty.notify_one();
        }

        // cached模式下，耗时的扩容操作
        if (pool_mode == poolmode::mode_cached && task_nums > idle_thread_nums && thread_max_nums > cur_thread_nums)
        {
            std::cout << ">>> 任务积压，开始扩容新线程！" << std::endl;
            int new_id = next_thread_id++;
            auto ptr = std::make_unique<Thread>(std::bind(&(Threadpool::threadFunc), this, std::placeholders::_1), new_id);
            ptr->start();
            {
                std::unique_lock<std::mutex> lock(thread_list_mtx);
                thread_list.emplace_back(move(ptr));
            }
            cur_thread_nums++;
            idle_thread_nums++;
        }

        return res;
    }

    // threadpool(const threadpool &)  类的拷贝构造函数。它的作用是“用一个已经存在的对象，克隆出一个新对象”。
    //= delete;表示不要自动生成这个函数，也不允许任何人调用这个函数。
    Threadpool(const Threadpool &) = delete;
    // 作用是禁止类似于threadpool tp2 = tp1;的操作,也就是禁止赋值
    Threadpool &operator=(const Threadpool &) = delete;

private:
    void threadFunc(int thread_id); // 线程函数，消费者函数
    bool check_running_state() const;
    Thread *get_thread_by_id(int id); // 找到属于哪一个thread对象
    void cleanup_dead_threads();      // 清理死亡线程的声明

private:
    std::mutex thread_list_mtx;                       // 专门用来保护 thread_list 的锁
    std::vector<std::unique_ptr<Thread>> thread_list; // 线程队列

    size_t thread_init_nums;          // 初始的线程数量
    std::atomic_int idle_thread_nums; // 空闲线程数量
    std::atomic_int cur_thread_nums;  // 当前线程数量
    size_t thread_max_nums;           // 最大线程数

    // 类型擦除 —— 抹去它们具体的类型差异，统一视为可调用对象。定义为无参无返
    std::queue<std::function<void()>> tasks; // 任务队列
    std::atomic_uint16_t task_nums;          // 任务数量
    uint16_t tasks_max_nums;                 // 最大任务数

    std::mutex task_queue_mtx;         // 任务队列锁
    std::condition_variable not_full;  // 表示任务队列不满，对应生产者线程
    std::condition_variable not_empty; // 表示任务队列不为空，对应消费者线程

    poolmode pool_mode;
    std::atomic_bool is_pool_running; // 线程池是否运行中
    std::atomic<int> next_thread_id;
};
