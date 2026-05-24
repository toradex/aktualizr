#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ini_parser.hpp>

#include "libaktualizr/config.h"
#include "utilities/config_utils.h"

namespace {
// Parse a space-separated string into a vector of strings.
// Systemd unit names cannot contain spaces, so space is a safe delimiter.
std::vector<std::string> ParseSpaceSeparatedList(const std::string& input) {
  std::vector<std::string> result;
  if (!input.empty()) {
    boost::split(result, input, boost::is_any_of(" "), boost::token_compress_on);
    // Remove any empty strings that might result from multiple spaces
    result.erase(std::remove_if(result.begin(), result.end(), [](const std::string& s) { return s.empty(); }),
                 result.end());
  }
  return result;
}

// Join a vector of strings with spaces.
std::string JoinWithSpaces(const std::vector<std::string>& vec) { return boost::algorithm::join(vec, " "); }
}  // namespace

void LoggerConfig::updateFromPropertyTree(const boost::property_tree::ptree& pt) {
  CopyFromConfig(loglevel, "loglevel", pt);
  CopyFromConfig(offline_logs_enabled, "offline_logs_enabled", pt);
  CopyFromConfig(offline_logs_file, "offline_logs_file", pt);
  CopyFromConfig(online_logs_enabled, "online_logs_enabled", pt);

  // Handle capture_services as a space-separated string
  boost::optional<std::string> services_str = pt.get_optional<std::string>("capture_services");
  if (services_str.is_initialized()) {
    capture_services = ParseSpaceSeparatedList(StripQuotesFromStrings(services_str.get()));
  }
}

void LoggerConfig::writeToStream(std::ostream& out_stream) const {
  writeOption(out_stream, loglevel, "loglevel");
  writeOption(out_stream, offline_logs_enabled, "offline_logs_enabled");
  writeOption(out_stream, offline_logs_file, "offline_logs_file");
  writeOption(out_stream, JoinWithSpaces(capture_services), "capture_services");
  writeOption(out_stream, online_logs_enabled, "online_logs_enabled");
}
