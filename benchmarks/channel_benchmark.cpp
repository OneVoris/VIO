#include <voris/io/channel.hpp>

#include "benchmark_support.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

int main() {
    using namespace voris::io;

    constexpr std::size_t count = 4096;
    channel<int> queue(count);
    std::vector<std::int64_t> samples;
    samples.reserve(count * 2U);
    std::size_t sent = 0;
    std::size_t received = 0;
    std::size_t errors = 0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i != count; ++i) {
        const auto operation_started = std::chrono::steady_clock::now();
        auto result = queue.send(static_cast<int>(i));
        samples.push_back(vio_bench::elapsed_ns(operation_started,
                                                std::chrono::steady_clock::now()));
        if (result.has_value()) {
            ++sent;
        } else {
            ++errors;
        }
    }
    for (std::size_t i = 0; i != count; ++i) {
        const auto operation_started = std::chrono::steady_clock::now();
        auto value = queue.receive();
        samples.push_back(vio_bench::elapsed_ns(operation_started,
                                                std::chrono::steady_clock::now()));
        if (value.has_value()) {
            ++received;
        } else {
            ++errors;
        }
    }
    const auto finished = std::chrono::steady_clock::now();

    const bool ok = sent == count && received == count && errors == 0;
    vio_bench::record record{
        .benchmark = "channel",
        .workload = "bounded_channel_send_receive",
        .result = ok ? "ok" : "failed",
        .reason = ok ? "ok" : "count_mismatch",
        .operations = sent + received,
        .elapsed_ns = vio_bench::elapsed_ns(started, finished),
        .errors = errors + (ok ? 0U : 1U),
        .extra = {{"sent", std::to_string(sent)},
                  {"received", std::to_string(received)},
                  {"capacity", std::to_string(count)}}};
    vio_bench::set_latency_percentiles(record, samples);
    vio_bench::emit(record);
    return ok ? 0 : 1;
}
