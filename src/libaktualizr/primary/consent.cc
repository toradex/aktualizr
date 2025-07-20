#include "primary/consent.h"
#include <json/json.h>
#include <boost/algorithm/string/case_conv.hpp>

Json::Value Consent::TargetsToJson(const std::vector<Uptane::Target> &targets) {
  Json::Value res;
  res["_type"] = "Targets";

  Json::Value tgts;
  for (const auto &target : targets) {
    Json::Value tgt;
    tgt["custom"] = target.custom_data();
    tgt["length"] = Json::Value(static_cast<Json::Value::Int64>(target.length()));
    for (const auto &hash : target.hashes()) {
      tgt["hashes"][hash.TypeString()] = boost::algorithm::to_lower_copy(hash.HashString());
    }

    tgts[target.filename()] = std::move(tgt);
  }
  res["targets"] = std::move(tgts);
  return res;
}
