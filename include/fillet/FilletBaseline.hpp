#pragma once

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <vector>

namespace fillet::baseline {

struct Options {
  bool includeCylinder = true;
  bool includeTorus = true;
  bool includeSphere = true;
  bool requireSimilarRadius = true;
  double radiusAbsTolerance = 1.0e-4;
  double radiusRelTolerance = 0.05;
  double tangentAngleToleranceRadians = 3.0 * 3.14159265358979323846 / 180.0;
  int minimumTangentSupportSides = 2;
  bool rejectWithoutEnoughSupports = true;
};

struct Chain {
  std::vector<TopoDS_Face> faces;
  std::vector<TopoDS_Edge> boundaryEdges;
  std::vector<TopoDS_Edge> tangentSupportEdges;
  std::vector<TopoDS_Edge> endEdges;
  double minimumRadius = 0.0;
  double maximumRadius = 0.0;
  double meanRadius = 0.0;
  int tangentSupportSideCount = 0;
  bool isClosed = false;
  bool isBranched = false;
};

std::vector<Chain> search(const TopoDS_Shape& shape,
                          double minimumRadius,
                          double maximumRadius,
                          const Options& options = {});

} // namespace fillet::baseline

