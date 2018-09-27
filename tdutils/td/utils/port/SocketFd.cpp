//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/utils/port/SocketFd.h"

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/PollFlags.h"

#if TD_PORT_WINDOWS
#include "td/utils/buffer.h"
#include "td/utils/port/detail/Iocp.h"
#include "td/utils/SpinLock.h"
#include "td/utils/VectorQueue.h"
#endif

#if TD_PORT_POSIX
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cstring>

namespace td {
namespace detail {
#if TD_PORT_WINDOWS
class SocketFdImpl : private Iocp::Callback {
 public:
  explicit SocketFdImpl(NativeFd native_fd) : info(std::move(native_fd)) {
    VLOG(fd) << get_native_fd() << " create from native_fd";
    get_poll_info().add_flags(PollFlags::Write());
    Iocp::get()->subscribe(get_native_fd(), this);
    is_read_active_ = true;
    notify_iocp_connected();
  }

  SocketFdImpl(NativeFd native_fd, const IPAddress &addr) : info(std::move(native_fd)) {
    VLOG(fd) << get_native_fd() << " create from native_fd and connect";
    get_poll_info().add_flags(PollFlags::Write());
    Iocp::get()->subscribe(get_native_fd(), this);
    LPFN_CONNECTEX ConnectExPtr = nullptr;
    GUID guid = WSAID_CONNECTEX;
    DWORD numBytes;
    auto error =
        ::WSAIoctl(get_native_fd().socket(), SIO_GET_EXTENSION_FUNCTION_POINTER, static_cast<void *>(&guid),
                   sizeof(guid), static_cast<void *>(&ConnectExPtr), sizeof(ConnectExPtr), &numBytes, nullptr, nullptr);
    if (error) {
      on_error(OS_SOCKET_ERROR("WSAIoctl failed"));
      return;
    }
    std::memset(&read_overlapped_, 0, sizeof(read_overlapped_));
    inc_refcnt();
    is_read_active_ = true;
    auto status = ConnectExPtr(get_native_fd().socket(), addr.get_sockaddr(), narrow_cast<int>(addr.get_sockaddr_len()),
                               nullptr, 0, nullptr, &read_overlapped_);

    if (status == TRUE || !check_status("Failed to connect")) {
      is_read_active_ = false;
      dec_refcnt();
    }
  }

  void close() {
    notify_iocp_close();
  }

  PollableFdInfo &get_poll_info() {
    return info;
  }
  const PollableFdInfo &get_poll_info() const {
    return info;
  }

  const NativeFd &get_native_fd() const {
    return info.native_fd();
  }

  Result<size_t> write(Slice data) {
    output_writer_.append(data);
    if (is_write_waiting_) {
      auto lock = lock_.lock();
      is_write_waiting_ = false;
      notify_iocp_write();
    }
    return data.size();
  }

  Result<size_t> read(MutableSlice slice) {
    if (get_poll_info().get_flags().has_pending_error()) {
      TRY_STATUS(get_pending_error());
    }
    input_reader_.sync_with_writer();
    auto res = input_reader_.advance(td::min(slice.size(), input_reader_.size()), slice);
    if (res == 0) {
      get_poll_info().clear_flags(PollFlags::Read());
    }
    return res;
  }

  Status get_pending_error() {
    Status res;
    {
      auto lock = lock_.lock();
      if (!pending_errors_.empty()) {
        res = pending_errors_.pop();
      }
      if (res.is_ok()) {
        get_poll_info().clear_flags(PollFlags::Error());
      }
    }
    return res;
  }

 private:
  PollableFdInfo info;
  SpinLock lock_;

  std::atomic<int> refcnt_{1};
  bool close_flag_{false};

  bool is_connected_{false};
  bool is_read_active_{false};
  ChainBufferWriter input_writer_;
  ChainBufferReader input_reader_ = input_writer_.extract_reader();
  WSAOVERLAPPED read_overlapped_;
  VectorQueue<Status> pending_errors_;

  bool is_write_active_{false};
  std::atomic<bool> is_write_waiting_{false};
  ChainBufferWriter output_writer_;
  ChainBufferReader output_reader_ = output_writer_.extract_reader();
  WSAOVERLAPPED write_overlapped_;

  char close_overlapped_;

