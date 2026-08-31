#pragma once

#include "fillet/ImprovedFilletRecognizer.hpp"

#include <TopoDS_Shape.hxx>

#include <filesystem>
#include <string>

namespace fillet::report {

std::filesystem::path write(const improved::Result& result,
                            const TopoDS_Shape& sourceShape,
                            const std::filesystem::path& outputRoot,
                            const std::string& modelName);

std::filesystem::path writeSewingComparison(
    const improved::SewingComparison& comparison,
    const std::filesystem::path& outputRoot,
    const std::string& modelName);

} // namespace fillet::report
