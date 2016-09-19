#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <tuple>
#include <cmath>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/optional.hpp>
#include <boost/format.hpp>

#include "config.h"

#include <valhalla/baldr/graphreader.h>
#include <valhalla/baldr/pathlocation.h>
#include <valhalla/baldr/connectivity_map.h>
#include <valhalla/loki/search.h>
#include <valhalla/sif/costfactory.h>
#include <valhalla/odin/directionsbuilder.h>
#include <valhalla/odin/util.h>
#include <valhalla/proto/trippath.pb.h>
#include <valhalla/proto/tripdirections.pb.h>
#include <valhalla/proto/directions_options.pb.h>
#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/thor/astar.h>
#include <valhalla/thor/bidirectional_astar.h>
#include <valhalla/thor/multimodal.h>
#include <valhalla/thor/trippathbuilder.h>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::loki;
using namespace valhalla::odin;
using namespace valhalla::sif;
using namespace valhalla::thor;

namespace bpo = boost::program_options;

namespace {
  class PathStatistics {
    std::pair<float, float> origin;
    std::pair<float, float> destination;
    std::string success;
    uint32_t passes;
    uint32_t runtime;
    uint32_t trip_time;
    float trip_dist;
    float arc_dist;
    uint32_t manuevers;

  public:
    PathStatistics (std::pair<float, float> p1, std::pair<float, float> p2)
      : origin(p1), destination(p2), success("false"),
        passes(0), runtime(), trip_time(),
        trip_dist(), arc_dist(), manuevers() { }

    void setSuccess(std::string s) { success = s; }
    void incPasses(void) { ++passes; }
    void addRuntime(uint32_t msec) { runtime += msec; }
    void setTripTime(uint32_t t) { trip_time = t; }
    void setTripDist(float d) { trip_dist = d; }
    void setArcDist(float d) { arc_dist = d; }
    void setManuevers(uint32_t n) { manuevers = n; }
    void log() {
      valhalla::midgard::logging::Log(
        (boost::format("%f,%f,%f,%f,%s,%d,%d,%d,%f,%f,%d")
          % origin.first % origin.second % destination.first % destination.second
          % success % passes % runtime % trip_time % trip_dist % arc_dist % manuevers).str(),
        " [STATISTICS] ");
    }
  };
}

/**
 * Test a single path from origin to destination.
 */
TripPath PathTest(GraphReader& reader, PathLocation& origin,
                  PathLocation& dest, PathAlgorithm* pathalgorithm,
                  const std::shared_ptr<DynamicCost>* mode_costing,
                  const TravelMode mode, PathStatistics& data,
                  bool multi_run, uint32_t iterations,
                  bool using_astar) {
  auto t1 = std::chrono::high_resolution_clock::now();
  std::vector<PathInfo> pathedges;
  std::vector<PathLocation> through_loc;
  pathedges = pathalgorithm->GetBestPath(origin, dest, reader, mode_costing, mode);
  cost_ptr_t cost = mode_costing[static_cast<uint32_t>(mode)];
  data.incPasses();
  if (pathedges.size() == 0) {
    if (cost->AllowMultiPass()) {
      LOG_INFO("Try again with relaxed hierarchy limits");
      pathalgorithm->Clear();
      float relax_factor = (using_astar) ? 16.0f : 8.0f;
      float expansion_within_factor = (using_astar) ? 4.0f : 2.0f;
      cost->RelaxHierarchyLimits(using_astar, expansion_within_factor);
      pathedges = pathalgorithm->GetBestPath(origin, dest, reader, mode_costing, mode);
      data.incPasses();
    }
  }
  if (pathedges.size() == 0) {
    // Third pass only if using astar
    if (!using_astar) {
      // Return an empty trip path
      return TripPath();
    }
    cost->DisableHighwayTransitions();
    pathalgorithm->Clear();
    pathedges = pathalgorithm->GetBestPath(origin, dest, reader, mode_costing, mode);
    data.incPasses();
    if (pathedges.size() == 0) {
      // Return an empty trip path
      return TripPath();
    }
  }
  auto t2 = std::chrono::high_resolution_clock::now();
  uint32_t msecs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  LOG_INFO("PathAlgorithm GetBestPath took " + std::to_string(msecs) + " ms");

  // Form trip path
  t1 = std::chrono::high_resolution_clock::now();
  TripPath trip_path = TripPathBuilder::Build(reader, pathedges, origin,
                                              dest, through_loc);
  t2 = std::chrono::high_resolution_clock::now();
  msecs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  LOG_INFO("TripPathBuilder took " + std::to_string(msecs) + " ms");

  // Time how long it takes to clear the path
  t1 = std::chrono::high_resolution_clock::now();
  pathalgorithm->Clear();
  t2 = std::chrono::high_resolution_clock::now();
  msecs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
  LOG_INFO("PathAlgorithm Clear took " + std::to_string(msecs) + " ms");

  // Run again to see benefits of caching
  if (multi_run) {
    uint32_t totalms = 0;
    for (uint32_t i = 0; i < iterations; i++) {
      t1 = std::chrono::high_resolution_clock::now();
      pathedges = pathalgorithm->GetBestPath(origin, dest, reader, mode_costing, mode);
      t2 = std::chrono::high_resolution_clock::now();
      totalms += std::chrono::duration_cast<std::chrono::milliseconds>(t2-t1).count();
      pathalgorithm->Clear();
    }
    msecs = totalms / iterations;
    LOG_INFO("PathAlgorithm GetBestPath average: " + std::to_string(msecs) + " ms");
  }
  return trip_path;
}