  bool check_status(Slice message) {
    auto last_error = WSAGetLastError();
    if (last_error == ERROR_IO_PENDING) {
      return true;
    }
    on_error(OS_SOCKET_ERROR(message));
    return false;
  }

  void loop_read() {
    CHECK(is_connected_);
    CHECK(!is_read_active_);
    if (close_flag_) {
      return;
    }
    std::memset(&read_overlapped_, 0, sizeof(read_overlapped_));
    auto dest = input_writer_.prepare_append();
    WSABUF buf;
    buf.len = narrow_cast<ULONG>(dest.size());
    buf.buf = dest.data();
    DWORD flags = 0;
    int status = WSARecv(get_native_fd().socket(), &buf, 1, nullptr, &flags, &read_overlapped_, nullptr);
    if (status == 0 || check_status("Failed to read from connection")) {
      inc_refcnt();
      is_read_active_ = true;
    }
  }

  void loop_write() {
    CHECK(is_connected_);
    CHECK(!is_write_active_);

    output_reader_.sync_with_writer();
    auto to_write = output_reader_.prepare_read();
    if (to_write.empty()) {
      auto lock = lock_.lock();
      to_write = output_reader_.prepare_read();
      if (to_write.empty()) {
        is_write_waiting_ = true;
        return;
      }
    }
    if (to_write.empty()) {
      return;
    }
    auto dest = output_reader_.prepare_read();
    std::memset(&write_overlapped_, 0, sizeof(write_overlapped_));
    WSABUF buf;
    buf.len = narrow_cast<ULONG>(dest.size());
    buf.buf = const_cast<CHAR *>(dest.data());
    int status = WSASend(get_native_fd().socket(), &buf, 1, nullptr, 0, &write_overlapped_, nullptr);
    if (status == 0 || check_status("Failed to write to connection")) {
      inc_refcnt();
      is_write_active_ = true;
    }
  }

  void on_iocp(Result<size_t> r_size, WSAOVERLAPPED *overlapped) override {
    // called from other thread
    if (dec_refcnt() || close_flag_) {
      VLOG(fd) << "ignore iocp (file is closing)";
      return;
    }
    if (r_size.is_error()) {
      return on_error(r_size.move_as_error());
    }

    if (!is_connected_ && overlapped == &read_overlapped_) {
      return on_connected();
    }

    auto size = r_size.move_as_ok();
    if (overlapped == &write_overlapped_) {
      return on_write(size);
    }
    if (overlapped == nullptr) {
      CHECK(size == 0);
      return on_write(size);
    }

    if (overlapped == &read_overlapped_) {
      return on_read(size);
    }
    if (overlapped == reinterpret_cast<WSAOVERLAPPED *>(&close_overlapped_)) {
      return on_close();
    }
    UNREACHABLE();
  }

  void on_error(Status status) {
    VLOG(fd) << get_native_fd() << " on error " << status;
    {
      auto lock = lock_.lock();
      pending_errors_.push(std::move(status));
    }
    get_poll_info().add_flags_from_poll(PollFlags::Error());
  }

  void on_connected() {
    VLOG(fd) << get_native_fd() << " on connected";
    CHECK(!is_connected_);
    CHECK(is_read_active_);
    is_connected_ = true;
    is_read_active_ = false;
    loop_read();
    loop_write();
  }

  void on_read(size_t size) {
    VLOG(fd) << get_native_fd() << " on read " << size;
    CHECK(is_read_active_);
    is_read_active_ = false;
    if (size == 0) {
      get_poll_info().add_flags_from_poll(PollFlags::Close());
      return;
    }
    input_writer_.confirm_append(size);
    get_poll_info().add_flags_from_poll(PollFlags::Read());
    loop_read();
  }

  void on_write(size_t size) {
    VLOG(fd) << get_native_fd() << " on write " << size;
    if (size == 0) {
      if (is_write_active_) {
        return;
      }
      is_write_active_ = true;
    }
    CHECK(is_write_active_);
    is_write_active_ = false;
    output_reader_.confirm_read(size);
    loop_write();
  }

  void on_close() {
    VLOG(fd) << get_native_fd() << " on close";
    close_flag_ = true;
    info.set_native_fd({});
  }
  bool dec_refcnt() {
    VLOG(fd) << get_native_fd() << " dec_refcnt from " << refcnt_;
    if (--refcnt_ == 0) {
      delete this;
      return true;
    }
    return false;
  }
  void inc_refcnt() {
    CHECK(refcnt_ != 0);
    refcnt_++;
    VLOG(fd) << get_native_fd() << " inc_refcnt to " << refcnt_;
  }

