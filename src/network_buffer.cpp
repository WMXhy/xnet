#include <xnet/network_buffer.hpp>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <sys/socket.h>

namespace xnet {

NetworkBuffer::NetworkBuffer(std::size_t size) : buffer_(size) {}

NetworkBuffer::NetworkBuffer(NetworkBuffer&& other) noexcept
    : buffer_(std::move(other.buffer_)),
      read_index_(std::exchange(other.read_index_, 0)),
      write_index_(std::exchange(other.write_index_, 0)) {}

NetworkBuffer& NetworkBuffer::operator=(NetworkBuffer&& other) noexcept {
    if (this != &other) {
        buffer_ = std::move(other.buffer_);
        read_index_ = std::exchange(other.read_index_, 0);
        write_index_ = std::exchange(other.write_index_, 0);
    }
    return *this;
}

const char* NetworkBuffer::peek() const noexcept {
    return read_index_ == 0 ? buffer_.data() : buffer_.data() + read_index_;
}

std::size_t NetworkBuffer::readable() const noexcept {
    return write_index_ - read_index_;
}

void NetworkBuffer::ensureWritable(std::size_t len) {
    const auto free = buffer_.size() - write_index_;
    if (free >= len) return;

    if (read_index_ + free >= len) {
        const auto size = readable();
        std::memmove(buffer_.data(), peek(), size);
        read_index_ = 0;
        write_index_ = size;
        return;
    }

    if (len > buffer_.max_size() - write_index_) {
        throw std::length_error("xnet network buffer is too large");
    }
    buffer_.resize(write_index_ + len);
}

void NetworkBuffer::append(const char* data, std::size_t len) {
    if (len == 0) return;

    ensureWritable(len);
    std::memcpy(buffer_.data() + write_index_, data, len);
    write_index_ += len;
}

ssize_t NetworkBuffer::read_from_fd(int fd) {
    ensureWritable(65536);
    const auto count = ::recv(fd, buffer_.data() + write_index_, buffer_.size() - write_index_, 0);
    if (count > 0) write_index_ += static_cast<std::size_t>(count);
    return count;
}

void NetworkBuffer::retrieve(std::size_t len) {
    assert(len <= readable());
    read_index_ += len;
    if (read_index_ == write_index_) {
        read_index_ = 0;
        write_index_ = 0;
    }
}

ssize_t NetworkBuffer::write_to_fd(int fd) {
    ssize_t total = 0;
    while (readable() > 0) {
        const auto count = ::send(fd, peek(), readable(), MSG_NOSIGNAL);
        if (count > 0) {
            retrieve(static_cast<std::size_t>(count));
            total += count;
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    return total;
}

} // namespace xnet