namespace std {

//TODO: maybe move this into location.h if its actually useful elsewhere than here?
std::string to_string(const valhalla::baldr::Location& l) {
  std::string s;
  for (auto address : { &l.name_, &l.street_, &l.city_, &l.state_, &l.zip_, &l
      .country_ }) {
    s.append(*address);
    s.push_back(',');
  }
  s.erase(s.end() - 1);
  return s;
}

//TODO: maybe move this into location.h if its actually useful elsewhere than here?
std::string to_json(const valhalla::baldr::Location& l) {
  std::string json = "{";
  json += "\"lat\":";
  json += std::to_string(l.latlng_.lat());

  json += ",\"lon\":";
  json += std::to_string(l.latlng_.lng());

  json += ",\"type\":\"";
  json +=
      (l.stoptype_ == valhalla::baldr::Location::StopType::THROUGH) ?
          "through" : "break";
  json += "\"";

  if (l.heading_) {
    json += ",\"heading\":";
    json += *l.heading_;
  }

  if (!l.name_.empty()) {
    json += ",\"name\":\"";
    json += l.name_;
    json += "\"";
  }

  if (!l.street_.empty()) {
    json += ",\"street\":\"";
    json += l.street_;
    json += "\"";
  }

  if (!l.city_.empty()) {
    json += ",\"city\":\"";
    json += l.city_;
    json += "\"";
  }

  if (!l.state_.empty()) {
    json += ",\"state\":\"";
    json += l.state_;
    json += "\"";
  }

  if (!l.zip_.empty()) {
    json += ",\"postal_code\":\"";
    json += l.zip_;
    json += "\"";
  }

  if (!l.country_.empty()) {
    json += ",\"country\":\"";
    json += l.country_;
    json += "\"";
  }

  json += "}";

  return json;
}

}

std::string GetFormattedTime(uint32_t seconds) {
  uint32_t hours = (uint32_t) seconds / 3600;
  uint32_t minutes = ((uint32_t) (seconds / 60)) % 60;
  std::string formattedTime = "";
  // Hours
  if (hours > 0) {
    formattedTime += std::to_string(hours);
    formattedTime += (hours == 1) ? " hour" : " hours";
    if (minutes > 0) {
      formattedTime += ", ";
    }
  }
  // Minutes
  if (minutes > 0) {
    formattedTime += std::to_string(minutes);
    formattedTime += (minutes == 1) ? " minute" : " minutes";
  }
  return formattedTime;

}