  void notify_iocp_write() {
    inc_refcnt();
    Iocp::get()->post(0, this, nullptr);
  }
  void notify_iocp_close() {
    Iocp::get()->post(0, this, reinterpret_cast<WSAOVERLAPPED *>(&close_overlapped_));
  }
  void notify_iocp_connected() {
    inc_refcnt();
    Iocp::get()->post(0, this, &read_overlapped_);
  }
};

void SocketFdImplDeleter::operator()(SocketFdImpl *impl) {
  impl->close();
}

class InitWSA {
 public:
  InitWSA() {
    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;
    if (WSAStartup(wVersionRequested, &wsaData) != 0) {
      auto error = OS_SOCKET_ERROR("Failed to init WSA");
      LOG(FATAL) << error;
    }
  }
};

static InitWSA init_wsa;

#else
class SocketFdImpl {
 public:
  PollableFdInfo info;
  explicit SocketFdImpl(NativeFd fd) : info(std::move(fd)) {
  }
  PollableFdInfo &get_poll_info() {
    return info;
  }
  const PollableFdInfo &get_poll_info() const {
    return info;
  }

  const NativeFd &get_native_fd() const {
    return info.native_fd();
  }
  Result<size_t> write(Slice slice) {
    int native_fd = get_native_fd().socket();
    auto write_res = detail::skip_eintr([&] { return ::write(native_fd, slice.begin(), slice.size()); });
    auto write_errno = errno;
    if (write_res >= 0) {
      return narrow_cast<size_t>(write_res);
    }

    if (write_errno == EAGAIN
#if EAGAIN != EWOULDBLOCK
        || write_errno == EWOULDBLOCK
#endif
    ) {
      get_poll_info().clear_flags(PollFlags::Write());
      return 0;
    }

    auto error = Status::PosixError(write_errno, PSLICE() << "Write to fd " << native_fd << " has failed");
    switch (write_errno) {
      case EBADF:
      case ENXIO:
      case EFAULT:
      case EINVAL:
        LOG(FATAL) << error;
        UNREACHABLE();
      default:
        LOG(WARNING) << error;
      // fallthrough
      case ECONNRESET:
      case EDQUOT:
      case EFBIG:
      case EIO:
      case ENETDOWN:
      case ENETUNREACH:
      case ENOSPC:
      case EPIPE:
        get_poll_info().clear_flags(PollFlags::Write());
        get_poll_info().add_flags(PollFlags::Close());
        return std::move(error);
    }
  }
  Result<size_t> read(MutableSlice slice) {
    if (get_poll_info().get_flags().has_pending_error()) {
      TRY_STATUS(get_pending_error());
    }
    int native_fd = get_native_fd().socket();
    CHECK(slice.size() > 0);
    auto read_res = detail::skip_eintr([&] { return ::read(native_fd, slice.begin(), slice.size()); });
    auto read_errno = errno;
    if (read_res >= 0) {
      if (read_res == 0) {
        errno = 0;
        get_poll_info().clear_flags(PollFlags::Read());
        get_poll_info().add_flags(PollFlags::Close());
      }
      return narrow_cast<size_t>(read_res);
    }
    if (read_errno == EAGAIN
#if EAGAIN != EWOULDBLOCK
        || read_errno == EWOULDBLOCK
#endif
    ) {
      get_poll_info().clear_flags(PollFlags::Read());
      return 0;
    }
    auto error = Status::PosixError(read_errno, PSLICE() << "Read from fd " << native_fd << " has failed");
    switch (read_errno) {
      case EISDIR:
      case EBADF:
      case ENXIO:
      case EFAULT:
      case EINVAL:
        LOG(FATAL) << error;
        UNREACHABLE();
      default:
        LOG(WARNING) << error;
      // fallthrough
      case ENOTCONN:
      case EIO:
      case ENOBUFS:
      case ENOMEM:
      case ECONNRESET:
      case ETIMEDOUT:
        get_poll_info().clear_flags(PollFlags::Read());
        get_poll_info().add_flags(PollFlags::Close());
        return std::move(error);
    }
  }
  Status get_pending_error() {
    if (!get_poll_info().get_flags().has_pending_error()) {
      return Status::OK();
    }
    TRY_STATUS(detail::get_socket_pending_error(get_native_fd()));
    get_poll_info().clear_flags(PollFlags::Error());
    return Status::OK();
  }
};

void SocketFdImplDeleter::operator()(SocketFdImpl *impl) {
  delete impl;
}

Status get_socket_pending_error(const NativeFd &fd) {
  int error = 0;
  socklen_t errlen = sizeof(error);
  if (getsockopt(fd.socket(), SOL_SOCKET, SO_ERROR, static_cast<void *>(&error), &errlen) == 0) {
    if (error == 0) {
      return Status::OK();
    }
    return Status::PosixError(error, PSLICE() << "Error on socket [fd_ = " << fd << "]");
  }
  auto status = OS_SOCKET_ERROR(PSLICE() << "Can't load error on socket [fd_ = " << fd << "]");
  LOG(INFO) << "Can't load pending socket error: " << status;
  return status;
}

#endif

Status init_socket_options(NativeFd &native_fd) {
  TRY_STATUS(native_fd.set_is_blocking_unsafe(false));

  auto sock = native_fd.socket();
#if TD_PORT_POSIX
  int flags = 1;
#elif TD_PORT_WINDOWS
  BOOL flags = TRUE;
#endif
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&flags), sizeof(flags));
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&flags), sizeof(flags));
  setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&flags), sizeof(flags));
  // TODO: SO_REUSEADDR, SO_KEEPALIVE, TCP_NODELAY, SO_SNDBUF, SO_RCVBUF, TCP_QUICKACK, SO_LINGER

  return Status::OK();
}

}  // namespace detail

