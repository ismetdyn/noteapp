#pragma once

// Config'teki sınırların (core::Limits, `0 = sınırsız`) motor sınırlarına çevirisi. Motor config okumaz;
// NoteStore ve CLI motoru bu değerlerle açar. Sınırsız SIZE_MAX'tır (süre için milliseconds::max()).
#include "core/limits.hpp"
#include "graph/graph.hpp"
#include "graph/query.hpp"

namespace noteapp::core {

graph::GraphLimits graph_limits_of(const GraphLimits& limits);
// Kullanıcının sorgusu (limits.query) ve görünüm (limits.view) aynı çeviriyi kullanır.
graph::QueryLimits query_limits_of(const QueryLimits& limits);

}  // namespace noteapp::core
