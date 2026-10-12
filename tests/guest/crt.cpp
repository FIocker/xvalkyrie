// Test guest with the CRT: startup, heap, stdio, RTTI and exceptions.
#include <cstdio>
#include <stdexcept>
#include <vector>

struct Base {
    virtual ~Base() = default;
};
struct Derived : Base {
    int value = 4;
};

int main() {
    Derived object;
    Base *base = &object;
    try {
        try {
            throw Derived();
        } catch (const Base &) {
            throw;
        }
    } catch (const Derived &caught) {
        if (dynamic_cast<Derived *>(base)->value != caught.value)
            return 1;
    }
    std::vector<int> values{1, 2, 3};
    int sum = 0;
    try {
        for (const int value : values)
            sum += value;
        throw std::runtime_error("unwound");
    } catch (const std::exception &error) {
        std::printf("guest-crt-ok %d %s\n", sum, error.what());
    }
    return 7;
}
