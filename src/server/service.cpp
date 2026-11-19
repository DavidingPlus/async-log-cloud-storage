#include "server/service.h"

#include "server/config.h"
#include "server/log.h"
#include "utils/common.h"
#include "utils/fileutil.h"
#include "utils/jsonutil.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <base64.h>

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/util.h>

#include <utf8.h>


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_BEGIN(server)


namespace
{

    std::optional<StorageType> getStorageTypeFromText(std::string_view text)
    {
        if ("uncompressed" == text)
        {
            return StorageType::Uncompressed;
        }
        else if ("compressed" == text)
        {
            return StorageType::Compressed;
        }
        else
        {
            return std::nullopt;
        }
    }

    std::optional<StorageType> getStorageTypeFromFilePath(const std::filesystem::path &filePath)
    {
        auto &config = Config::GetInstance();

        // 将配置目录与文件名组合后比较完整路径，兼容目录末尾带 / 的写法。
        if ((config.getUncompressedFilesDirPath() / filePath.filename()).lexically_normal() == filePath)
        {
            return StorageType::Uncompressed;
        }
        else if ((config.getCompressedFilesDirPath() / filePath.filename()).lexically_normal() == filePath)
        {
            return StorageType::Compressed;
        }
        else
        {
            return std::nullopt;
        }
    }

    bool isValidFileName(std::string_view fileName)
    {
        // 1. 文件名不能为空，也不能是特殊目录名。
        if (fileName.empty() || "." == fileName || ".." == fileName) return false;

        // 2. 限制文件名长度，按字节计算。
        if (fileName.size() > 255) return false;

        // 3. Linux 文件名不能包含 '/' 或 NUL。
        if (std::string_view::npos != fileName.find('/') || std::string_view::npos != fileName.find('\0')) return false;

        // 4. 要求文件名符合 UTF-8 编码。
        return utf8::is_valid(fileName.begin(), fileName.end());
    }

    // 解析 Range 请求头的值，结合原文大小确定下载区间，供 Download() 决定响应状态及读取范围。
    int parseRangeHeader(std::string_view rangeHeader, size_t size, size_t &offset, size_t &length)
    {
        // 整体流程：去掉两端空白 → 检查单位和分隔符 → 解析数字 → 判断区间能否满足 → 计算起点和长度。
        // rangeHeader 是 Range 请求头的值；size 是原文总字节数，压缩存储时也按解压后的内容计算。offset、length 是引用输出参数，分别表示从第几个字节开始、读取多少字节；字节位置从 0 开始。
        // 例如请求头为 Range: bytes=4-9，evhttp_find_header() 取出的 rangeHeader 是 "bytes=4-9"。原文有 10 字节时，该值表示第 4～9 字节，包含两端；解析结果为 offset=4、length=6，返回 206。
        // 调用方须先设置 offset = 0、length = size。返回 200 时保留原值，用于完整下载；返回 206 时更新为实际区间。返回 416 表示区间不可满足，调用方不读取正文，而是返回 Content-Range: bytes */总大小。

        // 1. 去掉整个值两端的空格和制表符；全为空白或原文为空时忽略 Range，返回完整 200。
        size_t begin = rangeHeader.find_first_not_of(" \t");
        if (std::string_view::npos == begin || 0 == size) return HTTP_OK;
        rangeHeader = rangeHeader.substr(begin, rangeHeader.find_last_not_of(" \t") - begin + 1);

        // 2. 只接受 bytes 单位，前缀比较忽略大小写；移除 bytes= 后，用第一个 '-' 分隔左右两部分。前缀或分隔符不符合格式时回退完整 200。
        if (rangeHeader.size() < 6 || 0 != evutil_ascii_strncasecmp(rangeHeader.data(), "bytes=", 6)) return HTTP_OK;
        rangeHeader.remove_prefix(6);
        size_t dash = rangeHeader.find('-');
        if (std::string_view::npos == dash) return HTTP_OK;

        // 3. 用标准库解析十进制非负整数。ec 检查转换失败和溢出，ptr 检查是否消费了整个字符串，避免接受 2x 等值。
        auto number = [](std::string_view text, size_t &result)
        {
            if (text.empty()) return false;
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            return std::errc{} == parsed.ec && text.data() + text.size() == parsed.ptr;
        };

        auto first = rangeHeader.substr(0, dash), last = rangeHeader.substr(dash + 1);
        size_t start = 0, end = 0;

        // TODO 第一版只处理单区间，多区间及无法解析的数值回退完整 200，按需增加 multipart 支持。

        // 4. 左侧为空表示后缀区间 bytes=-N，即最后 N 个字节；这里 end 暂存请求的字节数，而非结束位置。
        if (first.empty())
        {
            if (!number(last, end)) return HTTP_OK;
            // 请求最后 0 字节不可满足；N 超过原文长度时返回全部原文字节，但仍属于有效区间，返回 206。
            if (0 == end) return 416;

            length = std::min(end, size);
            offset = size - length;
        }
        else
        {
            // 5. 左侧非空表示 bytes=start-end 或 bytes=start-；起止位置包含两端，右侧为空表示一直读到末尾。
            // 数字非法或结束位置小于起点时忽略 Range；格式有效但起点不在文件内时返回 416。
            if (!number(first, start)) return HTTP_OK;
            if (!last.empty() && (!number(last, end) || end < start)) return HTTP_OK;
            if (start >= size) return 416;

            offset = start;
            // 显式结束位置最多取 size - 1，长度再加 1 以包含末字节。例如 10 字节文件的 bytes=2-4 得到 offset=2、length=3。
            length = last.empty() ? size - offset : std::min(end, size - 1) - offset + 1;
        }


        // 6. 区间可满足，返回 206；Download() 使用 offset/length 读取片段并生成对应的 Content-Range。
        return 206;
    }

}


