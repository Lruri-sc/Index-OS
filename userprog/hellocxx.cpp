// Dynamic-linked C++ smoke test. Exercises:
//   * PT_INTERP -> /lib/ld-musl-aarch64.so.1 (the kernel's ELF loader path)
//   * ld-musl finds /usr/lib/libstdc++.so.6 + libgcc_s.so.1 via
//     /etc/ld-musl-aarch64.path (musl's per-arch lib-search config)
//   * libstdc++'s iostreams, string, exception, RTTI all resolve through
//     the shared object instead of being copied in by -static-libstdc++.
//
// Build target lives in the Makefile alongside hellomusl/hellodyn so the
// existing zig cc -dynamic flow does the link.
#include <iostream>
#include <string>
#include <stdexcept>
#include <typeinfo>
#include <vector>

int main() {
    std::cout << "Hello from dynamically linked C++!\n";

    // STL works.
    std::vector<std::string> v{"Index", "is", "alive"};
    for (auto &s : v) std::cout << s << ' ';
    std::cout << '\n';

    // RTTI works.
    const std::type_info &t = typeid(v);
    std::cout << "vector mangled: " << t.name() << '\n';

    // Exceptions across libstdc++.so + libgcc_s.so unwind boundary.
    try {
        throw std::runtime_error("thrown-through-dso");
    } catch (const std::exception &e) {
        std::cout << "caught: " << e.what() << '\n';
    }
    return 0;
}
