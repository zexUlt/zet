#include <span>

#include "agent.hpp"

int main(int argc, char** argv) {
    return zet::agent::Main(std::span{argv, static_cast<std::size_t>(argc)});
}