Service::Service()
{
    m_serverIp = Config::GetInstance().getServerIp();
    m_serverPort = Config::GetInstance().getServerPort();
    m_downloadPrefix = Config::GetInstance().getDownloadPrefix();
}

void Service::run()
{
    // 创建事件循环，使用 std::unique_ptr<T, Deleter> 独占管理资源。
    // 第一个模板参数 event_base 是对象类型，unique_ptr 内部保存 event_base*。
    // 第二个模板参数 decltype(&event_base_free) 获取删除器的类型，即 void (*)(event_base*)，不会执行该函数。
    // 两个构造参数分别是 event_base_new() 返回的资源指针，类型是 event_base *，以及 event_base_free 函数指针；后者保存起来，供释放资源时调用。
    // unique_ptr 默认使用 delete，这里指定 libevent 配套的释放函数；正常结束、提前 return 或因异常离开作用域时，自动释放非空指针。
    std::unique_ptr<event_base, decltype(&event_base_free)> base(event_base_new(), event_base_free);
    if (!base) throw std::runtime_error("Service::run(): event_base_new() failed");

    // base.get() 取出内部的原始指针供 libevent 使用，资源仍归 base 管理；http 使用同样的自定义删除器写法。局部变量按构造的反序销毁：先释放 HTTP 服务，再释放它依赖的 base。
    std::unique_ptr<evhttp, decltype(&evhttp_free)> http(evhttp_new(base.get()), evhttp_free);
    if (!http) throw std::runtime_error("Service::run(): evhttp_new() failed");

    // TODO 实现分块上传，并在接收层设置独立的单次请求正文上限。

    // 使用配置中的监听地址和端口，Config 已校验端口范围。
    if (0 != evhttp_bind_socket(http.get(), m_serverIp.data(), static_cast<ev_uint16_t>(m_serverPort))) throw std::runtime_error("Service::run(): evhttp_bind_socket() failed for " + m_serverIp + ":" + std::to_string(m_serverPort));

    DLOG_LOGGER_INFO(g_logger) << "Listening on " << m_serverIp << ":" << m_serverPort;

    // evhttp_set_gencb() 注册通用请求回调（generic callback），由 libevent 收到请求后调用 GenericCallback；this 作为上下文传给 arg 参数。
    evhttp_set_gencb(http.get(), GenericCallback, this);

    // 信号回调退出循环；随后由智能指针释放 HTTP 服务（关闭监听）和事件循环。
    auto stop = [](evutil_socket_t, short, void *arg)
    { event_base_loopbreak(static_cast<event_base *>(arg)); };

    std::unique_ptr<event, decltype(&event_free)> interrupt(evsignal_new(base.get(), SIGINT, stop, base.get()), event_free);
    std::unique_ptr<event, decltype(&event_free)> terminate(evsignal_new(base.get(), SIGTERM, stop, base.get()), event_free);
    if (!interrupt || !terminate) throw std::runtime_error("Service::run(): evsignal_new() failed");

    if (0 != event_add(interrupt.get(), nullptr) || 0 != event_add(terminate.get(), nullptr)) throw std::runtime_error("Service::run(): registering shutdown signals failed");

    // 阻塞等待事件并执行回调；返回 -1 时抛出异常，0/1 分别表示正常退出或没有待处理事件。
    if (-1 == event_base_dispatch(base.get())) throw std::runtime_error("Service::run(): event_base_dispatch() failed");
}

