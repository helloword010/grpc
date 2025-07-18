//
// Copyright 2019 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "src/core/xds/grpc/xds_server_grpc.h"

#include <stdlib.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "src/core/config/core_configuration.h"
#include "src/core/util/down_cast.h"
#include "src/core/util/env.h"
#include "src/core/util/json/json_reader.h"
#include "src/core/util/json/json_writer.h"

namespace grpc_core {

namespace {

constexpr absl::string_view kServerFeatureIgnoreResourceDeletion =
    "ignore_resource_deletion";

constexpr absl::string_view kServerFeatureFailOnDataErrors =
    "fail_on_data_errors";

constexpr absl::string_view kServerFeatureResourceTimerIsTransientFailure =
    "resource_timer_is_transient_error";

constexpr absl::string_view kServerFeatureTrustedXdsServer =
    "trusted_xds_server";

}  // namespace

bool GrpcXdsServer::IgnoreResourceDeletion() const {
  return server_features_.find(std::string(
             kServerFeatureIgnoreResourceDeletion)) != server_features_.end();
}

bool GrpcXdsServer::FailOnDataErrors() const {
  return server_features_.find(std::string(kServerFeatureFailOnDataErrors)) !=
         server_features_.end();
}

bool GrpcXdsServer::ResourceTimerIsTransientFailure() const {
  return server_features_.find(
             std::string(kServerFeatureResourceTimerIsTransientFailure)) !=
         server_features_.end();
}

bool GrpcXdsServer::TrustedXdsServer() const {
  return server_features_.find(std::string(kServerFeatureTrustedXdsServer)) !=
         server_features_.end();
}

bool GrpcXdsServer::Equals(const XdsServer& other) const {
  const auto& o = DownCast<const GrpcXdsServer&>(other);
  return (server_target_->Equals(*o.server_target_) &&
          server_features_ == o.server_features_);
}

const JsonLoaderInterface* GrpcXdsServer::JsonLoader(const JsonArgs&) {
  static const auto* loader = JsonObjectLoader<GrpcXdsServer>().Finish();
  return loader;
}

namespace {

struct ChannelOrCallCreds {
  std::string type;
  Json::Object config;