SocketFd::SocketFd() = default;
SocketFd::SocketFd(SocketFd &&) = default;
SocketFd &SocketFd::operator=(SocketFd &&) = default;
SocketFd::~SocketFd() = default;

SocketFd::SocketFd(unique_ptr<detail::SocketFdImpl> impl) : impl_(impl.release()) {
}

Result<SocketFd> SocketFd::from_native_fd(NativeFd fd) {
  TRY_STATUS(detail::init_socket_options(fd));
  return SocketFd(make_unique<detail::SocketFdImpl>(std::move(fd)));
}

Result<SocketFd> SocketFd::open(const IPAddress &address) {
  NativeFd native_fd{socket(address.get_address_family(), SOCK_STREAM, 0)};
  if (!native_fd) {
    return OS_SOCKET_ERROR("Failed to create a socket");
  }
  TRY_STATUS(detail::init_socket_options(native_fd));

#if TD_PORT_POSIX
  int e_connect =
      connect(native_fd.socket(), address.get_sockaddr(), narrow_cast<socklen_t>(address.get_sockaddr_len()));
  if (e_connect == -1) {
    auto connect_errno = errno;
    if (connect_errno != EINPROGRESS) {
      return Status::PosixError(connect_errno, PSLICE() << "Failed to connect to " << address);
    }
  }
  return SocketFd(make_unique<detail::SocketFdImpl>(std::move(native_fd)));
#elif TD_PORT_WINDOWS
  auto bind_addr = address.get_any_addr();
  auto e_bind = bind(native_fd.socket(), bind_addr.get_sockaddr(), narrow_cast<int>(bind_addr.get_sockaddr_len()));
  if (e_bind != 0) {
    return OS_SOCKET_ERROR("Failed to bind a socket");
  }
  return SocketFd(make_unique<detail::SocketFdImpl>(std::move(native_fd), address));
#endif
}

void SocketFd::close() {
  impl_.reset();
}

bool SocketFd::empty() const {
  return !impl_;
}

PollableFdInfo &SocketFd::get_poll_info() {
  return impl_->get_poll_info();
}
const PollableFdInfo &SocketFd::get_poll_info() const {
  return impl_->get_poll_info();
}

const NativeFd &SocketFd::get_native_fd() const {
  return impl_->get_native_fd();
}

Status SocketFd::get_pending_error() {
  return impl_->get_pending_error();
}

Result<size_t> SocketFd::write(Slice slice) {
  return impl_->write(slice);
}

Result<size_t> SocketFd::read(MutableSlice slice) {
  return impl_->read(slice);
}

}  // namespace td
