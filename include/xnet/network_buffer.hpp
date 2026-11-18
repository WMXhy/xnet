#pragma once

#include <cstddef>
#include <sys/types.h>
#include <vector>

namespace xnet {

class NetworkBuffer {
public:
    explicit NetworkBuffer(std::size_t size = 1024);

    NetworkBuffer(const NetworkBuffer&) = default;
    NetworkBuffer& operator=(const NetworkBuffer&) = default;
    NetworkBuffer(NetworkBuffer&& other) noexcept;
    NetworkBuffer& operator=(NetworkBuffer&& other) noexcept;

    // The source range must not overlap the buffer.
    void append(const char* data, std::size_t len);
    ssize_t read_from_fd(int fd);
    ssize_t write_to_fd(int fd);

    // The view can be invalidated by a subsequent mutation.
    const char* peek() const noexcept;
    std::size_t readable() const noexcept;
    void retrieve(std::size_t len);

private:
    void ensureWritable(std::size_t len);

    std::vector<char> buffer_;
    std::size_t read_index_ = 0;
    std::size_t write_index_ = 0;
};

} // namespace xnet
