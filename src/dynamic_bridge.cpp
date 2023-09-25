// Copyright 2015 Open Source Robotics Foundation, Inc.
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

#include <boost/algorithm/string/predicate.hpp>

#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ros1_bridge/helper.hpp"
#include "ros1_bridge/action_factory.hpp"

std::mutex g_bridge_mutex;

bool parse_command_options(
  int argc, char ** argv, bool & output_topic_introspection,
  bool & bridge_all_1to2_topics, bool & bridge_all_2to1_topics,
  bool & multi_threads)
{
  std::vector<std::string> args(argv, argv + argc);

  if (ros1_bridge::find_command_option(args, "-h") ||
    ros1_bridge::find_command_option(args, "--help"))
  {
    std::stringstream ss;
    ss << "Usage:" << std::endl;
    ss << " -h, --help: This message." << std::endl;
    ss << " --show-introspection: Print output of introspection of both sides of the bridge.";
    ss << std::endl;
    ss << " --print-pairs: Print a list of the supported ROS 2 <=> ROS 1 conversion pairs.";
    ss << std::endl;
    ss << " --bridge-all-topics: Bridge all topics in both directions, whether or not there is ";
    ss << "a matching subscriber." << std::endl;
    ss << " --bridge-all-1to2-topics: Bridge all ROS 1 topics to ROS 2, whether or not there is ";
    ss << "a matching subscriber." << std::endl;
    ss << " --bridge-all-2to1-topics: Bridge all ROS 2 topics to ROS 1, whether or not there is ";
    ss << "a matching subscriber." << std::endl;
    ss << " --multi-threads: Bridge with multiple threads for spinner of ROS 1 and ROS 2.";
    ss << std::endl;
    std::cout << ss.str();
    return false;
  }

  if (ros1_bridge::get_flag_option(args, "--print-pairs")) {
    auto mappings_2to1 = ros1_bridge::get_all_message_mappings_2to1();
    if (mappings_2to1.size() > 0) {
      printf("Supported ROS 2 <=> ROS 1 message type conversion pairs:\n");
      for (auto & pair : mappings_2to1) {
        printf("  - '%s' (ROS 2) <=> '%s' (ROS 1)\n", pair.first.c_str(), pair.second.c_str());
      }
    } else {
      printf("No message type conversion pairs supported.\n");
    }
    mappings_2to1 = ros1_bridge::get_all_service_mappings_2to1();
    if (mappings_2to1.size() > 0) {
      printf("Supported ROS 2 <=> ROS 1 service type conversion pairs:\n");
      for (auto & pair : mappings_2to1) {
        printf("  - '%s' (ROS 2) <=> '%s' (ROS 1)\n", pair.first.c_str(), pair.second.c_str());
      }
    } else {
      printf("No service type conversion pairs supported.\n");
    }
    mappings_2to1 = ros1_bridge::get_all_action_mappings_2to1();
    if (mappings_2to1.size() > 0) {
      printf("Supported ROS 2 <=> ROS 1 action type conversion pairs:\n");
      for (auto & pair : mappings_2to1) {
        printf("  - '%s' (ROS 2) <=> '%s' (ROS 1)\n", pair.first.c_str(), pair.second.c_str());
      }
    } else {
      printf("No action type conversion pairs supported.\n");
    }
    return false;
  }

  output_topic_introspection = ros1_bridge::get_flag_option(args, "--show-introspection");

  bool bridge_all_topics = ros1_bridge::get_flag_option(args, "--bridge-all-topics");
  bridge_all_1to2_topics = bridge_all_topics ||
    ros1_bridge::get_flag_option(args, "--bridge-all-1to2-topics");
  bridge_all_2to1_topics = bridge_all_topics ||
    ros1_bridge::get_flag_option(args, "--bridge-all-2to1-topics");
  multi_threads = ros1_bridge::get_flag_option(args, "--multi-threads");

  return true;
}

