#include <iostream>

#include "server/config.h"
#include "server/service.h"
#include "utils/fileutil.h"


int main()
{
    try
    {
        // 1. 获取 Config 单例并且读取配置。
        auto &config = server::Config::GetInstance();

        // 2. 准备压缩和未压缩文件的存储目录。createDirectory() 会递归创建缺失的父目录，已有目录保持不变。
        utils::FileUtil(config.getCompressedFilesDirPath()).createDirectory();
        utils::FileUtil(config.getUncompressedFilesDirPath()).createDirectory();

        // 3. 准备元数据文件的存储目录。
        auto metadataPath = config.getMetadataFilePath();
        utils::FileUtil(metadataPath.parent_path()).createDirectory();

        // 4. 若元数据文件缺失，写入空 JSON 数组 []，表示当前没有文件记录。DataManager 构造时会读取元数据文件；如果文件不存在，就会报错。
        // symlink_status() 检查路径本身，连指向不存在目标的符号链接也视为已占用，避免沿链接创建文件。
        if (!std::filesystem::exists(std::filesystem::symlink_status(metadataPath))) utils::FileUtil(metadataPath).setContent("[]");

        // 5. Service 构造时同时构造成员 DataManager，从刚准备好的元数据文件加载历史记录到内存。
        server::Service service;

        // 6. 按配置监听 HTTP 请求并阻塞运行。收到 SIGINT/SIGTERM 后退出事件循环，释放监听和事件资源。run() 正常返回后 Service 自动销毁，main 执行结束时隐式返回 0；启动或运行异常统一由下方捕获。
        service.run();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Server failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
