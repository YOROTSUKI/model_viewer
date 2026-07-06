#pragma once

#include "Model.h"

#include <filesystem>

namespace viewer {

LoadedModel loadCastModel(const std::filesystem::path& path);

} // namespace viewer
