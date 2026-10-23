#include "common/nal.hpp"

namespace hs {

uint64_t nal_types(Bytes b) {
    uint64_t mask = 0;
    // Emulation prevention guarantees 00 00 01 only occurs as a start code.
    for (uint32_t i = 0; i + 3 < b.n; ++i) {
        if (b.p[i] == 0 && b.p[i + 1] == 0 && b.p[i + 2] == 1) {
            mask |= uint64_t(1) << ((b.p[i + 3] >> 1) & 0x3F);
            i += 2;
        }
    }
    return mask;
}

bool is_valid_idr(Bytes b) {
    uint64_t m = nal_types(b);
    auto has = [m](uint8_t t) { return (m >> t) & 1; };
    return (has(kNalIdrWRadl) || has(kNalIdrNLp)) && has(kNalVps) && has(kNalSps) && has(kNalPps);
}

uint32_t write_nal_header(uint8_t* out, uint8_t type) {
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 1;
    out[4] = uint8_t(type << 1);
    out[5] = 1;  // nuh_temporal_id_plus1
    return 6;
}

}  // namespace hs
