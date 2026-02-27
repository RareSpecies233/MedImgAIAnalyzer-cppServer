#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

class RuntimeLogger {
public:
    static RuntimeLogger &instance()
    {
        static RuntimeLogger logger;
        return logger;
    }

    void init(const std::filesystem::path &db_dir, bool persist_to_file)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        save_to_file_ = persist_to_file;
        log_file_path_.clear();
        if (log_file_.is_open()) {
            log_file_.close();
        }

        if (save_to_file_) {
            std::error_code ec;
            std::filesystem::create_directories(db_dir, ec);
            if (ec) {
                save_to_file_ = false;
                write_line_unlocked("ERROR", "创建日志目录失败: " + db_dir.string() + ", error=" + ec.message());
                return;
            }

            const std::string stamp = filename_stamp_local();
            log_file_path_ = (db_dir / (stamp + "log.txt")).string();
            log_file_.open(log_file_path_, std::ios::out | std::ios::app | std::ios::binary);
            if (!log_file_) {
                save_to_file_ = false;
                write_line_unlocked("ERROR", "打开日志文件失败: " + log_file_path_);
                return;
            }
            write_line_unlocked("INFO", "日志文件已启用: " + log_file_path_);
        } else {
            write_line_unlocked("INFO", "日志文件已禁用（--nolog）");
        }
    }

    uint64_t next_request_id()
    {
        return ++req_counter_;
    }

    std::string log_file_path() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return log_file_path_;
    }

    static void debug(const std::string &msg) { instance().write_line("DEBUG", msg); }
    static void info(const std::string &msg) { instance().write_line("INFO", msg); }
    static void warn(const std::string &msg) { instance().write_line("WARN", msg); }
    static void error(const std::string &msg) { instance().write_line("ERROR", msg); }

    static std::string preview(const std::string &text, std::size_t max_len = 240)
    {
        std::string out = text;
        std::replace(out.begin(), out.end(), '\n', ' ');
        std::replace(out.begin(), out.end(), '\r', ' ');
        std::replace(out.begin(), out.end(), '\t', ' ');
        if (out.size() <= max_len) {
            return out;
        }
        return out.substr(0, max_len) + "...(truncated)";
    }

private:
    RuntimeLogger() = default;

    static std::string now_stamp_local()
    {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto tt = system_clock::to_time_t(now);
        const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }

    static std::string filename_stamp_local()
    {
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return std::string(buf);
    }

    void write_line(const char *level, const std::string &msg)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        write_line_unlocked(level, msg);
    }

    void write_line_unlocked(const char *level, const std::string &msg)
    {
        std::ostringstream oss;
        oss << '[' << now_stamp_local() << "]"
            << '[' << level << "] "
            << msg;
        const std::string line = oss.str();

        if (std::string(level) == "ERROR") {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }

        if (save_to_file_ && log_file_) {
            log_file_ << line << '\n';
            log_file_.flush();
        }
    }

    mutable std::mutex mtx_;
    bool save_to_file_ = true;
    std::ofstream log_file_;
    std::string log_file_path_;
    std::atomic<uint64_t> req_counter_{0};
};
