#include <span>

#include "mux.hpp"

int main(int argc, char** argv) {
    return zet::mux::Main(std::span{argv, static_cast<std::size_t>(argc)});
}