void update_bridge(
  ros::NodeHandle & ros1_node,
  rclcpp::Node::SharedPtr ros2_node,
  const std::map<std::string, std::string> & ros1_publishers,
  const std::map<std::string, std::string> & ros1_subscribers,
  const std::map<std::string, std::string> & ros2_publishers,
  const std::map<std::string, std::string> & ros2_subscribers,
  const std::map<std::string, std::map<std::string, std::string>> & ros1_services,
  const std::map<std::string, std::map<std::string, std::string>> & ros2_services,
  const std::map<std::string, std::map<std::string, std::string>> & ros1_action_servers,
  const std::map<std::string, std::map<std::string, std::string>> & ros2_action_servers,
  std::map<std::string, ros1_bridge::Bridge1to2HandlesAndMessageTypes> & bridges_1to2,
  std::map<std::string, ros1_bridge::Bridge2to1HandlesAndMessageTypes> & bridges_2to1,
  std::map<std::string, ros1_bridge::ServiceBridge1to2> & service_bridges_1_to_2,
  std::map<std::string, ros1_bridge::ServiceBridge2to1> & service_bridges_2_to_1,
  std::map<std::string,
  std::unique_ptr<ros1_bridge::ActionFactoryInterface>> & action_bridges_1_to_2,
  std::map<std::string,
  std::unique_ptr<ros1_bridge::ActionFactoryInterface>> & action_bridges_2_to_1,
  bool bridge_all_1to2_topics, bool bridge_all_2to1_topics,
  bool multi_threads = false)
{
  std::lock_guard<std::mutex> lock(g_bridge_mutex);

  // create 1to2 bridges
  for (auto ros1_publisher : ros1_publishers) {
    // identify topics available as ROS 1 publishers as well as ROS 2 subscribers
    auto topic_name = ros1_publisher.first;
    std::string ros1_type_name = ros1_publisher.second;
    std::string ros2_type_name;

    auto ros2_subscriber = ros2_subscribers.find(topic_name);
    if (ros2_subscriber == ros2_subscribers.end()) {
      if (!bridge_all_1to2_topics) {
        continue;
      }
      // update the ROS 2 type name to be that of the anticipated bridged type
      // TODO(dhood): support non 1-1 "bridge-all" mappings
      bool mapping_found = ros1_bridge::get_1to2_mapping(ros1_type_name, ros2_type_name);
      if (!mapping_found) {
        // printf("No known mapping for ROS 1 type '%s'\n", ros1_type_name.c_str());
        continue;
      }
      // printf("topic name '%s' has ROS 2 publishers\n", topic_name.c_str());
    } else {
      ros2_type_name = ros2_subscriber->second;
      // printf("topic name '%s' has ROS 1 publishers and ROS 2 subscribers\n", topic_name.c_str());
    }

    // check if 1to2 bridge for the topic exists
    if (bridges_1to2.find(topic_name) != bridges_1to2.end()) {
      auto bridge = bridges_1to2.find(topic_name)->second;
      if (bridge.ros1_type_name == ros1_type_name && bridge.ros2_type_name == ros2_type_name) {
        // skip if bridge with correct types is already in place
        continue;
      }
      // remove existing bridge with previous types
      bridges_1to2.erase(topic_name);
      printf("replace 1to2 bridge for topic '%s'\n", topic_name.c_str());
    }

    ros1_bridge::Bridge1to2HandlesAndMessageTypes bridge;
    bridge.ros1_type_name = ros1_type_name;
    bridge.ros2_type_name = ros2_type_name;

    auto ros2_publisher_qos = rclcpp::QoS(rclcpp::KeepLast(10));
    if (topic_name == "/tf_static") {
      ros2_publisher_qos.keep_all();
      ros2_publisher_qos.transient_local();
    }
    try {
      bridge.bridge_handles = ros1_bridge::create_bridge_from_1_to_2(
        ros1_node, ros2_node,
        bridge.ros1_type_name, topic_name, 10,
        bridge.ros2_type_name, topic_name, ros2_publisher_qos);
    } catch (std::runtime_error & e) {
      fprintf(
        stderr,
        "failed to create 1to2 bridge for topic '%s' "
        "with ROS 1 type '%s' and ROS 2 type '%s': %s\n",
        topic_name.c_str(), bridge.ros1_type_name.c_str(), bridge.ros2_type_name.c_str(), e.what());
      if (std::string(e.what()).find("No template specialization") != std::string::npos) {
        fprintf(stderr, "check the list of supported pairs with the `--print-pairs` option\n");
      }
      continue;
    }

    bridges_1to2[topic_name] = bridge;
    printf(
      "created 1to2 bridge for topic '%s' with ROS 1 type '%s' and ROS 2 type '%s'\n",
      topic_name.c_str(), bridge.ros1_type_name.c_str(), bridge.ros2_type_name.c_str());
  }

  // create 2to1 bridges
  for (auto ros2_publisher : ros2_publishers) {
    // identify topics available as ROS 1 subscribers as well as ROS 2 publishers
    auto topic_name = ros2_publisher.first;
    std::string ros2_type_name = ros2_publisher.second;
    std::string ros1_type_name;

    auto ros1_subscriber = ros1_subscribers.find(topic_name);
    if (ros1_subscriber == ros1_subscribers.end()) {
      if (!bridge_all_2to1_topics) {
        continue;
      }
      // update the ROS 1 type name to be that of the anticipated bridged type
      // TODO(dhood): support non 1-1 "bridge-all" mappings
      bool mapping_found = ros1_bridge::get_2to1_mapping(ros2_type_name, ros1_type_name);
      if (!mapping_found) {
        // printf("No known mapping for ROS 2 type '%s'\n", ros2_type_name.c_str());
        continue;
      }
      // printf("topic name '%s' has ROS 2 publishers\n", topic_name.c_str());
    } else {
      ros1_type_name = ros1_subscriber->second;
      // printf("topic name '%s' has ROS 1 subscribers and ROS 2 publishers\n", topic_name.c_str());
    }

    // check if 2to1 bridge for the topic exists
    if (bridges_2to1.find(topic_name) != bridges_2to1.end()) {
      auto bridge = bridges_2to1.find(topic_name)->second;
      if ((bridge.ros1_type_name == ros1_type_name || bridge.ros1_type_name == "") &&
        bridge.ros2_type_name == ros2_type_name)
      {
        // skip if bridge with correct types is already in place
        continue;
      }
      // remove existing bridge with previous types
      bridges_2to1.erase(topic_name);
      printf("replace 2to1 bridge for topic '%s'\n", topic_name.c_str());
    }

    ros1_bridge::Bridge2to1HandlesAndMessageTypes bridge;
    bridge.ros1_type_name = ros1_type_name;
    bridge.ros2_type_name = ros2_type_name;

    try {
      bridge.bridge_handles = ros1_bridge::create_bridge_from_2_to_1(
        ros2_node, ros1_node,
        bridge.ros2_type_name, topic_name, 10,
        bridge.ros1_type_name, topic_name, 10,
        nullptr,
        multi_threads);
    } catch (std::runtime_error & e) {
      fprintf(
        stderr,
        "failed to create 2to1 bridge for topic '%s' "
        "with ROS 2 type '%s' and ROS 1 type '%s': %s\n",
        topic_name.c_str(), bridge.ros2_type_name.c_str(), bridge.ros1_type_name.c_str(), e.what());
      if (std::string(e.what()).find("No template specialization") != std::string::npos) {
        fprintf(stderr, "check the list of supported pairs with the `--print-pairs` option\n");
      }
      continue;
    }

    bridges_2to1[topic_name] = bridge;
    printf(
      "created 2to1 bridge for topic '%s' with ROS 2 type '%s' and ROS 1 type '%s'\n",
      topic_name.c_str(), bridge.ros2_type_name.c_str(), bridge.ros1_type_name.c_str());
  }

  // remove obsolete bridges
  std::vector<std::string> to_be_removed_1to2;
  for (auto it : bridges_1to2) {
    std::string topic_name = it.first;
    if (
      ros1_publishers.find(topic_name) == ros1_publishers.end() ||
      (!bridge_all_1to2_topics && ros2_subscribers.find(topic_name) == ros2_subscribers.end()))
    {
      to_be_removed_1to2.push_back(topic_name);
    }
  }
  for (auto topic_name : to_be_removed_1to2) {
    bridges_1to2.erase(topic_name);
    printf("removed 1to2 bridge for topic '%s'\n", topic_name.c_str());
  }

  std::vector<std::string> to_be_removed_2to1;
  for (auto it : bridges_2to1) {
    std::string topic_name = it.first;
    if (
      (!bridge_all_2to1_topics && ros1_subscribers.find(topic_name) == ros1_subscribers.end()) ||
      ros2_publishers.find(topic_name) == ros2_publishers.end())
    {
      to_be_removed_2to1.push_back(topic_name);
    }
  }
  for (auto topic_name : to_be_removed_2to1) {
    bridges_2to1.erase(topic_name);
    printf("removed 2to1 bridge for topic '%s'\n", topic_name.c_str());
  }

  // create bridges for ros1 services
  for (auto & service : ros1_services) {
    auto & name = service.first;
    auto & details = service.second;
    if (
      service_bridges_2_to_1.find(name) == service_bridges_2_to_1.end() &&
      service_bridges_1_to_2.find(name) == service_bridges_1_to_2.end())
    {
      auto factory = ros1_bridge::get_service_factory(
        "ros1", details.at("package"), details.at("name"));
      if (factory) {
        try {
          service_bridges_2_to_1[name] = factory->service_bridge_2_to_1(
            ros1_node, ros2_node, name, multi_threads);
          printf("Created 2 to 1 bridge for service %s\n", name.data());
        } catch (std::runtime_error & e) {
          fprintf(stderr, "Failed to created a bridge: %s\n", e.what());
        }
      }
    }
  }

  int service_execution_timeout{5};
  ros1_node.getParamCached(
    "ros1_bridge/dynamic_bridge/service_execution_timeout", service_execution_timeout);

  // create bridges for ros2 services
  for (auto & service : ros2_services) {
    auto & name = service.first;
    auto & details = service.second;
    if (
      service_bridges_1_to_2.find(name) == service_bridges_1_to_2.end() &&
      service_bridges_2_to_1.find(name) == service_bridges_2_to_1.end())
    {
      auto factory = ros1_bridge::get_service_factory(
        "ros2", details.at("package"), details.at("name"));
      if (factory) {
        try {
          service_bridges_1_to_2[name] = factory->service_bridge_1_to_2(
            ros1_node, ros2_node, name, service_execution_timeout, multi_threads);
          printf("Created 1 to 2 bridge for service %s\n", name.data());
        } catch (std::runtime_error & e) {
          fprintf(stderr, "Failed to created a bridge: %s\n", e.what());
        }
      }
    }
  }

  // remove obsolete ros1 services
  for (auto it = service_bridges_2_to_1.begin(); it != service_bridges_2_to_1.end(); ) {
    if (ros1_services.find(it->first) == ros1_services.end()) {
      printf("Removed 2 to 1 bridge for service %s\n", it->first.data());
      try {
        it = service_bridges_2_to_1.erase(it);
      } catch (std::runtime_error & e) {
        fprintf(stderr, "There was an error while removing 2 to 1 bridge: %s\n", e.what());
      }
    } else {
      ++it;
    }
  }

  // remove obsolete ros2 services
  for (auto it = service_bridges_1_to_2.begin(); it != service_bridges_1_to_2.end(); ) {
    if (ros2_services.find(it->first) == ros2_services.end()) {
      printf("Removed 1 to 2 bridge for service %s\n", it->first.data());
      try {
        it->second.server.shutdown();
        it = service_bridges_1_to_2.erase(it);
      } catch (std::runtime_error & e) {
        fprintf(stderr, "There was an error while removing 1 to 2 bridge: %s\n", e.what());
      }
    } else {
      ++it;
    }
  }

  // create bridges for ros1 actions
  for (auto & ros1_action : ros1_action_servers) {
    auto & name = ros1_action.first;
    auto & details = ros1_action.second;
    if (
      action_bridges_1_to_2.find(name) == action_bridges_1_to_2.end() &&
      action_bridges_2_to_1.find(name) == action_bridges_2_to_1.end())
    {
      auto factory = ros1_bridge::get_action_factory(
        "ros1", details.at("package"), details.at("type"));
      if (factory) {
        try {
          factory->create_server_client(ros1_node, ros2_node, name);
          action_bridges_2_to_1[name] = std::move(factory);
          printf("Created 2 to 1 bridge for action %s\n", name.data());
        } catch (std::runtime_error & e) {
          fprintf(stderr, "Failed to created a bridge: %s\n", e.what());
        }
      }
    }
  }

  // create bridges for ros2 actions
  for (auto & ros2_action : ros2_action_servers) {
    auto & name = ros2_action.first;
    auto & details = ros2_action.second;
    if (
      action_bridges_1_to_2.find(name) == action_bridges_1_to_2.end() &&
      action_bridges_2_to_1.find(name) == action_bridges_2_to_1.end())
    {
      auto factory = ros1_bridge::get_action_factory(
        "ros2", details.at("package"), details.at("type"));
      if (factory) {
        try {
          factory->create_server_client(ros1_node, ros2_node, name);
          action_bridges_1_to_2[name] = std::move(factory);
          printf("Created 1 to 2 bridge for action %s\n", name.data());
        } catch (std::runtime_error & e) {
          fprintf(stderr, "Failed to created a bridge: %s\n", e.what());
        }
      }
    }
  }

  // remove obsolete ros1 actions
  for (auto it = action_bridges_2_to_1.begin(); it != action_bridges_2_to_1.end(); ) {
    if (ros1_action_servers.find(it->first) == ros1_action_servers.end()) {
      printf("Removed 2 to 1 bridge for action %s\n", it->first.data());
      try {
        it->second->shutdown();
        it->second.reset();
        it = action_bridges_2_to_1.erase(it);
      } catch (std::runtime_error & e) {
        fprintf(stderr, "There was an error while removing 2 to 1 bridge: %s\n", e.what());
      }
    } else {
      ++it;
    }
  }

  // remove obsolete ros2 actions
  for (auto it = action_bridges_1_to_2.begin(); it != action_bridges_1_to_2.end(); ) {
    if (ros2_action_servers.find(it->first) == ros2_action_servers.end()) {
      printf("Removed 1 to 2 bridge for action %s\n", it->first.data());
      try {
        // it->second.server.shutdown();
        it->second->shutdown();
        it->second.reset();
        it = action_bridges_1_to_2.erase(it);
      } catch (std::runtime_error & e) {
        fprintf(stderr, "There was an error while removing 1 to 2 bridge: %s\n", e.what());
      }
    } else {
      ++it;
    }
  }
}

