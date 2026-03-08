#include <iostream>
#include "threadpool.h"

int add(int a, int b)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return a + b;
}

std::string concat_str(std::string a, std::string b)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return a + b;
}

// 任务 3：无返回值 (void)
void heavy_task(int id)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "任务 " << id << " 执行完毕（无返回值）" << std::endl;
}

int main(int argc, char const *argv[])
{
    Threadpool pool;
    pool.set_mode(poolmode::mode_cached);
    pool.start();

    std::cout << "=== 开始疯狂提交不同类型的任务 ===" << std::endl;

    Result res1 = pool.sumbit_task(add, 10, 20);
    Result res2 = pool.sumbit_task(concat_str, "Hello, ", "Threadpool!");
    Result res3 = pool.sumbit_task(heavy_task, 99);
    std::cout << "=== 任务提交完毕，主线程开始获取结果 ===" << std::endl;
    int sum = res1.get<int>();
    std::string str = res2.get<std::string>();
    std::cout << "加法结果: " << sum << std::endl;
    std::cout << "拼接结果: " << str << std::endl;
    return 0;
}