void Service::GenericCallback(evhttp_request *req, void *arg) noexcept
{
    try
    {
        // evhttp_request_get_evhttp_uri() 取出已解析的 URI；path 不含查询参数。先判空，再构造字符串，具体见 docs/libevent-request-uri.md。
        auto uri = evhttp_request_get_evhttp_uri(req);
        const char *rawPath = uri ? evhttp_uri_get_path(uri) : nullptr;
        if (!rawPath)
        {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Invalid request URI", nullptr);
            return;
        }

        // 只将路径解码的参数异常归为 400；业务处理抛出的异常由外层统一返回 500。
        std::string requestPath;
        try
        {
            requestPath = utils::urlDecode(rawPath);
        }
        catch (const std::invalid_argument &)
        {
            evhttp_send_reply(req, HTTP_BADREQUEST, "Invalid URL encoding", nullptr);
            return;
        }

        DLOG_LOGGER_INFO(g_logger) << "Request path=" << utils::urlEncode(requestPath);

        // run() 注册回调时传入 this 指针。
        Service *service = static_cast<Service *>(arg);

        // 下载请求：下载前缀必须位于路径开头，避免把路径中间包含前缀的未知路由分发到下载处理。例如：
        // GET /download/a.txt：进入下载处理。
        // GET /other/download/a.txt：作为未知路由返回 404。
        // POST /other/download/a.txt：作为未知路由返回 404。
        if (0 == requestPath.find(service->m_downloadPrefix))
        {
            Download(req, arg);
        }
        else if ("/api/files" == requestPath)
        {
            ListFiles(req, arg);
        }
        // 上传请求。
        else if ("/upload" == requestPath)
        {
            Upload(req, arg);
        }
        // 404。
        else
        {
            evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", nullptr);
        }
    }
    catch (const std::exception &error)
    {
        DLOG_LOGGER_ERROR(g_logger) << "Request failed: " << utils::urlEncode(error.what());

        evhttp_send_reply(req, HTTP_INTERNAL, "Internal Server Error", nullptr);
    }
}