inline bool is_action_topic(
  std::map<std::string, std::map<std::string, std::string>> & actions,
  std::map<std::string, uint8_t> & action_nums, const bool is_action_type,
  const std::string topic_name, const std::string topic_name_ends_with,
  const std::string type, const std::string type_ends_with, bool is_ros2 = false)
{
  // check if the topic name and topic types are as expected
  if (boost::algorithm::ends_with(topic_name.c_str(), topic_name_ends_with.c_str()) &&
    boost::algorithm::ends_with(type.c_str(), type_ends_with.c_str()))
  {
    // extract action name from topic name
    std::string name = topic_name.substr(0, topic_name.find(topic_name_ends_with.c_str()));
    if (actions.find(name) == actions.end()) {
      actions[name]["package"] = "";
      actions[name]["type"] = "";
      action_nums[name] = 0;
    }

    // e.g.: topic type of '/fibonacci/goal' is 'actionlib_tutorials/FibonacciActionGoal'
    // Thus, package name is action type is 'actionlib_tutorials' and
    // action type is 'Fibonacci'
    if (!type.empty() && is_action_type) {
      std::string pkg_name = type.substr(0, type.find("/"));
      std::string action_type =
        type.substr(
        type.find_last_of("/") + 1,
        type.length() - (type.find_last_of("/") + type_ends_with.length() + 1));
      actions[name]["package"] = pkg_name;
      if (is_ros2) {
        actions[name]["type"] = "action/" + action_type;
      } else {
        actions[name]["type"] = action_type;
      }
    }

    action_nums[name] += 1;

    return true;
  }
  return false;
}

