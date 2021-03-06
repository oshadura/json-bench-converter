#include <iostream>
#include <regex>
#include <fstream>

#include "json.hpp"
using json = nlohmann::json;

// Data structures for holding the parsed data

// A single run of a single function with a certain template type
struct bench_run {
  std::map<std::string, long> times;

  bench_run(long real_time, long cpu_time)
  {
    times["real"] = real_time;
    times["CPU"] = cpu_time;
  }
  bench_run() = default;
};

// Series of runs for the same function with the same template type.
// Input size is either this funny integer we have in front of "_mean"
// or the number of threads
struct bench_series {
  std::map<long, bench_run> mean_run_by_input_size;
  std::map<long, bench_run> stddev_run_by_input_size;
};

// All series of this function with their template types 
struct bench_func {
  std::map<std::string, bench_series> series_by_template_type;
};

static std::vector<std::string> split_string(const std::string& in, char delimiter) {
   std::vector<std::string> out;
   std::string token = "";
   for (size_t i = 0, e = in.size(); i < e; ++i) {
      if (in[i] == delimiter && !token.empty()) {
         out.push_back(token);
         token = "";
      }
      else
         token += in[i];
   }
   if (!token.empty())
      out.push_back(token);
   return out;
}

// Goes through all nodes and creates javascript files for stuff that looks like vectorized code
bool handle_vectorize_cases(json j) {
  
  // List of all the functions we benchmarked 
  std::map<std::string, bench_func> funcs;

  //std::regex mean_regex(R"regex(([A-Za-z_0-9:]+)[<]([A-Za-z_0-9:]+)[>]/(\d+)_mean)regex");
  //std::regex stddev_regex(R"regex(([A-Za-z_0-9:]+)[<]([A-Za-z_0-9:]+)[>]/(\d+)_stddev)regex");
  //std::smatch base_match;

  for(auto& b : j["benchmarks"]) {
    std::string name = b["name"];
    long real_time = b["real_time"];
    long cpu_time = b["cpu_time"];
    std::string template_arg;
    std::string func;
    long input_size = 0;
    bool is_mean = false;
    bool is_stddev = false;

    for (size_t i = 0, e = name.size(); i < e; ++i) {
      while (isalnum(name[i]) || name[i] == '_') {
        func += name[i++];
      }
      if (name[i++] == '<') {
        while (name[i] != '>')
          template_arg += name[i++];
        // Consume >
        ++i;
      }
      assert(name[i] == '/' && "Broken input");
      if (isdigit(name[++i])) { // consume '/'
        input_size = atoi(&name[i]);
        while (isdigit(name[i++]));
        --i; // back up one to get to the _
      }
      if (name[i] == '_') {
        ++i; // skip '_'
        std::string mean_or_stddev = "";
        while(i < e)
          mean_or_stddev += name[i++];
        if (mean_or_stddev == "mean")
          is_mean = true;
        else if (mean_or_stddev == "stddev")
          is_stddev = true;
        else
          assert(0 && "Unreachable");
      }
    }

    auto & series = funcs[func].series_by_template_type[template_arg];
    if (is_mean) {
      series.mean_run_by_input_size[input_size] = bench_run(real_time, cpu_time);
    } else if (is_stddev) {
      series.stddev_run_by_input_size[input_size] = bench_run(real_time, cpu_time);
    } else {
      assert(false);
    }
  }

  for (auto& func_pair : funcs) {
    std::string function_name = func_pair.first;
    json output = {
      {"chart", {
        {"zoomType" , "x"}
      }},
      {"title" , {
        {"text" , "Vectorization " + function_name}
      }},
      {"yAxis" , {
          {"labels" , {
            {"format" , "{value}ms"}
          }},
          {"title", {"text" , "time"}}
      }},

      {"tooltip", {
        {"shared", true}
      }},
    };
    
    json series_list = json::array();
    
    std::vector<std::string> time_kinds = {"real"/*, "CPU"*/};
    // We want one data row for real and one for CPU time.
    // The strings are taken to lookup in the map of bench_run.
    for (std::string time_kind : time_kinds) {
      for (auto& series : func_pair.second.series_by_template_type) {
        std::string template_type = series.first;
        json mean_run = {
          {"name", time_kind + " time " + template_type},
          {"type", "spline"},
          {"data", json::array()},
          {"marker", {
              {"enabled", false}
          }},
        };
        json stddev_run = {
          {"color", "#FF0000"},
          {"name", time_kind + " time error " + template_type},
          {"type", "errorbar"},
          {"data", json::array()},
          {"tooltip", {
              "pointFormat", "Error range: {point.low}-{point.high}ms"
          }},
          {"stemWidth", 3},
          {"whiskerLength", 0}
        };
        for (auto& run : series.second.mean_run_by_input_size) {
          // We want to have [input_size, time] so that we get the axis range right.
          mean_run["data"] += json::array({run.first, run.second.times[time_kind]});
        }
        for (auto& run : series.second.stddev_run_by_input_size) {
          // The stddev is relative, so we have to get the mean time and then add/substract
          // our stddev time
          auto mean_time = series.second.mean_run_by_input_size[run.first].times[time_kind];
          auto stddev_time = run.second.times[time_kind];
          // We want to have [input_size, min, max] so that we get the axis range right.
          stddev_run["data"] += json::array({run.first, mean_time - stddev_time, mean_time + stddev_time});
        }
        series_list += mean_run;
        series_list += stddev_run;
      }
    }
    output["series"] = series_list;

    std::cout << "Highcharts.chart('container', ";
    std::cout << std::setw(1) << output << ");\n";
  }
  return true;
}