void Service::Upload(evhttp_request *req, void *arg)
{
    // 整体流程：校验上传参数 → 保存数据文件 → 生成元数据 → 登记内存表并保存元数据文件 → 返回成功。
    // 数据文件保存实际内容。元数据文件在哪里、占多少空间、何时修改以及如何下载。
    // 1. 检查 HTTP 方法，上传只接受 POST。
    // 2. 读取 FileName、StorageType 请求头，检查缺失或空值，并将存储策略解析为枚举。
    // 3. 将 FileName 从 Base64 解码，校验单个文件名、UTF-8 和长度。
    // 4. 根据存储策略选择配置目录，与文件名组合成最终数据文件路径。
    // 5. 检查下载 URL 和磁盘路径是否已被占用，发生冲突返回 409。
    // 6. 检查完整原始文件大小，超限返回 413；通过后复制到 content，保留二进制内容中的 '\0'，允许零字节文件。
    // 7. 确保存储目录存在，将原始内容或按 bundle_format 压缩后的内容写入磁盘数据文件。
    // 8. 数据文件写入成功后，再完成以下流程：
    //    a. 根据实际文件生成 FileMetadata，包含绝对路径、修改时间、实际存储大小和下载 URL。
    //    b. 准备含下载 URL 的 HTTP 响应 JSON，此时仅生成字符串，尚未发送给客户端。
    //    c. 调用 DataManager::insert()，把这条记录加入内存表，再将整张表保存到所有文件共用的磁盘元数据 JSON 文件。
    // 9. 数据文件和元数据都保存成功后，向客户端返回 201 和前面准备的响应 JSON。
    // 元数据持久化失败时，DataManager 撤销内存表中的新增记录，并保留原磁盘元数据 JSON。文件写入或元数据保存失败后，Upload 尝试删除本次已开始写入的数据文件，并返回 500。

    // 1. 检查 HTTP 方法，上传只接受 POST。其他方法返回 405，Allow 头告诉客户端这里接受 POST。
    if (EVHTTP_REQ_POST != evhttp_request_get_command(req))
    {
        DLOG_LOGGER_WARN(g_logger) << "Upload rejected: method not allowed";

        evhttp_add_header(evhttp_request_get_output_headers(req), "Allow", "POST");
        evhttp_send_reply(req, HTTP_BADMETHOD, "Method Not Allowed", nullptr);
        return;
    }

    Service *service = static_cast<Service *>(arg);
    std::filesystem::path filePath;
    bool fileWriteStarted = false;

    try
    {
        // 2. 读取 FileName、StorageType 请求头。先用原始指针接收 C 接口的返回值，检查字段是否存在。
        auto inputHeaders = evhttp_request_get_input_headers(req);
        const char *fileNameHeader = evhttp_find_header(inputHeaders, "FileName");
        const char *storageTypeHeader = evhttp_find_header(inputHeaders, "StorageType");
        // nullptr 表示请求头中没有这个字段，必须在读取字段值前检查。
        if (!fileNameHeader || !storageTypeHeader)
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: missing headers";

            evhttp_send_reply(req, HTTP_BADREQUEST, "Missing FileName or StorageType", nullptr);
            return;
        }

        std::string encodedFileName = fileNameHeader;
        // 字段存在也可能指向 ""，所以 FileName 还需检查空值；StorageType 的值由解析函数统一校验。
        if (encodedFileName.empty())
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: empty filename";

            evhttp_send_reply(req, HTTP_BADREQUEST, "Empty FileName", nullptr);
            return;
        }

        auto storageType = getStorageTypeFromText(storageTypeHeader);
        if (!storageType)
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: invalid storage type";

            evhttp_send_reply(req, HTTP_BADREQUEST, "Illegal storage type", nullptr);
            return;
        }

        // 3. 解码 UTF-8 文件名字节，例如 YS50eHQ= → a.txt；解码或文件名校验失败时返回 400。
        // 沿用 cpp-base64 的解码规则，允许库能接受的非标准写法；解码后的文件名仍须通过下方校验。
        std::string fileName;
        try
        {
            fileName = base64_decode(encodedFileName);
        }
        catch (...)
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: invalid filename encoding";

            evhttp_send_reply(req, HTTP_BADREQUEST, "Invalid Base64 fileName", nullptr);
            return;
        }

        // 校验 fileName 后才构造路径 path。
        if (!isValidFileName(fileName))
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: invalid filename";

            evhttp_send_reply(req, HTTP_BADREQUEST, "Invalid file name", nullptr);
            return;
        }

        // 4. 请求头已解析为合法枚举，按枚举选择配置目录。
        std::filesystem::path directory;

        switch (storageType.value())
        {
            case StorageType::Uncompressed:
                directory = Config::GetInstance().getUncompressedFilesDirPath();
                break;
            case StorageType::Compressed:
                directory = Config::GetInstance().getCompressedFilesDirPath();
                break;
        }

        filePath = directory / fileName;

        // 5. 先查内存中是否已经有对应 URL 的元数据，再查磁盘上是否已有文件；任意一处占用都返回 409。
        // 两种策略共用下载前缀和文件名，因此跨策略的同名文件也会冲突。
        try
        {
            // getFileMetaDataByRequestPath() 函数未找到抛出异常，在 catch 分支被捕获然后正常执行后续，这正是我们想要的。如果找到，走 try 的后续分支返回 409。
            service->m_dataManager.getFileMetaDataByRequestPath(service->m_downloadPrefix + fileName);

            DLOG_LOGGER_WARN(g_logger) << "Upload conflict file=" << utils::fileNameEncode(fileName);

            evhttp_send_reply(req, 409, "File already exists", nullptr);
            return;
        }
        catch (const std::out_of_range &)
        {
            // 未找到记录表示 URL 尚未使用，可以继续上传。
        }

        // symlink_status() 检查路径自身，连指向不存在目标的符号链接也视为已占用。
        if (std::filesystem::exists(std::filesystem::symlink_status(filePath)))
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload conflict file=" << utils::fileNameEncode(fileName);

            evhttp_send_reply(req, 409, "File already exists", nullptr);
            return;
        }

        // 6. 通过前面的检查后，再分配内存并复制正文，保留二进制内容中的 '\0'。
        // 取得请求正文缓冲区。
        auto inputBuf = evhttp_request_get_input_buffer(req);
        // 取得请求正文的字节数。
        size_t length = evbuffer_get_length(inputBuf);

        // 当前版本所有正文已完整缓冲，分块上传及单次请求上限接入后再限制接收内存。
        if (length > kMaxFileSize)
        {
            DLOG_LOGGER_WARN(g_logger) << "Upload rejected: file too large bytes=" << length;

            evhttp_send_reply(req, HTTP_ENTITYTOOLARGE, "File too large", nullptr);
            return;
        }

        // 准备接收内容的 content。
        std::string content(length, '\0');
        // 空 length 跳过复制，之后仍会创建零字节文件。
        if (length)
        {
            // evbuffer_copyout() 复制但不移除输入字节。
            if (static_cast<ev_ssize_t>(length) != evbuffer_copyout(inputBuf, content.data(), length)) throw std::runtime_error("Service::Upload(): evbuffer_copyout() failed");
        }

        // 7. 创建目录。压缩策略先处理内容，随后两种策略共用 setContent() 写入。

        // createDirectory() 使用 std::filesystem::create_directories()。目录不存在：创建目录，包括缺失的父目录。目录已存在：直接继续，不会清空目录或重新创建。
        utils::FileUtil(directory).createDirectory();

        // bundle 可能对不适合压缩的内容保留原字节，因此 compressed 不保证文件更小。
        if (StorageType::Compressed == storageType.value()) content = utils::FileUtil::Compress(content, Config::GetInstance().getBundleFormat());

        // 写入前设置标记，保证写到一半失败时也能清理残留文件。
        fileWriteStarted = true;

        utils::FileUtil(filePath).setContent(content);

        // 8. 从实际磁盘文件生成元数据，记录存储路径、修改时间和实际存储大小，然后添加到内存表中。
        // 这里有两份用途不同的 JSON：body 是给客户端的响应，m_dataManager 持久化的 JSON 是服务端的文件元数据。

        // 从磁盘文件生成元数据。
        auto info = FileMetadata::FromFile(filePath);

        // 先准备成功响应的 JSON，避免序列化失败时已经提交元数据；对外下载 URL 进行百分号编码。
        Json::Value result;
        result["url"] = utils::urlEncode(info.m_url);
        auto body = utils::JsonUtil::Serialize(result);

        // insert() 新增内存记录并持久化元数据 JSON 文件。保存失败时撤销新增，再抛异常进入下面的 catch，这一步是由 insert() 保证的。
        service->m_dataManager.insert(info);
        fileWriteStarted = false;

        DLOG_LOGGER_INFO(g_logger) << "Upload completed file=" << utils::fileNameEncode(fileName)
                                   << " storage=" << storageTypeHeader << " bytes=" << length
                                   << " stored_bytes=" << info.m_fileSize;

        // 9. 数据文件和元数据文件全部保存成功后，用请求自带的输出缓冲区返回 201 和 JSON。
        auto outputBuf = evhttp_request_get_output_buffer(req);
        evbuffer_add(outputBuf, body.data(), body.size());
        evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json;charset=utf-8");
        evhttp_send_reply(req, 201, "Created", nullptr);
    }
    catch (const std::exception &exception)
    {
        // 只清理本次已经开始写入的文件；error_code 重载避免清理失败再次抛异常。
        if (fileWriteStarted)
        {
            std::error_code error;
            std::filesystem::remove(filePath, error);

            if (error) DLOG_LOGGER_ERROR(g_logger) << "Upload cleanup failed path=" << utils::urlEncode(filePath.string()) << " error=" << utils::urlEncode(error.message());
        }

        DLOG_LOGGER_ERROR(g_logger) << "Upload failed: " << utils::urlEncode(exception.what());

        evhttp_send_reply(req, HTTP_INTERNAL, "Upload failed", nullptr);
    }
}