// if topics 'goal' with type 'ActionGoal' and 'cancel' with type 'GoalID' are pubs, then it is an
// action client
// equivalent ROS2 action pkg and type can be retrieved from get_mappings.cpp
void get_active_ros1_actions(
  std::map<std::string, std::string> publishers,
  std::map<std::string, std::string> subscribers,
  std::map<std::string, std::map<std::string, std::string>> & active_ros1_action_servers,
  std::map<std::string, std::map<std::string, std::string>> & active_ros1_action_clients)
{
  // check if the topics end with 'goal', 'result', 'cancel', 'status'

  // find topics that end with goal and cancel, find corresponding result, status and feedback
  // in the other map
  std::map<std::string, std::string>::iterator it;
  std::map<std::string, uint8_t>::iterator it_num;
  // store count of pubs and subs for each action
  std::map<std::string, uint8_t> action_server_nums, action_client_nums;

  for (it = publishers.begin(); it != publishers.end(); it++) {
    // check for action client
    if (
      is_action_topic(
        active_ros1_action_clients, action_client_nums, false,
        it->first.c_str(), "/cancel", it->second.c_str(), "/GoalID"))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros1_action_clients, action_client_nums, true,
        it->first.c_str(), "/goal", it->second.c_str(), "ActionGoal"))
    {
      continue;
    } else if (   // NOLINT // check for action server
      is_action_topic(
        active_ros1_action_servers, action_server_nums, true,
        it->first.c_str(), "/feedback", it->second.c_str(),
        "ActionFeedback"))
    {
      continue;
    }
    if (
      is_action_topic(
        active_ros1_action_servers, action_server_nums, false,
        it->first.c_str(), "/result", it->second.c_str(), "ActionResult"))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros1_action_servers, action_server_nums, false,
        it->first.c_str(), "/status", it->second.c_str(),
        "GoalStatusArray"))
    {
      continue;
    }
  }

  // subscribers do not report their types, but use it to confirm action
  for (it = subscribers.begin(); it != subscribers.end(); it++) {
    // check for action server
    if (
      is_action_topic(
        active_ros1_action_servers, action_server_nums, false,
        it->first.c_str(), "/cancel", it->second.c_str(), ""))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros1_action_servers, action_server_nums, false,
        it->first.c_str(), "/goal", it->second.c_str(), ""))
    {
      continue;
    } else if (   // NOLINT   // check for action client
      is_action_topic(
        active_ros1_action_clients, action_client_nums, false,
        it->first.c_str(), "/feedback", it->second.c_str(), ""))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros1_action_clients, action_client_nums, false,
        it->first.c_str(), "/result", it->second.c_str(), ""))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros1_action_clients, action_client_nums, false,
        it->first.c_str(), "/status", it->second.c_str(), ""))
    {
      continue;
    }
  }

  for (it_num = action_client_nums.begin(); it_num != action_client_nums.end(); it_num++) {
    if (it_num->second != 5) {
      active_ros1_action_clients.erase(it_num->first);
    }
  }
  for (it_num = action_server_nums.begin(); it_num != action_server_nums.end(); it_num++) {
    if (it_num->second != 5) {
      active_ros1_action_servers.erase(it_num->first);
    }
  }
}

