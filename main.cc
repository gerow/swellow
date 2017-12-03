#include <alloca.h>
#include <arpa/inet.h>
#include <errno.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <set>
#include <sstream>

static const char usage[] = "Usage: %s PORT\n";
static const int kBacklog = 50;

using std::string;

class Client {
public:
  // takes ownerhip of fd
  explicit Client(int fd) : fd_(fd) {}

private:
  int fd_;
  string in_, out_;
};

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, usage, argv[0]);
    exit(EXIT_FAILURE);
  }
  const char *port = argv[1];

  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    perror("signal");
    exit(EXIT_FAILURE);
  }

  struct addrinfo hints = {};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

  struct addrinfo *result = nullptr;
  if (getaddrinfo(nullptr, port, &hints, &result) != 0) {
    perror("getaddrinfo");
    exit(EXIT_FAILURE);
  }
  int lfd = -1;

  struct addrinfo *rp = nullptr;
  for (rp = result; rp != nullptr; rp = rp->ai_next) {
    lfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (lfd == -1)
      continue;
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

  std::set<std::unique_ptr<Client>> clients;
  for (;;) {
    struct sockaddr_storage addr = {};
    socklen_t addrlen = sizeof(addr);
    int cfd = accept(lfd, (struct sockaddr *)&addr, &addrlen);
    if (cfd == -1) {
      perror("accept");
      continue;
    }
    char host[NI_MAXHOST] = {};
    char service[NI_MAXSERV] = {};
    if (getnameinfo((struct sockaddr *)&addr, addrlen, host, sizeof(host),
                    service, sizeof(service), 0) == 0) {
      std::cerr << "accepted new client at (" << host << ", " << service << ")"
                << std::endl;
    } else {
      std::cerr << "accepted new client with unknown host/service" << std::endl;
    }
    auto ret = clients.insert(std::make_unique<Client>(cfd));
    if (!ret.second) {
      std::cerr << "found conflicting client, (how?)" << std::endl;
      exit(EXIT_FAILURE);
    }
  }
}
