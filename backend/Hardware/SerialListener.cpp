#include "SerialListener.h"

#include <chrono>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bvs {

SerialListener::SerialListener(std::string port, unsigned baud, LineHandler handler)
    : portName_(std::move(port)), baud_(baud), handler_(std::move(handler)) {}

SerialListener::~SerialListener() { stop(); }

void SerialListener::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&SerialListener::workerLoop, this);
    reader_ = std::thread(&SerialListener::readerLoop, this);
}

void SerialListener::stop() {
    if (!running_.exchange(false)) return;
    qCv_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (worker_.joinable()) worker_.join();
    closePort();
}

void SerialListener::sleepInterruptible(int ms) {
    for (int waited = 0; waited < ms && running_.load(); waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ------------------------------------------------------------------ port I/O
#ifdef _WIN32

bool SerialListener::openPort() {
    std::string path = "\\\\.\\" + portName_;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = baud_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 20;
    to.ReadTotalTimeoutConstant = 100;   // ~100 ms poll so stop() is responsive
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    std::lock_guard<std::mutex> lk(portMu_);
    handle_ = reinterpret_cast<std::intptr_t>(h);
    return true;
}

void SerialListener::closePort() {
    std::lock_guard<std::mutex> lk(portMu_);
    if (handle_ != -1) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
        handle_ = -1;
    }
    connected_ = false;
}

bool SerialListener::writeLine(const std::string& s) {
    std::lock_guard<std::mutex> lk(portMu_);
    if (handle_ == -1) return false;
    std::string out = s + "\n";
    DWORD written = 0;
    return WriteFile(reinterpret_cast<HANDLE>(handle_), out.data(), static_cast<DWORD>(out.size()), &written, nullptr) &&
           written == out.size();
}

static long readSome(std::intptr_t handle, char* buf, size_t cap) {
    DWORD n = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(handle), buf, static_cast<DWORD>(cap), &n, nullptr)) return -1;
    return static_cast<long>(n);
}

#else  // POSIX

static speed_t toSpeed(unsigned baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B9600;
    }
}

bool SerialListener::openPort() {
    int fd = ::open(portName_.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return false;
    termios tio{};
    if (tcgetattr(fd, &tio) != 0) { ::close(fd); return false; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, toSpeed(baud_));
    cfsetospeed(&tio, toSpeed(baud_));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1;  // 100 ms read timeout so stop() is responsive
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { ::close(fd); return false; }
    tcflush(fd, TCIOFLUSH);
    std::lock_guard<std::mutex> lk(portMu_);
    handle_ = fd;
    return true;
}

void SerialListener::closePort() {
    std::lock_guard<std::mutex> lk(portMu_);
    if (handle_ != -1) {
        ::close(static_cast<int>(handle_));
        handle_ = -1;
    }
    connected_ = false;
}

bool SerialListener::writeLine(const std::string& s) {
    std::lock_guard<std::mutex> lk(portMu_);
    if (handle_ == -1) return false;
    const std::string out = s + "\n";
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = ::write(static_cast<int>(handle_), out.data() + off, out.size() - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    tcdrain(static_cast<int>(handle_));
    return true;
}

static long readSome(std::intptr_t handle, char* buf, size_t cap) {
    ssize_t n = ::read(static_cast<int>(handle), buf, cap);
    if (n < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return static_cast<long>(n);
}

#endif

// ------------------------------------------------------------------ threads
void SerialListener::enqueue(std::string line) {
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (queue_.size() >= kMaxQueue) {
            dropped_.fetch_add(1);
            return;
        }
        queue_.push_back(std::move(line));
    }
    lines_.fetch_add(1);
    qCv_.notify_one();
}

void SerialListener::readerLoop() {
    char chunk[64];
    char line[64];  // fixed line buffer: no per-byte heap churn, hard upper bound on input
    size_t len = 0;
    bool overflow = false;

    while (running_.load()) {
        if (!openPort()) {
            sleepInterruptible(2000);
            continue;
        }
        std::cerr << "[serial] opened " << portName_ << "\n";
        sleepInterruptible(2000);  // opening the port resets an Arduino UNO; let it boot
        connected_ = true;
        len = 0;
        overflow = false;

        while (running_.load()) {
            const long n = readSome(handle_, chunk, sizeof chunk);
            if (n < 0) {
                std::cerr << "[serial] read error, reconnecting\n";
                break;
            }
            for (long i = 0; i < n; ++i) {
                const char c = chunk[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    if (!overflow && len > 0) enqueue(std::string(line, len));
                    len = 0;
                    overflow = false;
                } else if (len < sizeof line) {
                    line[len++] = c;
                } else {
                    overflow = true;  // drop the oversized line entirely
                }
            }
        }
        closePort();
    }
}

void SerialListener::workerLoop() {
    while (true) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait(lk, [&] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) {
                if (!running_.load()) return;
                continue;
            }
            line = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            const std::string reply = handler_(line);
            if (!reply.empty() && !writeLine(reply)) std::cerr << "[serial] write failed\n";
        } catch (const std::exception& e) {
            std::cerr << "[serial] handler error: " << e.what() << "\n";
            writeLine("DENY");  // fail closed
        }
    }
}

}  // namespace bvs