// how does ros2 action list determine active interfaces?
// ref: opt/ros/foxy/lib/python3.8/site-packages/ros2action/verb/list.py
// https://github.com/ros2/rcl/blob/master/rcl_action/src/rcl_action/graph.c
void get_active_ros2_actions(
  const std::map<std::string, std::string> active_ros2_publishers,
  const std::map<std::string, std::string> active_ros2_subscribers,
  std::map<std::string, std::map<std::string, std::string>> & active_ros2_action_servers,
  std::map<std::string, std::map<std::string, std::string>> & active_ros2_action_clients)
{
  std::map<std::string, std::string>::const_iterator it;
  std::map<std::string, uint8_t>::iterator it_num;
  std::map<std::string, uint8_t> action_server_nums, action_client_nums;
  for (it = active_ros2_publishers.begin(); it != active_ros2_publishers.end(); it++) {
    if (
      is_action_topic(
        active_ros2_action_servers, action_server_nums, true, it->first.c_str(),
        "/_action/feedback", it->second.c_str(), "_FeedbackMessage", true))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros2_action_servers, action_server_nums, false,
        it->first.c_str(), "/_action/status", it->second.c_str(),
        "GoalStatusArray"), true)
    {
      continue;
    }
  }
  for (it = active_ros2_subscribers.begin(); it != active_ros2_subscribers.end(); it++) {
    if (
      is_action_topic(
        active_ros2_action_clients, action_client_nums, true,
        it->first.c_str(), "/_action/feedback", it->second.c_str(),
        "_FeedbackMessage", true))
    {
      continue;
    } else if (   // NOLINT
      is_action_topic(
        active_ros2_action_clients, action_client_nums, false,
        it->first.c_str(), "/_action/status", it->second.c_str(),
        "GoalStatusArray", true))
    {
      continue;
    }
  }
  for (it_num = action_client_nums.begin(); it_num != action_client_nums.end(); it_num++) {
    if (it_num->second != 2) {
      active_ros2_action_clients.erase(it_num->first);
    }
  }
  for (it_num = action_server_nums.begin(); it_num != action_server_nums.end(); it_num++) {
    if (it_num->second != 2) {
      active_ros2_action_servers.erase(it_num->first);
    }
  }
}

