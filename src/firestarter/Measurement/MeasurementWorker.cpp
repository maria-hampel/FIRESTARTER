/******************************************************************************
 * FIRESTARTER - A Processor Stress Test Utility
 * Copyright (C) 2020 TU Dresden, Center for Information Services and High
 * Performance Computing
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/\>.
 *
 * Contact: daniel.hackenberg@tu-dresden.de
 *****************************************************************************/

#include <firestarter/Measurement/MeasurementWorker.hpp>

#include <queue>
#include <thread>

extern "C" {
#include <dlfcn.h>
}

using namespace firestarter::measurement;

MeasurementWorker::MeasurementWorker(
    std::chrono::milliseconds updateInterval, unsigned long long numThreads,
    std::vector<std::string> const &metricDylibs)
    : updateInterval(updateInterval), numThreads(numThreads) {

  // open dylibs and find metric symbol.
  // create an entry in _metricDylibs with handle from dlopen and
  // metric_interface_t structure. add this structe as a pointer to metrics.
  for (auto const &dylib : metricDylibs) {
    void *handle;
    const char *filename = dylib.c_str();

    handle = dlopen(dylib.c_str(), RTLD_NOW | RTLD_LOCAL);

    if (!handle) {
      firestarter::log::error() << filename << ": " << dlerror();
      continue;
    }

    // clear existing error
    dlerror();

    metric_interface_t *metric = nullptr;

    metric = (metric_interface_t *)dlsym(handle, "metric");

    char *error;
    if ((error = dlerror()) != NULL) {
      firestarter::log::error() << filename << ": " << error;
      dlclose(handle);
      continue;
    }

    // lets push our metric object and the handle
    this->_metricDylibs.push_back(handle);
    this->metrics.push_back(metric);
  }

  std::stringstream ss;
  unsigned maxLength = 0;
  std::map<std::string, bool> available;

  for (auto const &name : this->metricNames()) {
    maxLength = maxLength < name.size() ? name.size() : maxLength;
    auto metric = this->findMetricByName(name);
    if (metric == nullptr) {
      available[name] = false;
      continue;
    }
    int returnCode = metric->init();
    metric->fini();
    available[name] = returnCode == EXIT_SUCCESS ? true : false;
  }

  unsigned padding = maxLength > 6 ? maxLength - 6 : 0;
  ss << "  METRIC" << std::string(padding + 1, ' ') << "| available\n";
  ss << "  " << std::string(padding + 7, '-') << "-----------\n";
  for (auto const &[key, value] : available) {
    ss << "  " << key << std::string(padding + 7 - key.size(), ' ') << "| ";
    ss << (value ? "yes" : "no") << "\n";
  }

  this->availableMetricsString = ss.str();

  pthread_create(&this->workerThread, NULL,
                 reinterpret_cast<void *(*)(void *)>(
                     MeasurementWorker::dataAcquisitionWorker),
                 this);
}

MeasurementWorker::~MeasurementWorker() {
  pthread_cancel(this->workerThread);

  pthread_join(this->workerThread, NULL);

  for (auto const &[key, value] : this->values) {
    auto metric = this->findMetricByName(key);
    if (metric == nullptr) {
      continue;
    }

    metric->fini();
  }

  for (auto handle : this->_metricDylibs) {
    dlclose(handle);
  }
}

std::vector<std::string> MeasurementWorker::metricNames() {
  std::vector<std::string> metrics;
  std::transform(
      this->metrics.begin(), this->metrics.end(), std::back_inserter(metrics),
      [](auto &metric) -> std::string { return std::string(metric->name); });
  return metrics;
}

const metric_interface_t *
MeasurementWorker::findMetricByName(std::string metricName) {
  auto name_equal = [metricName](auto &metricInterface) {
    return metricName.compare(metricInterface->name) == 0;
  };
  auto metric =
      std::find_if(this->metrics.begin(), this->metrics.end(), name_equal);

  // metric not found
  if (metric == this->metrics.end()) {
    return nullptr;
  }
  // metric found
  return const_cast<const metric_interface_t *>(*metric);
}

