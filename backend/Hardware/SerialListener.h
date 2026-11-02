#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bvs {

// Two decoupled threads:
//   reader : owns the serial port, assembles '\n' lines in a fixed 64-byte buffer, auto-reconnects.
//            It never calls business logic, so a slow database can never stall the USB link.
//   worker : pops lines from a bounded FIFO, calls the handler, writes the reply back.
class SerialListener {
public:
    // Handler gets one complete line (no newline) and returns a reply line ("" = no reply).
    using LineHandler = std::function<std::string(const std::string&)>;

    SerialListener(std::string port, unsigned baud, LineHandler handler);
    ~SerialListener();

    void start();
    void stop();
    bool connected() const { return connected_.load(); }
    bool sendLine(const std::string& s) { return writeLine(s); }   // thread-safe (portMu_); used by enrolment
    uint64_t linesReceived() const { return lines_.load(); }
    uint64_t linesDropped() const { return dropped_.load(); }

private:
    void readerLoop();
    void workerLoop();
    bool openPort();
    void closePort();
    bool writeLine(const std::string& s);
    void enqueue(std::string line);
    void sleepInterruptible(int ms);

    const std::string portName_;
    const unsigned baud_;
    LineHandler handler_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> lines_{0};
    std::atomic<uint64_t> dropped_{0};

    std::mutex portMu_;          // guards handle_ for open/close/write
    std::intptr_t handle_ = -1;  // fd (POSIX) or HANDLE (Windows)

    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<std::string> queue_;
    static constexpr size_t kMaxQueue = 256;

    std::thread reader_;
    std::thread worker_;
};

}  // namespace bvs