int main(int argc, char * argv[])
{
  bool output_topic_introspection;
  bool bridge_all_1to2_topics;
  bool bridge_all_2to1_topics;
  bool multi_threads;
  if (!parse_command_options(
      argc, argv, output_topic_introspection, bridge_all_1to2_topics, bridge_all_2to1_topics,
      multi_threads))
  {
    return 0;
  }

  // ROS 2 node
  rclcpp::init(argc, argv);

  auto ros2_node = rclcpp::Node::make_shared("ros_bridge");

  // ROS 1 node
  ros::init(argc, argv, "ros_bridge");
  ros::NodeHandle ros1_node;
  std::unique_ptr<ros::CallbackQueue> ros1_callback_queue = nullptr;
  if (multi_threads) {
    ros1_callback_queue = std::make_unique<ros::CallbackQueue>();
    ros1_node.setCallbackQueue(ros1_callback_queue.get());
  }


  // mapping of available topic names to type names
  std::map<std::string, std::string> ros1_publishers;
  std::map<std::string, std::string> ros1_subscribers;
  std::map<std::string, std::string> ros2_publishers;
  std::map<std::string, std::string> ros2_subscribers;
  std::map<std::string, std::map<std::string, std::string>> ros1_services;
  std::map<std::string, std::map<std::string, std::string>> ros2_services;
  std::map<std::string, std::map<std::string, std::string>> ros1_action_servers;
  std::map<std::string, std::map<std::string, std::string>> ros1_action_clients;
  std::map<std::string, std::map<std::string, std::string>> ros2_action_servers;
  std::map<std::string, std::map<std::string, std::string>> ros2_action_clients;
  std::map<std::string, ros1_bridge::Bridge1to2HandlesAndMessageTypes> bridges_1to2;
  std::map<std::string, ros1_bridge::Bridge2to1HandlesAndMessageTypes> bridges_2to1;
  std::map<std::string, ros1_bridge::ServiceBridge1to2> service_bridges_1_to_2;
  std::map<std::string, ros1_bridge::ServiceBridge2to1> service_bridges_2_to_1;
  std::map<std::string, std::unique_ptr<ros1_bridge::ActionFactoryInterface>> action_bridges_1_to_2;
  std::map<std::string, std::unique_ptr<ros1_bridge::ActionFactoryInterface>> action_bridges_2_to_1;

  // setup polling of ROS 1 master
  auto ros1_poll = [
    &ros1_node, ros2_node,
    &ros1_publishers, &ros1_subscribers,
    &ros2_publishers, &ros2_subscribers,
    &bridges_1to2, &bridges_2to1,
    &ros1_services, &ros2_services,
    &ros1_action_servers, &ros1_action_clients,
    &ros2_action_servers, &ros2_action_clients,
    &service_bridges_1_to_2, &service_bridges_2_to_1,
    &action_bridges_1_to_2, &action_bridges_2_to_1,
    &output_topic_introspection,
    &bridge_all_1to2_topics, &bridge_all_2to1_topics,
    multi_threads
    ](const ros::TimerEvent &) -> void
    {
      // collect all topics names which have at least one publisher or subscriber beside this bridge
      std::set<std::string> active_publishers;
      std::set<std::string> active_subscribers;
      std::map<std::string, std::string> current_ros1_publishers;
      std::map<std::string, std::string> current_ros1_subscribers;
      std::map<std::string, std::map<std::string, std::string>> active_ros1_services;

      XmlRpc::XmlRpcValue payload;
      if (!ros1_bridge::get_ros1_master_system_state(payload)) {
        return;
      }

      ros1_bridge::get_ros1_active_publishers(payload, active_publishers);
      ros1_bridge::get_ros1_active_subscribers(payload, active_subscribers);
      ros1_bridge::get_ros1_current_topics(
        active_publishers, active_subscribers,
        current_ros1_publishers, current_ros1_subscribers,
        output_topic_introspection);
      ros1_bridge::get_ros1_services(payload, active_ros1_services);

      // check actions
      std::map<std::string, std::map<std::string, std::string>>
      active_ros1_action_servers, active_ros1_action_clients;
      get_active_ros1_actions(
        current_ros1_publishers, current_ros1_subscribers,
        active_ros1_action_servers, active_ros1_action_clients);

      {
        std::lock_guard<std::mutex> lock(g_bridge_mutex);
        ros1_services = active_ros1_services;
        ros1_action_servers = active_ros1_action_servers;
        ros1_action_clients = active_ros1_action_clients;
      }

      if (output_topic_introspection) {
        printf("\n");
      }

      {
        std::lock_guard<std::mutex> lock(g_bridge_mutex);
        ros1_services = active_ros1_services;
        ros1_publishers = current_ros1_publishers;
        ros1_subscribers = current_ros1_subscribers;
      }

      update_bridge(
        ros1_node, ros2_node,
        ros1_publishers, ros1_subscribers,
        ros2_publishers, ros2_subscribers,
        ros1_services, ros2_services,
        ros1_action_servers, ros2_action_servers,
        bridges_1to2, bridges_2to1,
        service_bridges_1_to_2, service_bridges_2_to_1,
        action_bridges_1_to_2, action_bridges_2_to_1,
        bridge_all_1to2_topics, bridge_all_2to1_topics,
        multi_threads);
    };

  auto ros1_poll_timer = ros1_node.createTimer(ros::Duration(1.0), ros1_poll);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // setup polling of ROS 2
  std::set<std::string> already_ignored_topics;
  std::set<std::string> already_ignored_services;
  auto ros2_poll = [
    &ros1_node, ros2_node,
    &ros1_publishers, &ros1_subscribers,
    &ros2_publishers, &ros2_subscribers,
    &ros1_services, &ros2_services,
    &ros1_action_servers, &ros1_action_clients,
    &ros2_action_servers, &ros2_action_clients,
    &bridges_1to2, &bridges_2to1,
    &service_bridges_1_to_2, &service_bridges_2_to_1,
    &action_bridges_1_to_2, &action_bridges_2_to_1,
    &output_topic_introspection,
    &bridge_all_1to2_topics, &bridge_all_2to1_topics,
    &already_ignored_topics, &already_ignored_services,
    multi_threads
    ]() -> void
    {
      std::map<std::string, std::string> current_ros2_publishers;
      std::map<std::string, std::string> current_ros2_subscribers;
      std::map<std::string, std::map<std::string, std::string>> active_ros2_services;

      ros1_bridge::get_ros2_current_topics(
        ros2_node,
        current_ros2_publishers, current_ros2_subscribers,
        bridges_1to2, bridges_2to1,
        already_ignored_topics, output_topic_introspection);
      ros1_bridge::get_ros2_services(ros2_node, active_ros2_services, already_ignored_services);

      std::map<std::string, std::map<std::string, std::string>> active_ros2_action_servers,
        active_ros2_action_clients;
      get_active_ros2_actions(
        current_ros2_publishers, current_ros2_subscribers,
        active_ros2_action_servers, active_ros2_action_clients);

      {
        std::lock_guard<std::mutex> lock(g_bridge_mutex);
        ros2_services = active_ros2_services;
        ros2_publishers = current_ros2_publishers;
        ros2_subscribers = current_ros2_subscribers;
        ros2_action_servers = active_ros2_action_servers;
        ros2_action_clients = active_ros2_action_clients;
      }

      if (output_topic_introspection) {
        printf("\n");
      }

      update_bridge(
        ros1_node, ros2_node,
        ros1_publishers, ros1_subscribers,
        ros2_publishers, ros2_subscribers,
        ros1_services, ros2_services,
        ros1_action_servers, ros2_action_servers,
        bridges_1to2, bridges_2to1,
        service_bridges_1_to_2, service_bridges_2_to_1,
        action_bridges_1_to_2, action_bridges_2_to_1,
        bridge_all_1to2_topics, bridge_all_2to1_topics,
        multi_threads);
    };

  auto check_ros1_flag = [&ros1_node] {
      if (!ros1_node.ok()) {
        rclcpp::shutdown();
      }
    };

  auto ros2_poll_timer = ros2_node->create_wall_timer(
    std::chrono::seconds(1), [&ros2_poll, &check_ros1_flag] {
      ros2_poll();
      check_ros1_flag();
    });


  // ROS 1 asynchronous spinner
  std::unique_ptr<ros::AsyncSpinner> async_spinner = nullptr;
  if (!multi_threads) {
    async_spinner = std::make_unique<ros::AsyncSpinner>(1);
  } else {
    async_spinner = std::make_unique<ros::AsyncSpinner>(0, ros1_callback_queue.get());
  }
  async_spinner->start();

  // ROS 2 spinning loop
  std::unique_ptr<rclcpp::Executor> executor = nullptr;
  if (!multi_threads) {
    executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
  } else {
    executor = std::make_unique<rclcpp::executors::MultiThreadedExecutor>();
  }
  executor->add_node(ros2_node);
  executor->spin();

  return 0;
}
