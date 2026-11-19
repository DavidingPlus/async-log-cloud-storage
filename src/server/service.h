#ifndef _ASYNC_LOG_CLOUD_STORAGE_SERVER_SERVICE_H_
#define _ASYNC_LOG_CLOUD_STORAGE_SERVER_SERVICE_H_

#include "globalmacros.h"

#include "server/datamanager.h"

#include <filesystem>
#include <string>
#include <vector>


struct evhttp_request;


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_BEGIN(server)


// 文件存储类型枚举。
enum class StorageType
{
    Uncompressed,
    Compressed
};


// 基于 libevent 的 HTTP 文件存储服务。
class Service
{

public:

    // 读取服务配置并构造元数据管理器；元数据文件须提前准备，加载失败时抛出异常。
    Service();

    // 启动 HTTP 监听并阻塞运行事件循环；SIGINT/SIGTERM 退出循环，自动关闭监听并释放资源后正常返回，由应用继续刷新日志。不等待所有在途响应发送完成。创建资源、绑定监听、注册信号或运行事件循环失败时抛出 std::runtime_error。
    void run();


private:

    static constexpr size_t kMaxFileSize = 16 * 1024 * 1024;


    // libevent 通用请求回调，解析并解码请求路径后分发请求。URI/路径缺失或百分号编码非法返回 400，未处理的业务异常返回 500，不允许异常逸出事件循环。
    static void GenericCallback(evhttp_request *req, void *arg) noexcept;

    // 上传处理：解码并校验 FileName（单个文件名、有效 UTF-8、1~255 字节），原始文件超过 kMaxFileSize 返回 413；按 StorageType 直接保存或压缩后保存，并持久化元数据，成功返回 201 和 JSON。允许上传零字节文件：正文长度为 0 时跳过字节复制，仍创建空文件并保存元数据，不视为错误。文件创建、写入或元数据保存失败时返回 500，并尝试清理本次上传文件。
    static void Upload(evhttp_request *req, void *arg);

    // 查询元数据并返回包含文件列表的 HTML 页面。
    static void ShowFileList(evhttp_request *req, void *arg);

    // GET /api/files 返回文件元数据的 JSON 列表。
    static void ListFiles(evhttp_request *req, void *arg);

    // 下载处理：仅接受 GET，返回原文字节、下载文件名和 ETag。支持单区间 Range 和强 ETag 的 If-Range；部分下载返回 206，不可满足返回 416，其余返回完整 200。普通文件按区间读取，压缩文件在内存中解压。支持零字节文件；记录或文件不存在返回 404，存储目录无法识别或读取/解压异常返回 500。缓存条件请求仍未接入。
    static void Download(evhttp_request *req, void *arg);

    // 按服务端本地时区格式化为 YYYY-MM-DD HH:MM:SS，时钟转换在函数内部完成。
    static std::string FormatFileTime(std::filesystem::file_time_type t);

    // 将文件字节数按 1024 进位格式化为 B/KB/MB/GB，保留两位小数。
    static std::string FormatFileSize(uint64_t bytes);

    // 根据元数据生成文件列表 HTML。
    static std::string GenerateFileListHtml(const std::vector<FileMetadata> &files);

    // 根据文件元数据生成版本标识，用于缓存验证和断点续传。
    static std::string GetETag(const FileMetadata &info);


    // 服务监听地址。
    std::string m_serverIp;

    // 服务监听端口。
    int m_serverPort = 0;

    // 下载 URL 前缀。
    std::string m_downloadPrefix;

    // 服务拥有的元数据管理器，构造时从持久化文件加载元数据到内存中。
    DataManager m_dataManager;
};


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_END


#endif