void Service::ListFiles(evhttp_request *req, void *arg)
{
    // 整体流程：检查 HTTP 方法 → 读取文件元数据快照 → 校验并组装列表 → 返回 JSON。
    // 1. 文件列表接口只接受 GET，其他方法返回 405 并附带 Allow 头。
    // 2. 从 DataManager 复制当前的元数据记录，空列表也返回 files 数组。
    // 3. 逐条校验存储目录、文件名和下载路径，生成供前端展示和下载的字段。
    // 4. 将列表序列化为 JSON 并返回 200，处理过程中抛出的异常交给 GenericCallback() 返回 500。

    // 1. 检查 HTTP 方法，Allow 头告诉客户端这里接受 GET。
    auto headers = evhttp_request_get_output_headers(req);

    if (EVHTTP_REQ_GET != evhttp_request_get_command(req))
    {
        evhttp_add_header(headers, "Allow", "GET");
        evhttp_send_reply(req, HTTP_BADMETHOD, "Method Not Allowed", nullptr);
        return;
    }

    Service *service = static_cast<Service *>(arg);
    auto &prefix = service->m_downloadPrefix;

    // 2. 显式初始化 files 数组，确保存储为空时也返回 {"files":[]}。
    Json::Value result;
    auto &files = result["files"] = Json::Value(Json::arrayValue);

    // getAll() 在读锁下复制元数据，后续校验和 JSON 组装使用这份快照。
    for (auto &file : service->m_dataManager.getAll())
    {
        // 3. 根据文件所在的配置目录判断存储策略，再校验用于展示的文件名。
        auto storageType = getStorageTypeFromFilePath(file.m_absoluteFilePath);
        if (!storageType) throw std::runtime_error("ListFiles(): unknown storage directory");

        auto name = file.m_absoluteFilePath.filename().string();
        if (!isValidFileName(name)) throw std::runtime_error("ListFiles(): invalid file name");

        // 下载路径须以配置前缀开头，后面只允许一个合法文件名，同时排除双斜杠开头和点目录段。
        auto &url = file.m_url;
        if (0 != url.compare(0, prefix.size(), prefix) ||
            0 == url.compare(0, 2, "//") ||
            !isValidFileName(std::string_view(url).substr(prefix.size())) ||
            std::string::npos != url.find("/./") ||
            std::string::npos != url.find("/../"))
        {
            throw std::runtime_error("ListFiles(): invalid download URL");
        }

        // 返回磁盘上的实际存储大小，并对外提供经过百分号编码的下载路径。
        Json::Value item;
        item["name"] = name;
        item["storage_type"] = StorageType::Compressed == storageType.value() ? "compressed" : "uncompressed";
        item["stored_size_bytes"] = static_cast<Json::UInt64>(file.m_fileSize);
        item["modified_time"] = FormatFileTime(file.m_mtime);
        item["download_url"] = utils::urlEncode(url);

        files.append(std::move(item));
    }

    // 4. 准备 JSON 正文和响应头，全部成功后再发送 200。
    auto body = utils::JsonUtil::Serialize(result);
    std::unique_ptr<evbuffer, decltype(&evbuffer_free)> buffer(evbuffer_new(), evbuffer_free);

    if (!buffer ||
        0 != evbuffer_add(buffer.get(), body.data(), body.size()) ||
        0 != evhttp_add_header(headers, "Content-Type", "application/json; charset=utf-8"))
    {
        throw std::runtime_error("ListFiles(): could not prepare JSON response");
    }

    DLOG_LOGGER_INFO(g_logger) << "File list count=" << files.size();

    evhttp_send_reply(req, HTTP_OK, nullptr, buffer.get());
}

