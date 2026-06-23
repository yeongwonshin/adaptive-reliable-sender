#include "netsim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <vector>
/*파일을 읽어서 [크기 헤더 + payload + CRC] 형태의 frame으로 만들고,
 send_frame()으로 전송한 뒤, ACK/NAK 결과에 따라 다음 payload 크기를 자동 조절하는 프로그램이다.
*/
namespace {

const int CRC_BYTES = 4;
const int SIZE_BYTES = 2;
const int MIN_PAYLOAD = 16;
const int MAX_PAYLOAD = 65535;
const double ROUND_TRIP_COST_K = 250.0;
const double FRAME_FIXED_COST = ROUND_TRIP_COST_K + SIZE_BYTES + CRC_BYTES;
const size_t HISTORY_LIMIT = 256;

unsigned int crc_table[256];
bool crc_table_ready = false;

void init_crc_table() {
    if (crc_table_ready) return;

    const unsigned int POLY = 0x04C11DB7u;
    for (int i = 0; i < 256; ++i) {
        unsigned int r = static_cast<unsigned int>(i) << 24;
        for (int b = 0; b < 8; ++b) {
            if (r & 0x80000000u) r = (r << 1) ^ POLY;
            else r <<= 1;
        }
        crc_table[i] = r;
    }
    crc_table_ready = true;
}

unsigned int crc32_mod2(const unsigned char *data, int len) {
    init_crc_table();

    unsigned int r = 0;
    for (int i = 0; i < len; ++i) {
        unsigned char idx = static_cast<unsigned char>((r >> 24) ^ data[i]);
        r = (r << 8) ^ crc_table[idx];
    }
    return r;
}

void put_u32_be(std::vector<unsigned char> &v, int pos, unsigned int x) {
    v[pos + 0] = static_cast<unsigned char>((x >> 24) & 0xffu);
    v[pos + 1] = static_cast<unsigned char>((x >> 16) & 0xffu);
    v[pos + 2] = static_cast<unsigned char>((x >> 8) & 0xffu);
    v[pos + 3] = static_cast<unsigned char>(x & 0xffu);
}

std::vector<unsigned char> make_frame(const unsigned char *payload, int payload_size) {
    std::vector<unsigned char> frame(SIZE_BYTES + payload_size + CRC_BYTES);

    frame[0] = static_cast<unsigned char>((payload_size >> 8) & 0xff);
    frame[1] = static_cast<unsigned char>(payload_size & 0xff);
    std::memcpy(&frame[SIZE_BYTES], payload, static_cast<size_t>(payload_size));

    unsigned int crc = crc32_mod2(&frame[0], SIZE_BYTES + payload_size);
    put_u32_be(frame, SIZE_BYTES + payload_size, crc);
    return frame;
}

long long get_file_size(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return -1;
    std::streampos pos = input.tellg();
    if (pos < 0) return -1;
    return static_cast<long long>(pos);
}

int initial_payload_size(long long file_size) {
    // Small files suffer more from one bad initial oversized probe in high-BER cases.
    // Starting a little smaller reduces that risk while keeping low-BER startup overhead small.
    if (file_size >= 0 && file_size >= 256LL * 1024LL && file_size <= 1024LL * 1024LL) return 64;
    return 80;
}

struct AttemptRecord {
    int bits;
    bool ack;
    AttemptRecord(int b, bool a) : bits(b), ack(a) {}
};

class PayloadController {
public:
    explicit PayloadController(int initial_size) : payload_size_(initial_size) {}

