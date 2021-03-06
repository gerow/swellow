#include <alloca.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_set>

#define N_ELEM(__x) (sizeof(__x) / sizeof(__x[0]))

static const char usage[] = "Usage: %s PORT\n";
static const int kBacklog = 50;

using std::string;

class UniqueFd {
 public:
  explicit UniqueFd(int fd) : fd_(fd) {}
  operator int() const { return fd_; }
  ~UniqueFd() {
    if (close(fd_) == -1) {
      std::cerr << "Failed to close " << fd_ << ": " << strerror(errno);
    }
  }

 private:
  int fd_;

  UniqueFd() = delete;
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
};

class Poll {
 public:
  virtual void Handle(struct epoll_event *event) = 0;
  virtual ~Poll() = default;
};

/*
class Request {
 public:
  string method;
  string path;

  static std::unique_ptr<Request> Parse(const std::string_view in) {
    size_t linePos = in.find("\r\n");
    if (linePos == string::npos) return nullptr;
    auto line = std::string_view(in.data(), linePos);


  }
};
*/

class Client : public Poll {
 public:
  // takes ownerhip of fd
  explicit Client(int fd, int epfd) : fd_(fd), epfd_(epfd) {
    // can we construct this?
    event_.data = {.ptr = this};
  }

  void Register() {
    event_.events = EPOLLIN;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd_, &event_) == -1) {
      perror("epoll_ctl");
      exit(EXIT_FAILURE);
    }
  }

  void handleRead() {
    char buf[4096];
    ssize_t len = read(fd_, buf, sizeof(buf));
    std::cerr << "got read of " << len << " bytes" << std::endl;
    if (len == -1) {
      if (errno != EAGAIN && errno != EINTR) {
        perror("read");
        exit(EXIT_FAILURE);
      }
      return;
    }
    if (len == 0) {
      std::cerr << "hup on fd " << fd_ << "; closing" << std::endl;
      if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd_, nullptr) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
      }
      // XXX(gerow): we did't close this or delete client, bad bad bad
    } else {
      in_.append(buf, len);
      std::cerr << "in for fd " << fd_ << " is now" << std::endl
                << Peek() << std::endl;
      // just for testing, ignore request and just say what we want
      this->PopRequest();
      this->Write(
          "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello world!\n");
    }
  }

  void handleWrite() {
    ssize_t len = write(fd_, out_.c_str(), out_.size());
    std::cerr << "wrote " << len << " bytes" << std::endl;
    if (len == -1) {
      if (errno != EAGAIN && errno != EINTR) {
        perror("write");
        exit(EXIT_FAILURE);
      }
    }
    out_.erase(0, len);
    if (out_.size() == 0) {
      event_.events &= ~EPOLLOUT;
      if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd_, &event_) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
      }
    }
  }

  void Handle(struct epoll_event *event) override {
    if (event->events & EPOLLIN) {
      handleRead();
    }
    if (event->events & EPOLLOUT) {
      handleWrite();
    }
  }

  const string PopRequest() {
    size_t pos = in_.find("\r\n\r\n", 0);
    if (pos == string::npos) {
      return "";
    }
    return Read(pos + 4);
  }

  const string &Peek() const { return in_; }

  string Read(size_t len) {
    string out = in_.substr(0, len);
    in_.erase(0, len);
    return out;
  }

  void Write(const string &data) {
    if (data.size() == 0) {
      return;
    }
    out_.append(data);
    if (!(event_.events & EPOLLOUT)) {
      event_.events |= EPOLLOUT;
      if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd_, &event_) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
      }
    }
  }

 private:
  UniqueFd fd_;
  int epfd_;
  epoll_event event_;
  // these would be better as ring buffers
  string in_, out_;

  Client() = delete;
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
};

class Listener : public Poll {
 public:
  Listener(int fd, int epfd) : fd_(fd), epfd_(epfd) {}

  void Register() {
    struct epoll_event event = {
        .events = EPOLLIN,
        .data = {.ptr = this},
    };
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd_, &event) == -1) {
      perror("epoll_ctl");
      exit(EXIT_FAILURE);
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
      perror("signal");
      exit(EXIT_FAILURE);
    }
  }

  void Handle(struct epoll_event *event) override {
    // keep accepting clients until we've accepted all of them
    for (;;) {
      struct sockaddr_storage addr = {};
      socklen_t addrlen = sizeof(addr);
      int cfd = accept4(fd_, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
      if (cfd == -1) {
        if (errno == EAGAIN || errno == EINTR) {
          perror("accept4");
          break;
        }
        perror("accept4");
        continue;
      }
      char host[NI_MAXHOST] = {};
      char service[NI_MAXSERV] = {};
      if (getnameinfo((struct sockaddr *)&addr, addrlen, host, sizeof(host),
                      service, sizeof(service), 0) == 0) {
        std::cerr << "accepted new client at (" << host << ", " << service
                  << ")" << std::endl;
      } else {
        std::cerr << "accepted new client with unknown host/service"
                  << std::endl;
      }
      auto client = std::make_unique<Client>(cfd, epfd_);
      client->Register();
      auto ret = clients_.insert(std::move(client));
      if (!ret.second) {
        std::cerr << "found conflicting client, (how?)" << std::endl;
        exit(EXIT_FAILURE);
      }
    }
  }

 private:
  UniqueFd fd_;
  int epfd_;
  std::unordered_set<std::unique_ptr<Client>> clients_;

  Listener() = delete;
  Listener(const Listener &) = delete;
  Listener &operator=(const Listener &) = delete;
};

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, usage, argv[0]);
    exit(EXIT_FAILURE);
  }
  const char *port = argv[1];

  struct addrinfo hints = {
      .ai_flags = AI_PASSIVE | AI_NUMERICSERV,
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
  };

  struct addrinfo *result = nullptr;
  if (getaddrinfo(nullptr, port, &hints, &result) != 0) {
    perror("getaddrinfo");
    exit(EXIT_FAILURE);
  }
  int lfd = -1;

  struct addrinfo *rp = nullptr;
  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    lfd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (lfd == -1) continue;
    int reuseopt = 1;
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuseopt,
                   sizeof(reuseopt)) == -1) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }
    if (bind(lfd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    // Bind failed so keep looking.
    close(lfd);
  }
  freeaddrinfo(result);
  if (rp == nullptr) {
    std::cerr << "failed to bind to wildcard address on port " << port
              << std::endl;
    exit(EXIT_FAILURE);
  }
  if (listen(lfd, kBacklog) == -1) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  std::cerr << "listening on port " << port << std::endl;

  int epfd = epoll_create(1);
  if (epfd == -1) {
    perror("epoll_create");
    exit(EXIT_FAILURE);
  }

  auto listener = std::make_unique<Listener>(lfd, epfd);
  listener->Register();
  struct epoll_event events[100] = {};
  for (;;) {
    int numEvents = epoll_wait(epfd, events, N_ELEM(events), -1);
    if (numEvents == -1) {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }
    for (int i = 0; i < numEvents; i++) {
      ((Poll *)events[i].data.ptr)->Handle(&events[i]);
    }
  }
}
