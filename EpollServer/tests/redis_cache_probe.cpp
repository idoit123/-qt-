#include "database.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    try {
        chat::Database db(argv[1]);
        if (!db.initialize()) return 3;
        std::string command, user, other;
        while (std::cin >> command) {
            if (command == "register") {
                std::cin >> user;
                std::cout << db.registerUser(user, "pw");
            } else if (command == "add" || command == "delete") {
                std::cin >> user >> other;
                std::cout << (command == "add" ? db.addFriend(user, other)
                                                : db.deleteFriend(user, other));
            } else if (command == "read") {
                std::cin >> user;
                const auto names = db.friends(user);
                std::cout << names.size();
                for (const auto &name : names) std::cout << ' ' << name;
            } else if (command == "stats") {
                const auto s = db.cacheStats();
                std::cout << s.redis.hits << ' ' << s.redis.misses << ' '
                          << s.redis.errors << ' ' << s.redis.writes << ' ' << s.friendQueries;
            } else if (command == "loop") {
                int count = 0;
                std::cin >> user >> count;
                const auto before = db.cacheStats();
                std::size_t total = 0;
                std::vector<double> latency;
                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < count; ++i) {
                    const auto tick = std::chrono::steady_clock::now();
                    total += db.friends(user).size();
                    latency.push_back(std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - tick).count());
                }
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                std::sort(latency.begin(), latency.end());
                const auto after = db.cacheStats();
                std::cout << ms << ' ' << total << ' '
                          << after.friendQueries - before.friendQueries << ' '
                          << after.redis.hits - before.redis.hits << ' '
                          << latency.at(latency.size() * 99 / 100);
            } else if (command == "race") {
                // Concurrent cache fills and DB mutations must agree after join.
                std::vector<std::thread> readers;
                for (int n = 0; n < 4; ++n)
                    readers.emplace_back([&db]() {
                        for (int i = 0; i < 100; ++i) db.friends("a");
                    });
                for (int i = 0; i < 30; ++i) {
                    if (!db.addFriend("a", "b") || !db.deleteFriend("a", "b")) return 4;
                }
                for (auto &thread : readers) thread.join();
                std::cout << db.friends("a").empty();
            } else {
                return 5;
            }
            std::cout << std::endl;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 6;
    }
}
