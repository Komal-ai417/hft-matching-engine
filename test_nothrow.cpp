#include <type_traits>
#include <iostream>
#include <vector>
struct Trade {};
int main() {
    std::vector<Trade> v;
    auto f = [&](const Trade& t) noexcept { v.push_back(t); };
    std::cout << std::is_nothrow_invocable_v<decltype(f)&&, const Trade&> << std::endl;
    return 0;
}