void Service::Download(evhttp_request *req, void *arg)
{
    // 整体流程：解析下载 URL → 查询元数据 → 判断版本和区间 → 读取或解压 → 返回原文字节。
    // 上传负责写入数据文件和登记元数据；下载根据已有元数据找到文件，读取后直接响应，不新增元数据记录。
    // 1. 检查 HTTP 方法，下载只接受 GET。
    // 2. 提取 URI 路径并进行一次 URL 解码，得到元数据表中的请求路径键。
    // 3. 按已解码的请求路径查询 FileMetadata，取得实际存储路径；记录不存在时返回 404。
    // 4. 根据实际存储目录判断 StorageType，目录无法识别时返回 500；数据文件丢失时返回 404。
    // 5. 取得原文长度，校验 If-Range 并解析 Range；有效但不可满足的区间返回 416。
    // 6. 普通文件只读取需要的区间；压缩文件在内存中解压后选择区间，设置响应头。
    // 7. 有效区间返回 206 和实际片段，其余返回完整 200；读取或解压抛出异常时返回 500。
    // 下载过程只读取数据文件，不创建解压临时文件，也不修改内存元数据表或磁盘元数据 JSON。

    // 1. 其他方法返回 405，Allow 头说明当前接受 GET。
    if (EVHTTP_REQ_GET != evhttp_request_get_command(req))
    {
        evhttp_add_header(evhttp_request_get_output_headers(req), "Allow", "GET");
        evhttp_send_reply(req, HTTP_BADMETHOD, "Method Not Allowed", nullptr);
        return;
    }

    Service *service = static_cast<Service *>(arg);

    try
    {
        // 2. 从请求的原始编码路径解码得到 requestPath，查询参数不参与查找；加号仍表示加号。
        // GenericCallback 的解码结果是其局部变量，这里重新读取原始 URI，避免对已解码的结果再次解码。
        auto requestPath = utils::urlDecode(evhttp_uri_get_path(evhttp_request_get_evhttp_uri(req)));

        // 3. 元数据提供实际文件位置，不能直接把客户端请求路径当成磁盘路径。
        FileMetadata info;
        try
        {
            info = service->m_dataManager.getFileMetaDataByRequestPath(requestPath);
        }
        catch (const std::out_of_range &)
        {
            DLOG_LOGGER_WARN(g_logger) << "Download not found path=" << utils::urlEncode(requestPath);

            evhttp_send_reply(req, HTTP_NOTFOUND, "File not found", nullptr);
            return;
        }

        // 4. 按照目录判断存储类型，同时判断数据文件是否存在。
        auto filePath = info.m_absoluteFilePath;
        auto storageType = getStorageTypeFromFilePath(filePath);
        if (!storageType)
        {
            // 两个存储目录都未匹配，无法确定存储类型，返回内部错误并结束处理。但在正常的运作流程中根本走不到这个分支，只做防御性编程。
            DLOG_LOGGER_ERROR(g_logger) << "Download has unknown storage path=" << utils::urlEncode(filePath.string());

            evhttp_send_reply(req, HTTP_INTERNAL, "Unknown storage directory", nullptr);
            return;
        }

        utils::FileUtil file(filePath);
        if (!file.exists())
        {
            DLOG_LOGGER_WARN(g_logger) << "Download file missing path=" << utils::urlEncode(requestPath);

            evhttp_send_reply(req, HTTP_NOTFOUND, "File not found", nullptr);
            return;
        }

        // 5. 获得响应正文 content。请求头中，Range 表示请求区间，If-Range 表示校验版本。具体说明见 docs/etag.md。
        std::string etag = GetETag(info);

        auto inputHeaders = evhttp_request_get_input_headers(req);
        const char *rangeHeader = evhttp_find_header(inputHeaders, "Range");
        const char *ifRangeHeader = evhttp_find_header(inputHeaders, "If-Range");

        // useRange 代表本次请求是否使用 Range 指定的区间进行下载。
        // 1. 没有 Range：false，完整下载，返回值 200 HTTP_OK。
        // 2. 有 Range，同时，没有 If-Range 或 ETag 匹配：true，解析区间。
        // 3. 有 Range，有 If-Range 但 ETag 校验不匹配：false，完整下载，返回值 200 HTTP_OK。
        bool useRange = rangeHeader && (!ifRangeHeader || ifRangeHeader == etag);

        // TODO 下载发送优化。评估 evbuffer_add_file() 文件/区间发送；压缩文件可参考独立解压临时文件中转，明确描述符所有权及临时文件清理时机。

        bool compressed = StorageType::Compressed == storageType.value();

        std::string content;
        if (compressed) content = utils::FileUtil::UnCompress(file.getContent());

        size_t total = compressed ? content.size() : file.fileSize(), offset = 0, length = total;

        int status = useRange ? parseRangeHeader(rangeHeader, total, offset, length) : HTTP_OK;
        // 416 表示请求的范围无法满足。
        if (416 == status)
        {
            DLOG_LOGGER_WARN(g_logger) << "Download range not satisfiable path=" << utils::urlEncode(requestPath) << " bytes=" << total;

            auto contentRange = "bytes */" + std::to_string(total);
            evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Range", contentRange.data());
            evhttp_send_reply(req, 416, "Range Not Satisfiable", nullptr);
            return;
        }

        if (!compressed) content = file.getPosLen(offset, length);

        // 6. 设置二进制响应类型、下载文件名及 ETag。
        // Content-Disposition 用于明确浏览器的下载行为和保存文件名，下载主流程本身不依赖此响应头。attachment 建议按附件下载；filename* 指定建议文件名，避免浏览器仅根据请求路径推断名称。UTF-8'' 声明文件名使用 UTF-8，语言标记留空；百分号编码支持中文、空格等字符。例如文件名 "a b.txt" 会生成 "attachment; filename*=UTF-8''a%20b.txt"。
        auto disposition = "attachment; filename*=UTF-8''" + utils::fileNameEncode(filePath.filename().string());
        auto headers = evhttp_request_get_output_headers(req);

        // 使用 ETag 提供文件版本标识。
        evhttp_add_header(headers, "ETag", etag.data());
        // 指定附件下载方式和建议文件名。
        evhttp_add_header(headers, "Content-Disposition", disposition.data());
        // 声明正文是通用二进制数据。
        evhttp_add_header(headers, "Content-Type", "application/octet-stream");
        evhttp_add_header(headers, "Accept-Ranges", "bytes");
        // 206 表示返回部分内容。Content-Range 描述实际响应区间与原文总大小，仅部分响应需要这个字段。
        if (206 == status)
        {
            auto contentRange = "bytes " + std::to_string(offset) + "-" + std::to_string(offset + length - 1) + "/" + std::to_string(total);
            evhttp_add_header(headers, "Content-Range", contentRange.data());
        }

        // 7. 按长度发送二进制正文，保留 '\0'；libevent 会根据实际正文生成 Content-Length 响应字段。
        auto outputBuf = evhttp_request_get_output_buffer(req);
        evbuffer_add(outputBuf, content.data() + (compressed ? offset : 0), length);

        DLOG_LOGGER_INFO(g_logger) << "Download response prepared file=" << utils::fileNameEncode(filePath.filename().string())
                                   << " storage=" << (compressed ? "compressed" : "uncompressed")
                                   << " status=" << status << " bytes=" << length;

        evhttp_send_reply(req, status, status == 206 ? "Partial Content" : "OK", nullptr);
    }
    catch (const std::exception &error)
    {
        DLOG_LOGGER_ERROR(g_logger) << "Download failed: " << utils::urlEncode(error.what());

        evhttp_send_reply(req, HTTP_INTERNAL, "Download failed", nullptr);
    }
}

