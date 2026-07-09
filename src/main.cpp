#include "deck/app.h"
#include "deck/cli.h"

#include <span>

int main(int argc, char** argv) {
  return deck::run_app(deck::parse_cli(std::span<char*>(argv, static_cast<std::size_t>(argc))));
}
