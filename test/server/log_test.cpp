#include <gtest/gtest.h>

#include "server/log.h"


TEST(ServerLogTests, StoresRegisteredLogger)
{
    static unsigned int run = 0;
    auto &manager = dlog::LogManager::GetInstance();
    const auto name = "server-log-test-" + std::to_string(++run);

    server::g_logger = manager.buildLogger(name);
    EXPECT_EQ(manager.getLogger(name), server::g_logger);

    server::g_logger->stop();
    server::g_logger.reset();
}