std::string Service::FormatFileTime(std::filesystem::file_time_type t)
{
    // 主要逻辑参考：https://github.com/DavidingPlus/dlog/blob/master/src/utils/timestamp.cpp 的 Timestamp::toFormattedString() 函数。

    // t 是文件修改时间点，使用文件系统时钟表示；转换时保留它所代表的修改时刻。fileNow 使用文件系统时钟 std::filesystem::file_time_type::clock，与 t 属于同一种时钟，可以相减，得到“修改时刻距离现在”的时间间隔。
    auto fileNow = std::filesystem::file_time_type::clock::now();
    // system_clock 是系统时钟类型，now() 返回系统时钟下的当前时间点 systemNow。它与文件系统时钟的计时起点可能不同，不能直接把文件时间的计数当作系统时间使用。
    auto systemNow = std::chrono::system_clock::now();
    // t - fileNow 是时间间隔，例如文件在 10 分钟前修改，结果就是 -10 分钟。systemNow 加上该间隔，得到系统时钟下对应的文件修改时间点 systemTime。
    auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(systemNow + (t - fileNow));

    auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(systemTime);
    // 对负时间戳，time_point_cast 会向 0 方向截断。例如 tp = -0.2s 时，截断后的 seconds 会先变成 0s，导致后面的子秒部分变成 -0.2s。这里把整秒部分回退到 -1s，最终就能拆成 (-1s) + 0.8s，保证纳秒部分始终落在 [0, 1s)。
    if (seconds > systemTime) seconds -= std::chrono::seconds(1);

    auto time = std::chrono::system_clock::to_time_t(seconds);

    // 不使用 std::localtime，因为它返回指向共享静态缓冲区的指针：多个线程同时调用时，某个线程可能覆盖另一个线程刚刚得到的日期时间。
    // 这里每次调用都使用自己的局部 tm 对象，再通过 localtime_s/localtime_r 把转换结果写入这个对象。
    std::tm localTime{};
    if (!::localtime_r(&time, &localTime)) throw std::runtime_error("FormatFileTime(): localtime_r() failed");

    // stream 收集格式化后的字，std::put_time() 按指定格式输出日历字段，str() 返回最终字符串。
    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    if (stream.fail()) throw std::runtime_error("FormatFileTime(): file time formatting failed");


    return stream.str();
}

