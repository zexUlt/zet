#include <span>

#include "client.hpp"

int main(int argc, char** argv) {
    return zet::client::Main(std::span{argv, static_cast<std::size_t>(argc)});
}