TripDirections DirectionsTest(const DirectionsOptions& directions_options,
                              TripPath& trip_path, Location origin,
                              Location destination, PathStatistics& data) {
  DirectionsBuilder directions;
  TripDirections trip_directions = directions.Build(directions_options,
                                                    trip_path);
  std::string units = (
      directions_options.units()
          == DirectionsOptions::Units::DirectionsOptions_Units_kKilometers ?
          "km" : "mi");
  int m = 1;
  valhalla::midgard::logging::Log("From: " + std::to_string(origin),
                                  " [NARRATIVE] ");
  valhalla::midgard::logging::Log("To: " + std::to_string(destination),
                                  " [NARRATIVE] ");
  valhalla::midgard::logging::Log(
      "==============================================", " [NARRATIVE] ");
  for (int i = 0; i < trip_directions.maneuver_size(); ++i) {
    const auto& maneuver = trip_directions.maneuver(i);

    // Depart instruction
    if (maneuver.has_depart_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   %s")
              % maneuver.depart_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Verbal depart instruction
    if (maneuver.has_verbal_depart_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   VERBAL_DEPART: %s")
              % maneuver.verbal_depart_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Instruction
    valhalla::midgard::logging::Log(
        (boost::format("%d: %s | %.1f %s") % m++ % maneuver.text_instruction()
            % maneuver.length() % units).str(),
        " [NARRATIVE] ");

    // Verbal transition alert instruction
    if (maneuver.has_verbal_transition_alert_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   VERBAL_ALERT: %s")
              % maneuver.verbal_transition_alert_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Verbal pre transition instruction
    if (maneuver.has_verbal_pre_transition_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   VERBAL_PRE: %s")
              % maneuver.verbal_pre_transition_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Verbal post transition instruction
    if (maneuver.has_verbal_post_transition_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   VERBAL_POST: %s")
              % maneuver.verbal_post_transition_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Arrive instruction
    if (maneuver.has_arrive_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   %s")
              % maneuver.arrive_instruction()).str(),
          " [NARRATIVE] ");
    }

    // Verbal arrive instruction
    if (maneuver.has_verbal_arrive_instruction()) {
      valhalla::midgard::logging::Log(
          (boost::format("   VERBAL_ARRIVE: %s")
              % maneuver.verbal_arrive_instruction()).str(),
          " [NARRATIVE] ");
    }

    if (i < trip_directions.maneuver_size() - 1)
      valhalla::midgard::logging::Log(
          "----------------------------------------------", " [NARRATIVE] ");
  }
  valhalla::midgard::logging::Log(
      "==============================================", " [NARRATIVE] ");
  valhalla::midgard::logging::Log(
      "Total time: " + GetFormattedTime(trip_directions.summary().time()),
      " [NARRATIVE] ");
  valhalla::midgard::logging::Log(
      (boost::format("Total length: %.1f %s")
          % trip_directions.summary().length() % units).str(),
      " [NARRATIVE] ");
  data.setTripTime(trip_directions.summary().time());
  data.setTripDist(trip_directions.summary().length());
  data.setManuevers(trip_directions.maneuver_size());

  return trip_directions;
}

// Returns the costing method (created from the dynamic cost factory).
// Get the costing options. Get the base options from the config and the
// options for the specified costing method. Merge in any request costing
// options that override those in the config.
valhalla::sif::cost_ptr_t get_costing(CostFactory<DynamicCost> factory,
                                      boost::property_tree::ptree& config,
                                      boost::property_tree::ptree& request,
                                      const std::string& costing) {
 std::string method_options = "costing_options." + costing;
 auto config_costing = config.get_child_optional(method_options);
 if (!config_costing)
   throw std::runtime_error("No costing method found for '" + costing + "'");
 auto request_costing = request.get_child_optional(method_options);
 if (request_costing) {
   // If the request has any options for this costing type, merge the 2
   // costing options - override any config options that are in the request.
   // and add any request options not in the config.
   for (const auto& r : *request_costing) {
     config_costing->put_child(r.first, r.second);
   }
 }
 return factory.Create(costing, *config_costing);
}

// Main method for testing a single path
int main(int argc, char *argv[]) {
  bpo::options_description options("valhalla_run_route " VERSION "\n"
  "\n"
  " Usage: valhalla_run_route [options]\n"
  "\n"
  "valhalla_run_route is a simple command line test tool for shortest path routing. "
  "\n"
  "Use the -o and -d options OR the -j option for specifying the locations. "
  "\n"
  "\n");

  std::string origin, destination, routetype, json, config;
  bool connectivity, multi_run;
  connectivity = multi_run = false;
  uint32_t iterations;

  options.add_options()("help,h", "Print this help message.")(
      "version,v", "Print the version of this software.")(
      "origin,o",
      boost::program_options::value<std::string>(&origin),
      "Origin: lat,lng,[through|stop],[name],[street],[city/town/village],[state/province/canton/district/region/department...],[zip code],[country].")(
      "destination,d",
      boost::program_options::value<std::string>(&destination),
      "Destination: lat,lng,[through|stop],[name],[street],[city/town/village],[state/province/canton/district/region/department...],[zip code],[country].")(
      "type,t", boost::program_options::value<std::string>(&routetype),
      "Route Type: auto|bicycle|pedestrian|auto-shorter")(
      "json,j",
      boost::program_options::value<std::string>(&json),
      "JSON Example: '{\"locations\":[{\"lat\":40.748174,\"lon\":-73.984984,\"type\":\"break\",\"heading\":200,\"name\":\"Empire State Building\",\"street\":\"350 5th Avenue\",\"city\":\"New York\",\"state\":\"NY\",\"postal_code\":\"10118-0110\",\"country\":\"US\"},{\"lat\":40.749231,\"lon\":-73.968703,\"type\":\"break\",\"name\":\"United Nations Headquarters\",\"street\":\"405 East 42nd Street\",\"city\":\"New York\",\"state\":\"NY\",\"postal_code\":\"10017-3507\",\"country\":\"US\"}],\"costing\":\"auto\",\"directions_options\":{\"units\":\"miles\"}}'")
      ("connectivity", "Generate a connectivity map before testing the route.")
      ("multi-run", bpo::value<uint32_t>(&iterations), "Generate the route N additional times before exiting.")
      // positional arguments
      ("config", bpo::value<std::string>(&config), "Valhalla configuration file");


  bpo::positional_options_description pos_options;
  pos_options.add("config", 1);

  bpo::variables_map vm;

  try {
    bpo::store(
        bpo::command_line_parser(argc, argv).options(options).positional(
            pos_options).run(),
        vm);
    bpo::notify(vm);

  } catch (std::exception &e) {
    std::cerr << "Unable to parse command line options because: " << e.what()
              << "\n" << "This is a bug, please report it at " PACKAGE_BUGREPORT
              << "\n";
    return EXIT_FAILURE;
  }

  if (vm.count("help")) {
    std::cout << options << "\n";
    return EXIT_SUCCESS;
  }

  if (vm.count("version")) {
    std::cout << "valhalla_run_route " << VERSION << "\n";
    return EXIT_SUCCESS;
  }

  if (vm.count("connectivity")) {
    connectivity = true;
  }

  if (vm.count("multi-run")) {
    multi_run = true;
  }

  // Directions options - set defaults
  DirectionsOptions directions_options;
  directions_options.set_units(
      DirectionsOptions::Units::DirectionsOptions_Units_kMiles);
  directions_options.set_language("en-US");

  // Locations
  std::vector<Location> locations;

  // argument checking and verification
  boost::property_tree::ptree json_ptree;
  if (vm.count("json") == 0) {
    for (auto arg : std::vector<std::string> { "origin", "destination", "type",
        "config" }) {
      if (vm.count(arg) == 0) {
        std::cerr
            << "The <"
            << arg
            << "> argument was not provided, but is mandatory when json is not provided\n\n";
        std::cerr << options << "\n";
        return EXIT_FAILURE;
      }
    }
    locations.push_back(Location::FromCsv(origin));
    locations.push_back(Location::FromCsv(destination));
  }
  ////////////////////////////////////////////////////////////////////////////
  // Process json input
  else {
    std::stringstream stream;
    stream << json;
    boost::property_tree::read_json(stream, json_ptree);

    try {
      for (const auto& location : json_ptree.get_child("locations"))
        locations.emplace_back(std::move(Location::FromPtree(location.second)));
      if (locations.size() < 2)
        throw;
    } catch (...) {
      throw std::runtime_error(
          "insufficiently specified required parameter 'locations'");
    }

    // Parse out the type of route - this provides the costing method to use
    std::string costing;
    try {
      routetype = json_ptree.get<std::string>("costing");
    } catch (...) {
      throw std::runtime_error("No edge/node costing provided");
    }

    // Grab the directions options, if they exist
    auto directions_options_ptree_ptr = json_ptree.get_child_optional(
        "directions_options");
    if (directions_options_ptree_ptr) {
      directions_options = valhalla::odin::GetDirectionsOptions(
          *directions_options_ptree_ptr);
    }

    // Grab the date_time, if is exists
    auto date_time_ptr = json_ptree.get_child_optional("date_time");
    if (date_time_ptr) {
      auto date_time_type = (*date_time_ptr).get<int>("type");
      auto date_time_value = (*date_time_ptr).get_optional<std::string>("value");

      if (date_time_type == 0) // current
        locations.front().date_time_ = "current";
      else if (date_time_type == 1) // depart at
        locations.front().date_time_ = date_time_value;
      else if (date_time_type == 2) // arrive by
        locations.back().date_time_ = date_time_value;
    }

  }

  // TODO: remove after input files are transformed
#ifdef LOGGING_LEVEL_DEBUG
  std::string json_input = "-j '{\"locations\":[";
  json_input += std::to_json(originloc);
  json_input += ",";
  json_input += std::to_json(destloc);
  json_input += "],\"costing\":\"auto\",";
  json_input += "\"directions_options\":{\"units\":\"miles\"}}'";
  json_input += " --config ../conf/valhalla.json";
  valhalla::midgard::logging::Log(json_input, " [JSON_INPUT] ");
#endif

  //parse the config
  boost::property_tree::ptree pt;
  boost::property_tree::read_json(config.c_str(), pt);

  //configure logging
  boost::optional<boost::property_tree::ptree&> logging_subtree = pt
      .get_child_optional("thor.logging");
  if (logging_subtree) {
    auto logging_config = valhalla::midgard::ToMap<
        const boost::property_tree::ptree&,
        std::unordered_map<std::string, std::string> >(logging_subtree.get());
    valhalla::midgard::logging::Configure(logging_config);
  }

  // Something to hold the statistics
  uint32_t n = locations.size() - 1;
  PathStatistics data({locations[0].latlng_.lat(), locations[0].latlng_.lng()},
                      {locations[n].latlng_.lat(), locations[n].latlng_.lng()});

  // Crow flies distance between locations (km)
  float d1 = 0.0f;
  for (uint32_t i = 0; i < n; i++) {
    d1 += locations[i].latlng_.Distance(locations[i+1].latlng_) * kKmPerMeter;
  }

  // Get something we can use to fetch tiles
  valhalla::baldr::GraphReader reader(pt.get_child("mjolnir"));

  auto t0 = std::chrono::high_resolution_clock::now();

  // Construct costing
  CostFactory<DynamicCost> factory;
  factory.Register("auto", CreateAutoCost);
  factory.Register("auto_shorter", CreateAutoShorterCost);
  factory.Register("bus", CreateBusCost);
  factory.Register("bicycle", CreateBicycleCost);
  factory.Register("pedestrian", CreatePedestrianCost);
  factory.Register("truck", CreateTruckCost);
  factory.Register("transit", CreateTransitCost);

  // Figure out the route type
  for (auto & c : routetype)
    c = std::tolower(c);
  LOG_INFO("routetype: " + routetype);

  // Get the costing method - pass the JSON configuration
  TripPath trip_path;
  TravelMode mode;
  std::shared_ptr<DynamicCost> mode_costing[4];
  if (routetype == "multimodal") {
    // Create array of costing methods per mode and set initial mode to
    // pedestrian
    mode_costing[0] = get_costing(factory, pt, json_ptree, "auto");
    mode_costing[1] = get_costing(factory, pt, json_ptree, "pedestrian");
    mode_costing[2] = get_costing(factory, pt, json_ptree, "bicycle");
    mode_costing[3] = get_costing(factory, pt, json_ptree, "transit");
    mode = TravelMode::kPedestrian;
  } else {
    // Assign costing method, override any config options that are in the
    // json request
    std::shared_ptr<DynamicCost> cost = get_costing(factory, pt,
                              json_ptree, routetype);
    mode = cost->travelmode();
    mode_costing[static_cast<uint32_t>(mode)] = cost;
  }

  // Find locations
  auto t1 = std::chrono::high_resolution_clock::now();
  std::shared_ptr<DynamicCost> cost = mode_costing[static_cast<uint32_t>(mode)];
  std::unordered_map<size_t, size_t> color_counts;

  std::vector<PathLocation> path_location;
  for (auto loc : locations) {
    try {
      path_location.push_back(Search(loc, reader, cost->GetEdgeFilter(), cost->GetNodeFilter()));
    } catch (...) {
      data.setSuccess("fail_invalid_origin");
      data.log();
      return EXIT_FAILURE;
    }
  }
  // If we are testing connectivity
  if (connectivity) {
    //TODO: get transit level for transit costing
    //TODO: if transit send a non zero radius
    connectivity_map_t connectivity_map(reader.GetTileHierarchy());
    auto colors = connectivity_map.get_colors(reader.GetTileHierarchy().levels().rbegin()->first, path_location.back(), 0);
    for(auto color : colors){
      auto itr = color_counts.find(color);
      if(itr == color_counts.cend())
        color_counts[color] = 1;
      else
        ++itr->second;
    }

    //are all the locations in the same color regions
    bool connected = false;
    for(const auto& c : color_counts) {
      if(c.second == locations.size()) {
        connected = true;
        break;
      }
    }
    if(!connected) {
      LOG_INFO("No tile connectivity between locations");
      data.setSuccess("fail_no_connectivity");
      data.log();
      return EXIT_FAILURE;
    }
  }
  auto t2 = std::chrono::high_resolution_clock::now();
  uint32_t msecs = std::chrono::duration_cast<std::chrono::milliseconds>(
                  t2 - t1).count();
  LOG_INFO("Location Processing took " + std::to_string(msecs) + " ms");

  // Get the route
  AStarPathAlgorithm astar;
  BidirectionalAStar bd;
  MultiModalPathAlgorithm mm;
  for (uint32_t i = 0; i < n; i++) {
    // Choose path algorithm
    PathAlgorithm* pathalgorithm;
    if (routetype == "multimodal") {
      pathalgorithm = &mm;
    } else if (routetype == "pedestrian") {
      pathalgorithm = &bd;
    } else {
      // Use bidirectional except for possible trivial cases
      pathalgorithm = &bd;
      for (auto& edge1 : path_location[i].edges) {
        for (auto& edge2 : path_location[i+1].edges) {
          if (edge1.id == edge2.id) {
            pathalgorithm = &astar;
          }
        }
      }
    }
    bool using_astar = (pathalgorithm == &astar);

    // Get the best path
    try {
      trip_path = PathTest(reader, path_location[i], path_location[i + 1],
                           pathalgorithm, mode_costing, mode, data, multi_run,
                           iterations, using_astar);
    } catch (std::runtime_error& rte) {
      LOG_ERROR("trip_path not found");
    }

    // If successful get directions
    if (trip_path.node().size() > 0) {
      // Try the the directions
      t1 = std::chrono::high_resolution_clock::now();
      TripDirections trip_directions = DirectionsTest(directions_options, trip_path,
                        locations[i], locations[i+1], data);
      t2 = std::chrono::high_resolution_clock::now();
      msecs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

      auto trip_time = trip_directions.summary().time();
      auto trip_length = trip_directions.summary().length() * 1609.344f;
      LOG_INFO("trip_processing_time (ms)::" + std::to_string(msecs));
      LOG_INFO("trip_time (secs)::" + std::to_string(trip_time));
      LOG_INFO("trip_length (meters)::" + std::to_string(trip_length));
      data.setSuccess("success");
    } else {
      // Check if origins are unreachable
      bool unreachable_origin = false;
      for (auto& edge : path_location[i].edges) {
        const GraphTile* tile = reader.GetGraphTile(edge.id);
        const DirectedEdge* directededge = tile->directededge(edge.id);
        std::unique_ptr<const EdgeInfo> ei = tile->edgeinfo(
                    directededge->edgeinfo_offset());
        if (directededge->unreachable()) {
          LOG_INFO("Origin edge is unconnected: wayid = " + std::to_string(ei->wayid()));
          unreachable_origin = true;
        }
        LOG_INFO("Origin wayId = " + std::to_string(ei->wayid()));
      }

      // Check if destinations are unreachable
      bool unreachable_dest = false;
      for (auto& edge : path_location[i+1].edges) {
        const GraphTile* tile = reader.GetGraphTile(edge.id);
        const DirectedEdge* directededge = tile->directededge(edge.id);
        std::unique_ptr<const EdgeInfo> ei = tile->edgeinfo(
                      directededge->edgeinfo_offset());
        if (directededge->unreachable()) {
          LOG_INFO("Destination edge is unconnected: wayid = " + std::to_string(ei->wayid()));
          unreachable_dest = true;
        }
        LOG_INFO("Destination wayId = " + std::to_string(ei->wayid()));
      }

      // Route was unsuccessful
      if (unreachable_origin && unreachable_dest) {
        data.setSuccess("fail_unreachable_locations");
      } else if (unreachable_origin) {
        data.setSuccess("fail_unreachable_origin");
      } else if (unreachable_dest) {
        data.setSuccess("fail_unreachable_dest");
      } else {
        data.setSuccess("fail_no_route");
      }
    }
  }

  // Set the arc distance. Convert to miles if needed
  if (directions_options.units() == DirectionsOptions::Units::DirectionsOptions_Units_kMiles) {
    d1 *= kMilePerKm;
  }
  data.setArcDist(d1);

  // Time all stages for the stats file: location processing,
  // path computation, trip path building, and directions
  t2 = std::chrono::high_resolution_clock::now();
  msecs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();
  LOG_INFO("Total time= " + std::to_string(msecs) + " ms");
  data.addRuntime(msecs);
  data.log();

  return EXIT_SUCCESS;
}