  static const JsonLoaderInterface* JsonLoader(const JsonArgs&) {
    static const auto* loader =
        JsonObjectLoader<ChannelOrCallCreds>()
            .Field("type", &ChannelOrCallCreds::type)
            .OptionalField("config", &ChannelOrCallCreds::config)
            .Finish();
    return loader;
  }
};

// TODO(roth): Remove this guard once we have reports from OSS Istio
// users that this works properly.
bool XdsBootstrapCallCredsEnabled() {
  auto value = GetEnv("GRPC_EXPERIMENTAL_XDS_BOOTSTRAP_CALL_CREDS");
  if (!value.has_value()) return false;
  bool parsed_value;
  bool parse_succeeded = gpr_parse_bool_value(value->c_str(), &parsed_value);
  return parse_succeeded && parsed_value;
}

}  // namespace

void GrpcXdsServer::JsonPostLoad(const Json& json, const JsonArgs& args,
                                 ValidationErrors* errors) {
  // Parse "channel_creds".
  RefCountedPtr<ChannelCredsConfig> channel_creds_config;
  {
    auto channel_creds_list =
        LoadJsonObjectField<std::vector<ChannelOrCallCreds>>(
            json.object(), args, "channel_creds", errors);
    if (channel_creds_list.has_value()) {
      ValidationErrors::ScopedField field(errors, ".channel_creds");
      for (size_t i = 0; i < channel_creds_list->size(); ++i) {
        ValidationErrors::ScopedField field(errors, absl::StrCat("[", i, "]"));
        auto& creds = (*channel_creds_list)[i];
        // Select the first channel creds type that we support, but
        // validate all entries.
        if (CoreConfiguration::Get().channel_creds_registry().IsSupported(
                creds.type)) {
          ValidationErrors::ScopedField field(errors, ".config");
          auto config =
              CoreConfiguration::Get().channel_creds_registry().ParseConfig(
                  creds.type, Json::FromObject(creds.config), args, errors);
          if (channel_creds_config == nullptr) {
            channel_creds_config = std::move(config);
          }
        }
      }
      if (channel_creds_config == nullptr) {
        errors->AddError("no known creds type found");
      }
    }
  }
  // Parse "call_creds".
  std::vector<RefCountedPtr<CallCredsConfig>> call_creds_configs;
  if (XdsBootstrapCallCredsEnabled()) {
    auto call_creds_list = LoadJsonObjectField<std::vector<ChannelOrCallCreds>>(
        json.object(), args, "call_creds", errors);
    if (call_creds_list.has_value()) {
      ValidationErrors::ScopedField field(errors, ".call_creds");
      for (size_t i = 0; i < call_creds_list->size(); ++i) {
        ValidationErrors::ScopedField field(errors, absl::StrCat("[", i, "]"));
        auto& creds = (*call_creds_list)[i];
        if (CoreConfiguration::Get().call_creds_registry().IsSupported(
                creds.type)) {
          ValidationErrors::ScopedField field(errors, ".config");
          call_creds_configs.push_back(
              CoreConfiguration::Get().call_creds_registry().ParseConfig(
                  creds.type, Json::FromObject(creds.config), args, errors));
        }
      }
    }
  }
  // Parse "server_features".
  {
    ValidationErrors::ScopedField field(errors, ".server_features");
    auto it = json.object().find("server_features");
    if (it != json.object().end()) {
      if (it->second.type() != Json::Type::kArray) {
        errors->AddError("is not an array");
      } else {
        const Json::Array& array = it->second.array();
        for (const Json& feature_json : array) {
          if (feature_json.type() == Json::Type::kString &&
              (feature_json.string() == kServerFeatureIgnoreResourceDeletion ||
               feature_json.string() == kServerFeatureFailOnDataErrors ||
               feature_json.string() ==
                   kServerFeatureResourceTimerIsTransientFailure ||
               feature_json.string() == kServerFeatureTrustedXdsServer)) {
            server_features_.insert(feature_json.string());
          }
        }
      }
    }
  }
  // Parse "server_uri".
  std::string server_uri_target = LoadJsonObjectField<std::string>(
                                      json.object(), args, "server_uri", errors)
                                      .value_or("");
  server_target_ = std::make_shared<GrpcXdsServerTarget>(
      std::move(server_uri_target), std::move(channel_creds_config),
      std::move(call_creds_configs));
}

std::string GrpcXdsServer::Key() const {
  std::vector<std::string> parts;
  parts.push_back("{");
  parts.push_back(absl::StrCat("target=", server_target_->Key()));
  if (!server_features_.empty()) {
    parts.push_back(absl::StrCat("server_features=[",
                                 absl::StrJoin(server_features_, ","), "]"));
  }
  parts.push_back("}");
  return absl::StrJoin(parts, ",");
}

std::string GrpcXdsServerTarget::Key() const {
  std::vector<std::string> parts;
  parts.push_back("{");
  parts.push_back(absl::StrCat("server_uri=", server_uri_));
  if (channel_creds_config_ != nullptr) {
    parts.push_back(
        absl::StrCat("channel_creds={type=", channel_creds_config_->type(),
                     ", config=", channel_creds_config_->ToString(), "}"));
  }
  for (const auto& call_creds_config : call_creds_configs_) {
    parts.push_back(absl::StrCat("call_creds={type=", call_creds_config->type(),
                                 ", config=", call_creds_config->ToString(),
                                 "}"));
  }
  parts.push_back("}");
  return absl::StrJoin(parts, ",");
}

bool GrpcXdsServerTarget::Equals(const XdsServerTarget& other) const {
  const auto& o = DownCast<const GrpcXdsServerTarget&>(other);
  if (server_uri_ != o.server_uri_) return false;
  if (channel_creds_config_->type() != o.channel_creds_config_->type() ||
      !channel_creds_config_->Equals(*o.channel_creds_config_)) {
    return false;
  }
  if (call_creds_configs_.size() != o.call_creds_configs_.size()) {
    return false;
  }
  for (size_t i = 0; i < call_creds_configs_.size(); ++i) {
    if (call_creds_configs_[i]->type() != o.call_creds_configs_[i]->type() ||
        !call_creds_configs_[i]->Equals(*o.call_creds_configs_[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace grpc_core
