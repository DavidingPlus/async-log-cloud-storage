#ifndef _ASYNC_LOG_CLOUD_STORAGE_SERVER_LOG_H_
#define _ASYNC_LOG_CLOUD_STORAGE_SERVER_LOG_H_

#include "globalmacros.h"

#include <dlog/core/log.h>


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_BEGIN(server)


// 1. C++17 inline 变量允许头文件被多个编译单元包含，最终共享同一个日志器。
// 2. 也可在此用 extern 声明，并在一个 .cpp 文件中定义，效果相同。
// 3. 若在头文件直接定义而不加 inline，多处包含会导致链接时重复定义。
// 4. 若在头文件使用 static，每个编译单元各有一个变量，无法共享日志器。如果在 cpp 里写 static，变量只能在该文件内访问，其他业务文件就不能直接使用。
inline dlog::Logger::SharedPtr g_logger;


ASYNC_LOG_CLOUD_STORAGE_NAMESPACE_END


#endif
