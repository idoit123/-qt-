#include "epoll_server.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    const auto port = static_cast<std::uint16_t>(
        argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 8888);
    const std::string databasePath = argc > 2 ? argv[2] : "cloud.db";
    const std::size_t hardware = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t ioThreads = argc > 3
        ? std::max<std::size_t>(1, std::strtoul(argv[3], nullptr, 10))
        : hardware;

    std::cout << "epoll chat server: port=" << port
              << " db=" << databasePath
              << " io_threads=" << ioThreads << '\n';
    try {
        chat::EpollServer server(port, databasePath, ioThreads);
        return server.run() ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception &error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
