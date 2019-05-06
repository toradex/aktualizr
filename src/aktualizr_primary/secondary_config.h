#ifndef SECONDARY_CONFIG_H_
#define SECONDARY_CONFIG_H_

#include <json/json.h>
#include <boost/filesystem.hpp>
#include <unordered_map>

namespace Primary {

class SecondaryConfig {
 public:
  SecondaryConfig(const char* type) : type_(type) {}
  virtual const char* type() const { return type_; }
  virtual ~SecondaryConfig() = default;

 private:
  const char* const type_;
};

class IPSecondaryConfig {
 public:
  static constexpr const char* const AddrField{"addr"};

  IPSecondaryConfig(std::string addr_ip, uint16_t addr_port) : ip(std::move(addr_ip)), port(addr_port) {}

  friend std::ostream& operator<<(std::ostream& os, const IPSecondaryConfig& cfg) {
    os << "(addr: " << cfg.ip << ":" << cfg.port << ")";
    return os;
  }

 public:
  const std::string ip;
  const uint16_t port;
};

class IPSecondariesConfig : public SecondaryConfig {
 public:
  static const char* const Type;
  static constexpr const char* const PortField{"secondaries_wait_port"};
  static constexpr const char* const TimeoutField{"secondaries_wait_timeout"};
  static constexpr const char* const SecondariesField{"secondaries"};

  IPSecondariesConfig(uint16_t wait_port, size_t wait_timeout)
      : SecondaryConfig(Type), secondaries_wait_port{wait_port}, secondaries_wait_timeout{wait_timeout} {}

  friend std::ostream& operator<<(std::ostream& os, const IPSecondariesConfig& cfg) {
    os << "(wait_port: " << cfg.secondaries_wait_port << " wait_timeout: " << cfg.secondaries_wait_timeout << ")";
    return os;
  }

 public:
  const uint16_t secondaries_wait_port;
  const size_t secondaries_wait_timeout;
  std::vector<IPSecondaryConfig> secondaries_cfg;
};

class SecondaryConfigParser {
 public:
  using Configs = std::vector<std::shared_ptr<SecondaryConfig>>;

  static Configs parse_config_file(const boost::filesystem::path& config_file);
  virtual ~SecondaryConfigParser() = default;

  // TODO implement iterator instead of parse
  virtual Configs parse() = 0;
};

class JsonConfigParser : public SecondaryConfigParser {
 public:
  JsonConfigParser(const boost::filesystem::path& config_file);

  Configs parse() override;

 private:
  static void createIPSecondariesCfg(Configs& configs, Json::Value& json_ip_sec_cfg);
  // add here a factory method for another type of secondary config

 private:
  using SecondaryConfigFactoryRegistry = std::unordered_map<std::string, std::function<void(Configs&, Json::Value&)>>;

  SecondaryConfigFactoryRegistry sec_cfg_factory_registry_ = {
      {IPSecondariesConfig::Type, createIPSecondariesCfg}
      // add here factory method for another type of secondary config
  };

  Json::Value root_;
};

}  // namespace Primary

#endif  // SECONDARY_CONFIG_H_