    int next_size() const {
        return std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, payload_size_));
    }

    void add_attempt(int payload_size, bool ack) {
        // The assignment states that bit errors are injected only into payload + CRC.
        // The 2-byte size header is protected, so it is excluded from BER estimation.
        int bits = 8 * (payload_size + CRC_BYTES);
        history_.push_back(AttemptRecord(bits, ack));
        if (history_.size() > HISTORY_LIMIT) history_.pop_front();
    }

    int resize_after_nak(int current_payload_size, int consecutive_naks) {
        int current = std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, current_payload_size));
        if (consecutive_naks < 4) return current;

        double p = estimate_ber();
        if (p <= 0.0) return current;

        int target = optimal_payload(p);
        int next = current;

        // A single NAK on a good link is often just noise; splitting immediately can
        // add an extra round trip.  Resize only after repeated NAKs and only in link
        // regions where the current ACK-based controller tends to benefit from a
        // smaller retry frame.  The undelivered suffix is sent in later frames.
        if (target < current && (p < 5e-5 || p > 5e-4)) {
            next = std::max(target, current / 2);
        }

        next = std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, next));
        if (next < current) payload_size_ = next;
        return next;
    }

    void finish_frame(int attempts) {
        int naks = attempts - 1;

        double p = estimate_ber();
        if (p > 0.0) {
            int target = optimal_payload(p);

            if (naks >= 2) {
                payload_size_ = target;
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
            long long grown;
            if (payload_size_ < 2048) grown = static_cast<long long>(payload_size_) * 2LL + 1LL;
            else grown = static_cast<long long>(payload_size_) * 9LL / 5LL + 1LL;
            if (grown > MAX_PAYLOAD) grown = MAX_PAYLOAD;
            payload_size_ = static_cast<int>(grown);
        }

        payload_size_ = std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, payload_size_));
    }

private:
    int payload_size_;
    std::deque<AttemptRecord> history_;

    double estimate_ber() const {
        int failures = 0;
        for (std::deque<AttemptRecord>::const_iterator it = history_.begin(); it != history_.end(); ++it) {
            if (!it->ack) ++failures;
        }
        if (failures == 0) return 0.0;

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
        double log_one_minus = ::log(1.0 - p);
        double d = 0.0;

        for (std::deque<AttemptRecord>::const_iterator it = history_.begin(); it != history_.end(); ++it) {
            double b = static_cast<double>(it->bits);
            if (it->ack) {
                d -= b / one_minus;
            } else {
                double a = ::exp(b * log_one_minus);
                if (a >= 1.0) d += 1e100;
                else if (a <= 0.0) d += 0.0;
                else d += (b * a) / (one_minus * (1.0 - a));
            }
        }
        return d;
    }

    static int optimal_payload(double p) {
        p = std::max(1e-12, std::min(0.25, p));

        // Minimize expected cost per delivered byte:
        //   (P + 6 + K) / (P * (1-p)^(8(P+4)))
        // Stationary point: C/(P(P+C)) = -8 ln(1-p), C=K+6.
        double a = -8.0 * ::log(1.0 - p);
        double c = FRAME_FIXED_COST;
        double root = (-c + ::sqrt(c * c + 4.0 * c / a)) * 0.5;

        int pld = static_cast<int>(root + 0.5);
        return std::max(MIN_PAYLOAD, std::min(MAX_PAYLOAD, pld));
    }
};

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) return 1;

    long long file_size = get_file_size(argv[1]);
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 1;

    PayloadController controller(initial_payload_size(file_size));
    std::vector<unsigned char> payload(MAX_PAYLOAD);

    while (true) {
        int want = controller.next_size();
        std::streampos chunk_start = input.tellg();

        input.read(reinterpret_cast<char *>(&payload[0]), want);
        int got = static_cast<int>(input.gcount());

        if (got <= 0) break;

        int current_payload_size = got;
        std::vector<unsigned char> frame = make_frame(&payload[0], current_payload_size);

        int attempts = 0;
        int consecutive_naks = 0;
        while (true) {
            ++attempts;
            int r = send_frame(&frame[0], static_cast<int>(frame.size()));

            if (r == NETSIM_ACK) {
                controller.add_attempt(current_payload_size, true);
                controller.finish_frame(attempts);

                // If NAK handling reduced this frame, only the prefix has been
                // delivered.  Move the file pointer back to the first undelivered
                // byte so the suffix is transmitted next.
                if (current_payload_size < got && chunk_start >= 0) {
                    input.clear();
                    input.seekg(chunk_start + static_cast<std::streamoff>(current_payload_size));
                }
                break;
            }
            if (r == NETSIM_NAK) {
                controller.add_attempt(current_payload_size, false);
                ++consecutive_naks;

                int next_payload_size = controller.resize_after_nak(current_payload_size, consecutive_naks);
                if (next_payload_size < current_payload_size) {
                    current_payload_size = next_payload_size;
                    frame = make_frame(&payload[0], current_payload_size);
                }
                continue;
            }
            return 2;
        }
    }

    return 0;
}