// this must be called by the main thread.
// if not done so things like perf_event_attr.inherit might not work as expected
unsigned
MeasurementWorker::initMetrics(std::vector<std::string> const &metricNames) {
  this->values_mutex.lock();

  unsigned count = 0;

  // try to find each metric and initialize it
  for (auto const &metricName : metricNames) {
    // init values map with empty vector
    auto name_equal = [metricName](auto const &pair) {
      return metricName.compare(pair.first) == 0;
    };
    auto pair =
        std::find_if(this->values.begin(), this->values.end(), name_equal);
    if (pair != this->values.end()) {
      pair->second.clear();
    } else {
      auto metric = this->findMetricByName(metricName);
      // metric not found
      if (metric == nullptr) {
        continue;
      }
      // metric found
      int returnValue = metric->init();
      if (returnValue != EXIT_SUCCESS) {
        log::error() << "Metric " << metric->name << ": "
                     << metric->get_error();
      } else {
        this->values[metricName] = std::vector<TimeValue>();
        count++;
      }
    }
  }

  this->values_mutex.unlock();

  return count;
}

void MeasurementWorker::startMeasurement() {
  this->startTime = std::chrono::high_resolution_clock::now();
}

std::map<std::string, Summary>
MeasurementWorker::getValues(std::chrono::milliseconds startDelta,
                             std::chrono::milliseconds stopDelta) {
  auto startTime = this->startTime + startDelta;
  auto endTime = std::chrono::high_resolution_clock::now() - stopDelta;

  auto findAll = [startTime, endTime](auto const &tv) {
    return startTime <= tv.time && tv.time <= endTime;
  };

  std::map<std::string, Summary> measurment = {};

  this->values_mutex.lock();

  for (auto &[key, values] : this->values) {
    auto begin = std::find_if(values.begin(), values.end(), findAll);
    auto end = values.end();

    auto metric = this->findMetricByName(key);
    if (metric == nullptr) {
      continue;
    }

    Summary sum = Summary::calculate(begin, end, metric->type);

    measurment[key] = sum;
  }

  this->values_mutex.unlock();

  return measurment;
}

int *MeasurementWorker::dataAcquisitionWorker(void *measurementWorker) {

  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

  auto _this = reinterpret_cast<MeasurementWorker *>(measurementWorker);

#ifndef __APPLE__
  pthread_setname_np(pthread_self(), "DataAcquisition");
#endif

  using clock = std::chrono::high_resolution_clock;

  using callbackTuple =
      std::tuple<void (*)(void), std::chrono::microseconds,
                 std::chrono::high_resolution_clock::time_point>;
  auto callbackTupleComparator = [](callbackTuple left, callbackTuple right) {
    return std::get<2>(left) > std::get<2>(right);
  };

  // this datastructure holds a tuple of our callback, the callback frequency
  // and the next timepoint. it will be sorted, so the pop function will give
  // back the next callback
  std::priority_queue<callbackTuple, std::vector<callbackTuple>,
                      decltype(callbackTupleComparator)>
      callbackQueue(callbackTupleComparator);

  _this->values_mutex.lock();

  for (auto const &[key, value] : _this->values) {
    auto metric_interface = _this->findMetricByName(key);

    auto callbackTime =
        std::chrono::microseconds(metric_interface->callback_time);
    if (callbackTime.count() == 0) {
      continue;
    }

    auto currentTime = clock::now();

    callbackQueue.push(
        std::make_tuple(metric_interface->callback, callbackTime, currentTime));
  }

  _this->values_mutex.unlock();

  auto nextFetch = clock::now() + _this->updateInterval;

  for (;;) {
    auto now = clock::now();

    if (nextFetch <= now) {
      _this->values_mutex.lock();

      for (auto &[metricName, values] : _this->values) {
        auto metric_interface = _this->findMetricByName(metricName);

        double value;

        if (EXIT_SUCCESS == metric_interface->get_reading(&value)) {
          if (metric_interface->type & METRIC_DIVIDE_BY_THREAD_COUNT) {
            value /= (double)_this->numThreads;
          }

          auto tv = TimeValue(std::chrono::high_resolution_clock::now(), value);
          values.push_back(tv);
        }
      }

      _this->values_mutex.unlock();

      nextFetch = now + _this->updateInterval;
    }

    auto nextWake = nextFetch;

    if (!callbackQueue.empty()) {
      auto [callbackFunction, callbackTime, nextCallback] = callbackQueue.top();

      if (nextCallback <= now) {
        // remove the elment from the queue
        callbackQueue.pop();

        // call our callback
        callbackFunction();

        // add it with the updated callback time to the queue again
        nextCallback = now + callbackTime;
        callbackQueue.push(
            std::make_tuple(callbackFunction, callbackTime, nextCallback));
      }

      nextWake = nextCallback < nextWake ? nextCallback : nextWake;
    }

    std::this_thread::sleep_for(nextWake - clock::now());
  }
}
