#include "writer.h"
#include <iostream>
#include "version_number.h"

namespace datadog {
namespace opentracing {

namespace {
const std::string agent_api_path = "/v0.3/traces";
const std::string agent_protocol = "http://";
// Max amount of time to wait between sending spans to agent. Agent discards spans older than 10s,
// so that is the upper bound.
const std::chrono::milliseconds default_write_period = std::chrono::seconds(6);
}  // namespace

template <class Span>
AgentWriter<Span>::AgentWriter(std::string host, uint32_t port)
    : AgentWriter(std::unique_ptr<Handle>{new CurlHandle{}}, config::tracer_version,
                  default_write_period, host, port){};

template <class Span>
AgentWriter<Span>::AgentWriter(std::unique_ptr<Handle> handle, std::string tracer_version,
                               std::chrono::milliseconds write_period, std::string host,
                               uint32_t port)
    : write_period_(write_period), tracer_version_(tracer_version) {
  setUpHandle(handle, host, port);
  startWriting(std::move(handle));
}

template <class Span>
void AgentWriter<Span>::setUpHandle(std::unique_ptr<Handle> &handle, std::string host,
                                    uint32_t port) {
  // Some options are the same for all actions, set them here.
  // Set the agent URI.
  std::stringstream agent_uri;
  agent_uri << agent_protocol << host << ":" << port << agent_api_path;
  auto rcode = handle->setopt(CURLOPT_URL, agent_uri.str().c_str());
  if (rcode != CURLE_OK) {
    throw std::runtime_error(std::string("Unable to set agent URL: ") + curl_easy_strerror(rcode));
  }
  // Set the common HTTP headers.
  rcode = handle->appendHeaders({"Content-Type: application/msgpack", "Datadog-Meta-Lang: cpp",
                                 "Datadog-Meta-Tracer-Version: " + tracer_version_});
  if (rcode != CURLE_OK) {
    throw std::runtime_error(std::string("Unable to set agent connection headers: ") +
                             curl_easy_strerror(rcode));
  }
}

template <class Span>
AgentWriter<Span>::~AgentWriter() {
  stop();
}

template <class Span>
void AgentWriter<Span>::stop() {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stop_writing_) {
      return;  // Already stopped.
    }
    stop_writing_ = true;
  }
  condition_.notify_all();
  worker_->join();
}

template <class Span>
void AgentWriter<Span>::write(Span &&span) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stop_writing_) {
    return;
  }
  spans_.push_back(std::move(span));
};

template <class Span>
void AgentWriter<Span>::startWriting(std::unique_ptr<Handle> handle) {
  // Start worker that sends Spans to agent.
  // We can capture 'this' because destruction of this stops the thread and the lambda.
  worker_ = std::make_unique<std::thread>(
      [this](std::unique_ptr<Handle> handle) {
        std::stringstream buffer;
        size_t num_spans = 0;
        while (true) {
          // Encode spans when there are new ones.
          {
            // Wait to be told about new spans (or to stop).
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, write_period_,
                                [&]() -> bool { return flush_worker_ || stop_writing_; });
            if (stop_writing_) {
              return;  // Stop the thread.
            }
            num_spans = spans_.size();
            if (num_spans == 0) {
              continue;
            }
            // Clear the buffer but keep the allocated memory.
            buffer.clear();
            buffer.str(std::string{});
            // Group Spans by trace_id.
            // TODO[willgittoes-dd]: Investigate whether it's faster to have grouping done on
            // write().
            std::unordered_map<int, std::vector<std::reference_wrapper<Span>>> spans_by_trace;
            for (Span &span : spans_) {
              spans_by_trace[span.traceId()].push_back(span);
            }
            // Change outer collection type to sequential from associative.
            std::vector<std::reference_wrapper<std::vector<std::reference_wrapper<Span>>>> traces;
            for (auto &trace : spans_by_trace) {
              traces.push_back(trace.second);
            }
            msgpack::pack(buffer, traces);
            spans_.clear();
          }  // lock on mutex_ ends.
          // Send spans, not in critical period.
          AgentWriter<Span>::postSpans(handle, buffer, num_spans);
          // Let thread calling 'flush' that we're done flushing.
          {
            std::unique_lock<std::mutex> lock(mutex_);
            flush_worker_ = false;
          }
          condition_.notify_all();
        }
      },
      std::move(handle));
}  // namespace opentracing

template <class Span>
void AgentWriter<Span>::flush() try {
  std::unique_lock<std::mutex> lock(mutex_);
  flush_worker_ = true;
  condition_.notify_all();
  // Wait until flush is complete.
  condition_.wait(lock, [&]() -> bool { return !flush_worker_ || stop_writing_; });
} catch (const std::bad_alloc &) {
}

template <class Span>
void AgentWriter<Span>::postSpans(std::unique_ptr<Handle> &handle, std::stringstream &buffer,
                                  size_t num_spans) try {
  auto rcode = handle->appendHeaders({"X-Datadog-Trace-Count: " + std::to_string(num_spans)});
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent communication headers: " << curl_easy_strerror(rcode)
              << std::endl;
    return;
  }

  // We have to set the size manually, because msgpack uses null characters.
  std::string post_fields = buffer.str();
  rcode = handle->setopt(CURLOPT_POSTFIELDSIZE, post_fields.size());
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent request size: " << curl_easy_strerror(rcode) << std::endl;
    return;
  }

  rcode = handle->setopt(CURLOPT_POSTFIELDS, post_fields.data());
  if (rcode != CURLE_OK) {
    std::cerr << "Error setting agent request body: " << curl_easy_strerror(rcode) << std::endl;
    return;
  }

  rcode = handle->perform();
  if (rcode != CURLE_OK) {
    std::cerr << "Error sending traces to agent: " << curl_easy_strerror(rcode) << std::endl
              << handle->getError() << std::endl;
    return;
  }
} catch (const std::bad_alloc &) {
  // Drop spans, but live to fight another day.
}

// Make sure we generate code for a Span-writing Writer.
template class AgentWriter<Span>;

}  // namespace opentracing
}  // namespace datadog