std::string Service::GetETag(const FileMetadata &info)
{
    // ETag 是服务端为文件生成的版本标识，客户端和服务端通过它判断是否需要重新传输文件。
    // 1. 首次下载时，服务端返回文件内容和 ETag；客户端可以将正文与 ETag 一起缓存。
    // 2. 本地副本可用、再次需要验证版本时，客户端通过请求头把保存的 ETag 带回服务端。
    // 3. 服务端比较新旧 ETag：相同则返回“未修改”（304），不再发送正文，客户端复用本地缓存。
    // 4. 如果 ETag 不同，服务端返回新文件和新 ETag，客户端更新保存的内容和标签。
    // 5. 断点续传时，也用 ETag 确认版本一致后再传输剩余部分；版本改变则重新下载，避免拼接新旧内容。

    // 文件名中的特殊字节采用百分号编码，保证标签可以直接用于响应头。
    // 例如文件名是 "a b.txt"，原样拼接：ETag: "a b.txt-1024-123"，编码以后：ETag: "a%20b.txt-1024-123"。
    // HTTP 规范要求 ETag 的标签部分由双引号包裹，所以返回的字符串首尾加上 ""。
    std::string etag = "\"";

    etag += utils::fileNameEncode(info.m_absoluteFilePath.filename().string());
    etag += "-";
    etag += std::to_string(info.m_fileSize);
    etag += "-";
    etag += std::to_string(info.m_mtime.time_since_epoch().count());
    etag += '"';


    return etag;
}


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_END
