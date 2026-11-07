#ifndef SESSIONSCOPE_PYTHON_RANDOM_H
#define SESSIONSCOPE_PYTHON_RANDOM_H

// Python's random.Random(seed) implementation. Matches CPython 3.x exactly for
// non-negative integer seeds. Uses the reference MT19937 (mt19937ar.c) seeding
// and generation algorithms. Do not use std::mt19937: its seeding differs.
//
// The simulator depends on this: the same seed has to give the same trace forever.

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace sessionscope {

class PythonRandom {
public:
    // seed must be a non-negative integer. Python's seeding:
    //   init_genrand(19650218)
    //   init_by_array(key) where key is seed split into 32-bit words, LSB first
    explicit PythonRandom(uint64_t seed = 0);

    // Returns a float in [0.0, 1.0), exactly as Python: 53 bits from two 32-bit words.
    double random();

    // Gaussian with mean mu and standard deviation sigma. Python's exact algorithm
    // with the stored second value (Box-Muller transform).
    double gauss(double mu, double sigma);

    // Exponential distribution with rate lambda (lambd). Python: -log(1.0 - random()) / lambd.
    double expovariate(double lambd);

private:
    // MT19937 state
    std::array<uint32_t, 624> mt_;
    int mti_ = 624;  // index into mt_; 624 means uninitialized / need to generate

    // For gauss(): the second value from Box-Muller, or nullopt if none stored.
    std::optional<double> gauss_next_;

    // Core MT19937 generation
    void init_genrand(uint32_t seed);
    void init_by_array(const uint32_t *key, int key_length);
    uint32_t genrand_uint32();

    static constexpr uint32_t MATRIX_A = 0x9908b0df;
    static constexpr uint32_t UPPER_MASK = 0x80000000;
    static constexpr uint32_t LOWER_MASK = 0x7fffffff;
    static constexpr uint32_t MAG01[2] = {0x0, MATRIX_A};
};

}  // namespace sessionscope

#endif  // SESSIONSCOPE_PYTHON_RANDOM_H
