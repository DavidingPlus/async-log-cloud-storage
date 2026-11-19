#include <iostream>

#include "server/config.h"
#include "server/log.h"
#include "server/service.h"
#include "utils/common.h"
#include "utils/fileutil.h"


int main()
{
    int res = EXIT_SUCCESS;

    try
    {
        // 1. 获取 Config 单例并且读取配置。创建日志管理器和日志器结构。
        auto &config = server::Config::GetInstance();

        auto &logManager = dlog::LogManager::GetInstance();
        server::g_logger = logManager.buildLogger<dlog::RollFileFlush>("server", config.getLogFileBasePath(), 64LL * 1024 * 1024);

        DLOG_LOGGER_INFO(server::g_logger) << "Server starting";
        std::cout << "Server starting" << std::endl;

        // 2. 准备压缩和未压缩文件的存储目录。createDirectory() 会递归创建缺失的父目录，已有目录保持不变。
        utils::FileUtil(config.getCompressedFilesDirPath()).createDirectory();
        utils::FileUtil(config.getUncompressedFilesDirPath()).createDirectory();

        // 3. 准备元数据文件的存储目录。
        auto metadataPath = config.getMetadataFilePath();
        utils::FileUtil(metadataPath.parent_path()).createDirectory();

        // 4. 若元数据文件缺失，写入空 JSON 数组 []，表示当前没有文件记录。DataManager 构造时会读取元数据文件；如果文件不存在，就会报错。
        // symlink_status() 检查路径本身，连指向不存在目标的符号链接也视为已占用，避免沿链接创建文件。
        if (!std::filesystem::exists(std::filesystem::symlink_status(metadataPath))) utils::FileUtil(metadataPath).setContent("[]");

        // Service 和 HTTP 资源在关闭日志系统之前释放。
        {
            // 5. Service 构造时同时构造成员 DataManager，从刚准备好的元数据文件加载历史记录到内存。
            server::Service service;

            // 6. 按配置监听 HTTP 请求并阻塞运行，收到 SIGINT/SIGTERM 后退出并释放资源。
            service.run();
        }

        DLOG_LOGGER_INFO(server::g_logger) << "Server stopped";
        std::cout << "Server stopped" << std::endl;
    }
    catch (const std::exception &error)
    {
        if (server::g_logger)
        {
            DLOG_LOGGER_ERROR(server::g_logger) << "Server failed: " << utils::urlEncode(error.what());
            std::cerr << "Server failed: " << utils::urlEncode(error.what()) << '\n';
        }
        res = EXIT_FAILURE;
    }

    dlog::LogManager::Shutdown();


    return res;
}