// Goes through all nodes and creates javascript files for things that look like
// threading benchmarks.
bool handle_threading_cases(json j) {
  
  // List of all the functions we benchmarked 
  std::map<std::string, bench_series> funcs;

  //std::regex thread_regex(R"regex(([A-Za-z_0-9:]+)\/threads:(\d+))regex");
  //std::smatch base_match;

  // Build up our data structure "funcs"
  for(auto& b : j["benchmarks"]) {
    std::string name = b["name"];
    long real_time = b["real_time"];
    long cpu_time = b["cpu_time"];
    std::string template_arg;
    std::string func;
    long threads;
    
    // BM_TBufferFile_FillTreeWithRandomData/real_time/threads:56
    std::vector<std::string> base_match = split_string(name, '/');
    if (!base_match.size()) {
      std::cerr << "couldnt match name for thread regex: " << name << std::endl;
      continue;
    }

    if (base_match.size() == 3) {
      // Split the thread count
      auto thread_and_count = base_match[base_match.size()-1];
      base_match.push_back(split_string(thread_and_count, ':')[1]);
      split_string(base_match[base_match.size()-1], ':');

      func = base_match[1];
      threads = std::atol(base_match[base_match.size()-1].c_str());
      auto & series = funcs[func];
      series.mean_run_by_input_size[threads] = bench_run(real_time, cpu_time);
    } else {
      std::cerr << "not enough results in match?" << std::endl;
    }
  }

  // Foreach function, create a javascript file
  for (auto& func_pair : funcs) {
    std::string func_name = func_pair.first;
    // header of the JSON object with generic information
    json output = {
      {"chart", {
        {"zoomType" , "x"}
      }},
      {"title" , {
        {"text" , "Threading " + func_name}
      }},
      {"yAxis" , {
          {"labels" , {
            {"format" , "{value}ms"}
          }},
          {"title", {"text" , "time"}}
      }},

      {"tooltip", {
        {"shared", true}
      }},
    };
    
    json series_list = json::array();
    
    std::vector<std::string> time_kinds = {"real", "CPU"};
    // We want one data row for real and one for CPU time.
    // The strings are taken to lookup in the map of bench_run.
    for (std::string time_kind : time_kinds) {
      json mean_run = {
        {"name", time_kind + " time " + func_name},
        {"type", "spline"},
        {"data", json::array()},
        {"marker", {
            {"enabled", false}
        }},
      };
      for (auto& run : func_pair.second.mean_run_by_input_size) {
        // We want to have [thread_size, time] so that we get the axis range right.
        mean_run["data"] += json::array({run.first, run.second.times[time_kind]});
      }
      series_list += mean_run;
    }
    output["series"] = series_list;

    // Write the 'output' json object to this file with some
    // code around that makes it valid javascript.
    std::cout << "Highcharts.chart('container', ";
    std::cout << std::setw(1) << output << ");\n";
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc <= 1) {
    std::cerr << "Invoke program like this: "
              << argv[0] << " input.json" << std::endl;
    return 1;
  }
  // read the input JSON file into j
  std::ifstream i(argv[1]);
  json j;
  i >> j;
  // Create vectorize/threading JavaScript files
  return handle_vectorize_cases(j) || handle_threading_cases(j);
}
