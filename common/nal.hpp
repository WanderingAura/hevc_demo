// Minimal H.265 Annex B scanner. Cheap insurance that an "IDR" really is one.
#pragma once
#include "common/types.hpp"

namespace hs {

enum NalType : uint8_t {
    kNalTrailR = 1,
    kNalIdrWRadl = 19,
    kNalIdrNLp = 20,
    kNalVps = 32,
    kNalSps = 33,
    kNalPps = 34,
};

// Bit i set if a NAL unit of type i (0..63) is present.
uint64_t nal_types(Bytes annexb);

// IDR slice (19/20) plus in-band VPS/SPS/PPS (32/33/34).
bool is_valid_idr(Bytes annexb);

// Writes a start code + 2-byte NAL header of `type`. Returns bytes written (6).
uint32_t write_nal_header(uint8_t* out, uint8_t type);

}  // namespace hs
