#include <iostream>
#include "threadpool.h"

int heavy_task(int id)
{
    // 让每个任务睡 10 毫秒，模拟真实工作负载
    // 2000 个任务单线程需要 20 秒，看我们的线程池能压缩到多少！
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return id * id; // 返回平方值用于后续校验
}

int main(int argc, char const *argv[])
{
    Threadpool pool;
    pool.set_mode(poolmode::mode_cached);
    pool.start();

    int task_count = 2000;
    std::vector<Result> results;
    results.reserve(task_count);

    auto start_time = std::chrono::high_resolution_clock::now();

    std::cout << ">>> 正在疯狂提交 " << task_count << " 个任务..." << std::endl;
    for (int i = 0; i < task_count; i++)
    {
        results.push_back(pool.sumbit_task(heavy_task, i));
    }
    std::cout << ">>> 所有任务已提交完毕，等待消费者处理..." << std::endl;
    long long total_sum = 0;
    for (int i = 0; i < task_count; i++)
    {
        total_sum += results[i].get<int>();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end_time - start_time;

    std::cout << "处理总任务数: " << task_count << std::endl;
    std::cout << "前5个任务结果校验: ";
    for (int i = 0; i < 5; i++)
        std::cout << results[i].get<int>() << " ";
    std::cout << "\n总耗时: " << diff.count() << " 秒" << std::endl;

    std::cout << "\n>>> 压测结束，主线程休眠 65 秒..." << std::endl;
    std::cout << ">>> 请观察终端：闲置线程是否会自动销毁，直到剩下 4 个！" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(65));
    return 0;
}
