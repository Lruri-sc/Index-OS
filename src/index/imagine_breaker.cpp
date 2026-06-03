#include "index/imagine_breaker.hpp"

#include "arch/aarch64/cpu.hpp"
#include "index/imaginary_number_district.hpp"

namespace index {

[[noreturn]] void imagine_breaker(const char *message) {
    imaginary_number_district::write("\n[Imagine Breaker] ");
    imaginary_number_district::writeln(message);
    arch::halt();
}

} // namespace index
