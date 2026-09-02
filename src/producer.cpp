#include <charconv>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

namespace {

void print_usage(std::string_view program_name) {
    std::cerr << "Usage: " << program_name << " <payload_size_bytes>\n";
}

bool parse_payload_size(std::string_view argument, std::size_t& payload_size) {
    if (argument.empty() || argument.front() == '-') {
        return false;
    }

    unsigned long long parsed_value = 0;
    const char* const begin = argument.data();
    const char* const end = begin + argument.size();
    const auto result = std::from_chars(begin, end, parsed_value, 10);

    if (result.ec != std::errc{} || result.ptr != end || parsed_value == 0 ||
        parsed_value > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    payload_size = static_cast<std::size_t>(parsed_value);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::size_t payload_size = 0;
    if (!parse_payload_size(argv[1], payload_size)) {
        std::cerr << "Error: payload size must be a positive integer in bytes.\n";
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "Payload size: " << payload_size << " bytes\n";
    return 0;
}
