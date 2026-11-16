#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <dlog/core/log.h>
#include <dlog/core/logworker.h>
#include <dlog/utils/threadpool.h>


namespace
{
    namespace fs = std::filesystem;


    std::string uniqueLoggerName()
    {
        static unsigned int run = 0;
        return "dlog-test-" + std::to_string(++run);
    }

    class DlogFileTests : public ::testing::Test
    {

    protected:

        void SetUp() override
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                const auto candidate = fs::temp_directory_path() / ("cloud-storage-dlog-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
                std::error_code error;
                if (fs::create_directory(candidate, error))
                {
                    directory = candidate;
                    return;
                }
                ASSERT_FALSE(error) << error.message();
            }
            FAIL() << "Cannot create an isolated temporary directory";
        }

        void TearDown() override
        {
            if (directory.empty()) return;
            std::error_code error;
            fs::remove_all(directory, error);
            EXPECT_FALSE(error) << error.message();
        }

        static std::string readFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot read test log file");
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }


        fs::path directory;
    };
}


TEST(DlogTests, WritesMessageWithAsyncLogger)
{
    auto logger = dlog::LogManager::GetInstance().buildLogger(uniqueLoggerName());

    testing::internal::CaptureStdout();
    DLOG_LOGGER_INFO(logger) << "dlog integration test " << 42;
    logger->stop();
    const auto output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("dlog integration test 42"), std::string::npos);
}

TEST(DlogTests, WritesDifferentLevelsAndDrainsOnStop)
{
    auto logger = dlog::LogManager::GetInstance().buildLogger(uniqueLoggerName());
    testing::internal::CaptureStdout();
    DLOG_LOGGER_INFO(logger) << "integration-info";
    DLOG_LOGGER_WARN(logger) << "integration-warning";
    DLOG_LOGGER_ERROR(logger) << "integration-error";
    logger->stop();
    const auto output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("[INFO ]"), std::string::npos);
    EXPECT_NE(output.find("[WARN ]"), std::string::npos);
    EXPECT_NE(output.find("[ERROR]"), std::string::npos);
    EXPECT_NE(output.find("integration-info"), std::string::npos);
    EXPECT_NE(output.find("integration-warning"), std::string::npos);
    EXPECT_NE(output.find("integration-error"), std::string::npos);
    EXPECT_THROW(logger->message(__FILE__, __LINE__, dlog::LogLevel::INFO), std::logic_error);
}

TEST(DlogTests, RegistersNamedLoggerAndRejectsDuplicateName)
{
    auto &manager = dlog::LogManager::GetInstance();
    const auto name = uniqueLoggerName();
    auto logger = manager.buildLogger(name);

    EXPECT_EQ(manager.getLogger(name), logger);
    EXPECT_THROW(manager.buildLogger(name), std::logic_error);
    EXPECT_EQ(manager.getLogger(name + "-missing"), nullptr);
    logger->stop();
}

TEST_F(DlogFileTests, FileOutputAppendsAndCreatesParentDirectory)
{
    const auto path = directory / "nested" / "server.log";
    {
        dlog::FileFlush file(path);
        file.write("first\n", 6);
        file.flush();
    }
    {
        dlog::FileFlush file(path);
        file.write("second\n", 7);
        file.flush();
    }
    EXPECT_EQ(readFile(path), "first\nsecond\n");
}

TEST_F(DlogFileTests, AsyncWorkerDrainsToFileOnStop)
{
    const auto path = directory / "async.log";
    {
        auto pool = std::make_shared<dlog::ThreadPool>(1);
        auto output = std::make_shared<dlog::FileFlush>(path);
        dlog::LogWorker worker(output, pool);
        worker.start();
        worker.append("first\n", 6);
        worker.append("last\n", 5);
        worker.stop();
    }
    EXPECT_EQ(readFile(path), "first\nlast\n");
}

TEST_F(DlogFileTests, RollingOutputPreservesBothWrites)
{
    {
        dlog::RollFileFlush file(directory / "server", 5);
        file.write("first", 5);
        file.flush();
        file.write("second", 6);
        file.flush();
    }

    std::string contents;
    size_t files = 0;
    for (const auto &entry : fs::directory_iterator(directory))
    {
        ASSERT_TRUE(entry.is_regular_file());
        contents += readFile(entry.path());
        ++files;
    }
    EXPECT_EQ(files, 2u);
    EXPECT_NE(contents.find("first"), std::string::npos);
    EXPECT_NE(contents.find("second"), std::string::npos);
}
