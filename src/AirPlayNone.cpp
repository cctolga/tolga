// Builds without the AirPlay module: no AirPlay. The menu offers the official app instead.
#include "tolga/AirPlayApi.h"

std::unique_ptr<AirPlay> createAirPlay(const std::string&, std::function<void()>, int) { return nullptr; }
