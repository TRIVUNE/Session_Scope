// MT19937 with Python's seeding and float conversion; python_random.h explains why.

#include "python_random.h"

#include <cmath>

namespace sessionscope {

namespace {
constexpr double TWO_PI = 2.0 * 3.14159265358979323846;
}  // namespace

PythonRandom::PythonRandom(uint64_t seed) {
    // Python's seeding: init_genrand(19650218) then init_by_array with the seed
    // split into 32-bit words, least significant first.
    // For seed=0, key is [0]. For small seeds, key is [seed].
    init_genrand(19650218);
    uint32_t key[2] = {static_cast<uint32_t>(seed & 0xffffffff), static_cast<uint32_t>(seed >> 32)};
    int key_length = (seed >> 32) != 0 ? 2 : 1;
    init_by_array(key, key_length);
}

void PythonRandom::init_genrand(uint32_t seed) {
    mt_[0] = seed;
    for (mti_ = 1; mti_ < 624; ++mti_) {
        mt_[mti_] = 1812433253 * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + mti_;
    }
}

void PythonRandom::init_by_array(const uint32_t *key, int key_length) {
    int i = 1, j = 0;
    int k = (624 > key_length) ? 624 : key_length;
    for (; k > 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525)) + key[j] + j;
        ++i; ++j;
        if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
        if (j >= key_length) j = 0;
    }
    for (k = 623; k > 0; --k) {
        mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941)) - i;
        ++i;
        if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
    }
    mt_[0] = 0x80000000;  // MSB is 1; assures non-zero initial array
}

uint32_t PythonRandom::genrand_uint32() {
    if (mti_ >= 624) {
        // Generate 624 words at once
        for (int kk = 0; kk < 624; ++kk) {
            uint32_t y = (mt_[kk] & UPPER_MASK) | (mt_[(kk + 1) % 624] & LOWER_MASK);
            mt_[kk] = mt_[(kk + 397) % 624] ^ (y >> 1) ^ MAG01[y & 1];
        }
        mti_ = 0;
    }
    uint32_t y = mt_[mti_++];
    // Tempering
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680;
    y ^= (y << 15) & 0xefc60000;
    y ^= (y >> 18);
    return y;
}

double PythonRandom::random() {
    // Python: a = genrand_uint32() >> 5, b = genrand_uint32() >> 6
    // return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0)
    uint32_t a = genrand_uint32() >> 5;   // 27 bits
    uint32_t b = genrand_uint32() >> 6;   // 26 bits
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

double PythonRandom::gauss(double mu, double sigma) {
    // Python's exact algorithm with stored second value
    double z;
    if (gauss_next_.has_value()) {
        z = *gauss_next_;
        gauss_next_.reset();
    } else {
        double x2pi = random() * TWO_PI;
        double g2rad = std::sqrt(-2.0 * std::log(1.0 - random()));
        z = std::cos(x2pi) * g2rad;
        gauss_next_ = std::sin(x2pi) * g2rad;
    }
    return mu + z * sigma;
}

double PythonRandom::expovariate(double lambd) {
    // Python: -log(1.0 - random()) / lambd
    return -std::log(1.0 - random()) / lambd;
}

}  // namespace sessionscope
