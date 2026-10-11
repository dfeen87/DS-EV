// Independent PB-06/PB-08 reconciliation; all CAN traffic is synthetic receive-only.
#include "ds_bms_middleware_v2.hpp"
#include "../src/ds_bms_hardware_adapter.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

void check_safety(const ds_plugin::SafetyLimits& limits, double current,
                  double temperature, double soc, bool fault,
                  bool soc_warning, bool low_temp_warning,
                  const char* expected_message) {
    ds_plugin::SafetyMonitor monitor(limits);
    ds::DSState raw;
    raw.voltage = 400.0;
    raw.current = current;
    raw.temperature = temperature;
    raw.state_of_charge = soc;
    ds_plugin::DiagnosticReport diagnostic;
    diagnostic.max_cell_temp_c = 25.0;
    const std::string label = "PB-06 current=" + std::to_string(current) +
        " temperature=" + std::to_string(temperature) + " SOC=" + std::to_string(soc);
    check(monitor.check(raw, diagnostic) == !fault, label + " check result");
    check(monitor.has_fault() == fault, label + " fault");
    check(monitor.has_soc_warning() == soc_warning, label + " SOC warning");
    check(monitor.has_low_temp_warning() == low_temp_warning, label + " cold warning");
    const auto& messages = fault ? monitor.faults() : monitor.advisories();
    if (expected_message) {
        check(messages.size() == 1 && messages.front() == expected_message,
              label + " exact reason");
    } else {
        check(messages.empty(), label + " no reason at equality/normal state");
    }
    check(fault ? monitor.advisories().empty() : monitor.faults().empty(),
          label + " correct diagnostic category");
    // A succeeding cycle removes prior warnings/reasons rather than leaking them.
    raw.current = 0.0; raw.temperature = 25.0; raw.state_of_charge = 0.5;
    check(monitor.check(raw, diagnostic) && !monitor.has_fault() &&
          !monitor.has_soc_warning() && !monitor.has_low_temp_warning() &&
          monitor.faults().empty() && monitor.advisories().empty(), label + " recovery");
}

void safety_boundary_matrix() {
    ds_plugin::SafetyLimits defaults;
    ds_plugin::SafetyLimits custom;
    custom.min_soc = 0.2; custom.max_soc = 0.8; custom.min_operating_temp = 0.0;
    const double lower_direction = -std::numeric_limits<double>::infinity();
    const double upper_direction = std::numeric_limits<double>::infinity();
    for (const auto& limits : {defaults, custom}) {
        for (double current : {-10.0, -0.0, 0.0, 10.0}) {
            const double low = std::nextafter(limits.min_soc, lower_direction);
            const double high = std::nextafter(limits.max_soc, upper_direction);
            check_safety(limits, current, 25.0, low, current > 0.0, true, false,
                current > 0.0 ? "SOC below minimum operating limit while discharging" :
                current < 0.0 ? "SOC below minimum operating limit while charging" :
                                "SOC below minimum operating limit while resting");
            check_safety(limits, current, 25.0, high, current < 0.0, true, false,
                current < 0.0 ? "SOC above maximum operating limit while charging" :
                current > 0.0 ? "SOC above maximum operating limit while discharging" :
                                "SOC above maximum operating limit while resting");
            for (double soc : {limits.min_soc,
                               std::nextafter(limits.min_soc, upper_direction),
                               std::nextafter(limits.max_soc, lower_direction),
                               limits.max_soc}) {
                check_safety(limits, current, 25.0, soc, false, false, false, nullptr);
            }
            check_safety(limits, current,
                std::nextafter(limits.min_operating_temp, lower_direction), 0.5,
                current != 0.0, false, true,
                current != 0.0 ? "Temperature below minimum operating limit while active" :
                                 "Temperature below minimum operating limit while resting");
            for (double temperature : {limits.min_operating_temp,
                        std::nextafter(limits.min_operating_temp, upper_direction),
                        limits.max_operating_temp}) {
                check_safety(limits, current, temperature, 0.5, false, false, false, nullptr);
            }
            check_safety(limits, current,
                std::nextafter(limits.max_operating_temp, upper_direction), 0.5,
                true, false, false, "Temperature too high");
        }
    }
    // These are the exact three historical at-rest stimuli: now warnings/advisories,
    // intentionally without a hard fault under the documented directional policy.
    check_safety(custom, 0.0, -5.0, 0.5, false, false, true,
                 "Temperature below minimum operating limit while resting");
    check_safety(custom, 0.0, 25.0, 0.1, false, true, false,
                 "SOC below minimum operating limit while resting");
    check_safety(custom, 0.0, 25.0, 0.9, false, true, false,
                 "SOC above maximum operating limit while resting");
}

