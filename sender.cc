#include "netsim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <vector>

namespace {

constexpr int CRC_BYTES = 4;
constexpr int SIZE_BYTES = 2;
constexpr int MIN_PAYLOAD = 16;
constexpr int MAX_PAYLOAD = 65535;
constexpr double ROUND_TRIP_COST_K = 250.0;
constexpr double FRAME_FIXED_COST = ROUND_TRIP_COST_K + SIZE_BYTES + CRC_BYTES; // 256

uint32_t crc_table[256];
bool crc_table_ready = false;

void init_crc_table() {
    if (crc_table_ready) return;

    constexpr uint32_t POLY = 0x04C11DB7u; // CRC-32 generator without the x^32 bit
    for (int i = 0; i < 256; ++i) {
        uint32_t r = static_cast<uint32_t>(i) << 24;
        for (int b = 0; b < 8; ++b) {
            if (r & 0x80000000u) r = (r << 1) ^ POLY;
            else r <<= 1;
        }
        crc_table[i] = r;
    }
    crc_table_ready = true;
}

uint32_t crc32_mod2(const uint8_t *data, int len) {
    init_crc_table();

    uint32_t r = 0;
    for (int i = 0; i < len; ++i) {
        uint8_t idx = static_cast<uint8_t>((r >> 24) ^ data[i]);
        r = (r << 8) ^ crc_table[idx];
    }
    return r;
}

void put_u32_be(std::vector<uint8_t> &v, int pos, uint32_t x) {
    v[pos + 0] = static_cast<uint8_t>((x >> 24) & 0xffu);
    v[pos + 1] = static_cast<uint8_t>((x >> 16) & 0xffu);
    v[pos + 2] = static_cast<uint8_t>((x >> 8) & 0xffu);
    v[pos + 3] = static_cast<uint8_t>(x & 0xffu);
}

std::vector<uint8_t> make_frame(const uint8_t *payload, int payload_size) {
    std::vector<uint8_t> frame(SIZE_BYTES + payload_size + CRC_BYTES);

    frame[0] = static_cast<uint8_t>((payload_size >> 8) & 0xff);
    frame[1] = static_cast<uint8_t>(payload_size & 0xff);
    std::memcpy(frame.data() + SIZE_BYTES, payload, static_cast<size_t>(payload_size));

    uint32_t crc = crc32_mod2(frame.data(), SIZE_BYTES + payload_size);
    put_u32_be(frame, SIZE_BYTES + payload_size, crc);
    return frame;
}

struct AttemptRecord {
    int bits;
    bool ack;
};

class PayloadController {
public:
    int next_size() const {
        return std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, payload_size_));
    }

    void add_attempt(int payload_size, bool ack) {
        // According to the specification, the size header is protected by netsim;
        // random bit errors are injected into payload + CRC only.
        int bits = 8 * (payload_size + CRC_BYTES);
        history_.push_back({bits, ack});
        if (history_.size() > HISTORY_LIMIT) history_.pop_front();
    }

    void finish_frame(int attempts) {
        int naks = attempts - 1;

        double p = estimate_ber();
        if (p > 0.0) {
            int target = optimal_payload(p);

            if (naks >= 2) {
                payload_size_ = target;                 // react quickly to a clearly bad frame size
            } else if (naks == 1) {
                payload_size_ = (payload_size_ + 2 * target) / 3;
            } else {
                if (target > payload_size_) {
                    payload_size_ = payload_size_ + std::max(1, (target - payload_size_) / 4);
                } else {
                    payload_size_ = (3 * payload_size_ + target) / 4;
                }
            }
        } else {
            // No observed corruption in the recent window: probe larger frames to reduce RTT cost.
            long long grown = static_cast<long long>(payload_size_ * 3LL) / 2LL + 1LL;
            payload_size_ = static_cast<int>(std::min<long long>(grown, MAX_PAYLOAD));
        }

        payload_size_ = std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, payload_size_));
    }

private:
    static constexpr size_t HISTORY_LIMIT = 256;
    int payload_size_ = 256;
    std::deque<AttemptRecord> history_;

    double estimate_ber() const {
        int failures = 0;
        for (const auto &r : history_) {
            if (!r.ack) ++failures;
        }
        if (failures == 0) return 0.0;

        // MLE for observations where ACK probability is (1-p)^bits and
        // NAK probability is 1-(1-p)^bits. The derivative of log-likelihood
        // is monotone, so binary search finds the root reliably.
        double lo = 1e-12;
        double hi = 0.25;

        for (int iter = 0; iter < 80; ++iter) {
            double mid = (lo + hi) * 0.5;
            double d = log_likelihood_derivative(mid);
            if (d > 0.0) lo = mid;
            else hi = mid;
        }
        return (lo + hi) * 0.5;
    }

    double log_likelihood_derivative(double p) const {
        double one_minus = 1.0 - p;
        double log_one_minus = std::log1p(-p);
        double d = 0.0;

        for (const auto &r : history_) {
            double b = static_cast<double>(r.bits);
            if (r.ack) {
                d -= b / one_minus;
            } else {
                double a = std::exp(b * log_one_minus); // ACK probability
                if (a >= 1.0) {
                    d += 1e100;
                } else if (a <= 0.0) {
                    d += 0.0;
                } else {
                    d += (b * a) / (one_minus * (1.0 - a));
                }
            }
        }
        return d;
    }

    static int optimal_payload(double p) {
        p = std::max(1e-12, std::min(0.25, p));

        // Minimize expected cost per delivered byte:
        //   (P + 6 + K) / (P * (1-p)^(8(P+4)))
        // The stationary point satisfies C/(P(P+C)) = -8 ln(1-p), C=K+6.
        double a = -8.0 * std::log1p(-p);
        double c = FRAME_FIXED_COST;
        double root = (-c + std::sqrt(c * c + 4.0 * c / a)) * 0.5;

        int pld = static_cast<int>(std::llround(root));
        return std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, pld));
    }
};

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 1;

    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;

    PayloadController controller;
    std::vector<uint8_t> payload(MAX_PAYLOAD);

    while (true) {
        int want = controller.next_size();
        input.read(reinterpret_cast<char *>(payload.data()), want);
        int got = static_cast<int>(input.gcount());

        if (got <= 0) break;

        std::vector<uint8_t> frame = make_frame(payload.data(), got);

        int attempts = 0;
        while (true) {
            ++attempts;
            int r = send_frame(frame.data(), static_cast<int>(frame.size()));

            if (r == NETSIM_ACK) {
                controller.add_attempt(got, true);
                controller.finish_frame(attempts);
                break;
            }
            if (r == NETSIM_NAK) {
                controller.add_attempt(got, false);
                continue; // Stop-and-Wait: resend exactly the same frame.
            }
            return 2;
        }
    }

    return 0;
}