class FakeTransport final : public ds_plugin::ICANTransport {
public:
    unsigned send_count = 0;
    std::vector<ds_plugin::CANFrame> frames;
    bool send(const ds_plugin::CANFrame&) override {
        ++send_count;
        throw std::runtime_error("Reconciliation fixture must never transmit");
    }
    std::optional<ds_plugin::CANFrame> receive() override {
        if (frames.empty()) return std::nullopt;
        const auto frame = frames.back(); frames.pop_back(); return frame;
    }
    void fill(const ds_plugin::OEMSignalMap& map) {
        for (auto signal : std::array<std::pair<unsigned, unsigned>, 4>{{
                {map.pack_voltage_id, 360}, {map.pack_current_id, 10},
                {map.pack_temp_id, 25}, {map.pack_soc_id, 50}}}) {
            ds_plugin::CANFrame frame;
            frame.id = signal.first;
            frame.data[0] = static_cast<uint8_t>(signal.second & 0xff);
            frame.data[1] = static_cast<uint8_t>(signal.second >> 8);
            frames.push_back(frame);
        }
    }
};

void reads(ds_plugin::DSHardwareAdapter& adapter, bool accepted, const std::string& label) {
    using Getter = double (ds_plugin::DSHardwareAdapter::*)() const;
    const std::array<Getter, 4> getters{{
        &ds_plugin::DSHardwareAdapter::read_pack_voltage,
        &ds_plugin::DSHardwareAdapter::read_pack_current,
        &ds_plugin::DSHardwareAdapter::read_pack_temperature,
        &ds_plugin::DSHardwareAdapter::estimate_soc}};
    const std::array<double, 4> expected{{360.0, 10.0, 25.0, 0.5}};
    for (std::size_t i = 0; i != getters.size(); ++i) {
        try {
            const double value = (adapter.*getters[i])();
            check(accepted && value == expected[i], label + " getter " + std::to_string(i));
        } catch (const std::runtime_error&) {
            check(!accepted, label + " getter " + std::to_string(i));
        }
    }
}

void hardware_clock_matrix() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double maximum = std::numeric_limits<double>::max();
    for (double max_age : {-1.0, nan, inf, -inf}) {
        FakeTransport transport;
        ds_plugin::OEMSignalMap map; map.max_signal_age_s = max_age;
        bool rejected = false;
        try { ds_plugin::DSHardwareAdapter adapter(transport, map, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "PB-08 invalid maximum age");
        check(transport.send_count == 0, "PB-08 invalid config sends nothing");
    }
    for (double max_age : {0.0, 0.5, 2.0}) {
        FakeTransport transport;
        ds_plugin::OEMSignalMap map; map.max_signal_age_s = max_age;
        map.pack_voltage_scale = map.pack_current_scale = map.pack_temp_scale =
            map.pack_soc_scale = 1.0;
        ds_plugin::DSHardwareAdapter adapter(transport, map, 1);
        reads(adapter, false, "PB-08 missing signals");
        adapter.set_now_seconds(100.0); transport.fill(map); adapter.poll_can();
        reads(adapter, true, "PB-08 fresh at stamp");
        adapter.set_now_seconds(100.0 + max_age);
        reads(adapter, true, "PB-08 maximum age equality");
        adapter.set_now_seconds(std::nextafter(100.0 + max_age, inf));
        reads(adapter, false, "PB-08 immediately expired");
        adapter.set_now_seconds(std::nextafter(100.0, -inf));
        reads(adapter, false, "PB-08 immediately future");
        adapter.set_now_seconds(0.0);
        reads(adapter, false, "PB-08 historical backward clock");
        for (double clock : {nan, inf, -inf}) {
            adapter.set_now_seconds(clock);
            reads(adapter, false, "PB-08 malformed clock");
            transport.fill(map); adapter.poll_can(); adapter.set_now_seconds(100.0);
            reads(adapter, false, "PB-08 malformed cached timestamp");
            transport.fill(map); adapter.poll_can();
            reads(adapter, true, "PB-08 recache recovery");
        }
        adapter.set_now_seconds(-maximum); transport.fill(map); adapter.poll_can();
        adapter.set_now_seconds(maximum);
        reads(adapter, false, "PB-08 finite subtraction overflows age");
        check(transport.send_count == 0, "PB-08 fake transport sends nothing");
    }
    // The compatible adapter checks calculated age, not clock trajectory. A clock
    // rollback that leaves a positive fresh age requires an external monotonic source.
    FakeTransport transport;
    ds_plugin::OEMSignalMap map;
    map.pack_voltage_scale = map.pack_current_scale = map.pack_temp_scale = map.pack_soc_scale = 1.0;
    ds_plugin::DSHardwareAdapter adapter(transport, map, 1);
    adapter.set_now_seconds(100.0); transport.fill(map); adapter.poll_can();
    adapter.set_now_seconds(100.4); reads(adapter, true, "PB-08 ordinary positive age");
    adapter.set_now_seconds(100.2); reads(adapter, true, "PB-08 documented positive-age rollback limit");
    check(transport.send_count == 0, "PB-08 compatible limitation sends nothing");
}
} // namespace

int main() {
    safety_boundary_matrix();
    hardware_clock_matrix();
    std::cout << "PB-06/PB-08 reconciliation: " << checks << " checks, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
