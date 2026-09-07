cbuffer Params : register(b0) {
    float sdr_white_nits;
    float hdr_peak_nits;
    float sdr_identity_eps;
    uint flags;
    uint tex_width;
    uint tex_height;
    uint lut_size_minus_one;
    uint rotation;
    float lut_input_max;
    float lut_inv_step;
    float _pad1;
    float _pad2;
    float4 color_row_r;
    float4 color_row_g;
    float4 color_row_b;
};

Texture2D<float4> src_tex : register(t0);
Texture2D<float> luma_lut_tex : register(t1);
RWTexture2D<unorm float4> dst_tex : register(u0);

static const float PQ_M1 = 0.159301758;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;

static const float PQ_ABSOLUTE_NITS = 10000.0;
static const float SDR_REFERENCE_WHITE_NITS = 80.0;
static const float SDR_OUTPUT_WHITE_NITS = 100.0;
static const float SDR_OUTPUT_BLACK_NITS = 0.1;
static const float HDR_INPUT_BLACK_NITS = 0.001;
static const float EPSILON = 1e-6;
static const uint FLAG_USE_LUT = 1u;
static const uint FLAG_RESTORE_COLORS = 2u;

float3 restore_screen_colors(float3 rgb) {
    if ((flags & FLAG_RESTORE_COLORS) != 0u) {
        float4 input = float4(rgb, 1.0);
        return float3(dot(color_row_r, input), dot(color_row_g, input), dot(color_row_b, input));
    }
    return rgb;
}

uint2 hdr_source_coord(uint2 coord) {
    if (rotation == 1u) return uint2(coord.y, tex_width - 1u - coord.x);
    if (rotation == 2u) return uint2(tex_width - 1u - coord.x, tex_height - 1u - coord.y);
    if (rotation == 3u) return uint2(tex_height - 1u - coord.y, coord.x);
    return coord;
}

float nits_to_pq(float nits) {
    float p = pow(max(nits, 0.0) / PQ_ABSOLUTE_NITS, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p), PQ_M2);
}

float pq_to_nits(float v) {
    float p = pow(max(v, 0.0), 1.0 / PQ_M2);
    float numerator = max(p - PQ_C1, 0.0);
    float denominator = max(PQ_C2 - PQ_C3 * p, EPSILON);
    return pow(numerator / denominator, 1.0 / PQ_M1) * PQ_ABSOLUTE_NITS;
}

float bt2390_eetf_pq(float x, float iw, float ib, float ow, float ob) {
    float denom = max(iw - ib, EPSILON);
    float min_lum = (ob - ib) / denom;
    float max_lum = (ow - ib) / denom;

    float ks = 1.5 * max_lum - 0.5;
    float b = min_lum;
    float y = (x - ib) / denom;

    if (y >= ks) {
        float tb = (y - ks) / max(1.0 - ks, EPSILON);
        float tb2 = tb * tb;
        float tb3 = tb2 * tb;
        y = (2.0 * tb3 - 3.0 * tb2 + 1.0) * ks
          + (tb3 - 2.0 * tb2 + tb) * (1.0 - ks)
          + (-2.0 * tb3 + 3.0 * tb2) * max_lum;
    }

    if (y >= 0.0) {
        y += b * pow(max(1.0 - y, 0.0), 4.0);
    }

    return clamp(y * denom + ib, ob, ow);
}

float bt2390_map_linear_luma(float linear_luma, float peak_nits) {
    float l_in = max(max(linear_luma, 0.0) * SDR_REFERENCE_WHITE_NITS, HDR_INPUT_BLACK_NITS);
    float x = nits_to_pq(l_in);
    float ow = nits_to_pq(SDR_OUTPUT_WHITE_NITS);
    float ob = nits_to_pq(SDR_OUTPUT_BLACK_NITS);
    float iw = nits_to_pq(max(peak_nits, SDR_OUTPUT_WHITE_NITS + 1e-3));
    float ib = min(nits_to_pq(HDR_INPUT_BLACK_NITS), ob - 1e-3);
    float mapped = bt2390_eetf_pq(x, iw, ib, ow, ob);
    return max(pq_to_nits(mapped) / SDR_REFERENCE_WHITE_NITS, 0.0);
}

float bt2390_map_linear_luma_lut(float linear_luma) {
    float clamped = clamp(max(linear_luma, 0.0), 0.0, lut_input_max);
    float pos = clamped * max(lut_inv_step, 0.0);
    uint idx0 = min((uint)floor(pos), lut_size_minus_one);
    uint idx1 = min(idx0 + 1, lut_size_minus_one);
    float t = pos - (float)idx0;
    float y0 = luma_lut_tex[uint2(idx0, 0)];
    float y1 = luma_lut_tex[uint2(idx1, 0)];
    return lerp(y0, y1, t);
}

float linear_to_srgb(float c) {
    c = saturate(c);
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055);
}

float3 inverse_windows_sdr_boost(float3 rgb) {
    float boost = max(sdr_white_nits / SDR_REFERENCE_WHITE_NITS, EPSILON);
    return rgb / boost;
}

bool is_sdr_identity_pixel(float3 rgb) {
    float eps = max(sdr_identity_eps, 0.0);
    return max(rgb.r, max(rgb.g, rgb.b)) <= (1.0 + eps);
}

float3 tone_map_hdr_pixel_bt2390(float3 rgb) {
    float y_in = max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0.0);
    if (y_in <= EPSILON) {
        return float3(0.0, 0.0, 0.0);
    }

    float y_out = ((flags & FLAG_USE_LUT) != 0u)
        ? bt2390_map_linear_luma_lut(y_in)
        : bt2390_map_linear_luma(y_in, hdr_peak_nits);
    float scale = y_out / max(y_in, EPSILON);
    rgb *= scale;

    float max_channel = max(rgb.r, max(rgb.g, rgb.b));
    if (max_channel > 1.0) {
        rgb /= max_channel;
    }

    return rgb;
}

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint2 coord = dtid.xy;
    bool in_bounds = coord.x < tex_width && coord.y < tex_height;
    if (!in_bounds) {
        return;
    }

    float4 src = src_tex[hdr_source_coord(coord)];
    float3 rgb = inverse_windows_sdr_boost(max(restore_screen_colors(src.rgb), 0.0));
    bool is_sdr = is_sdr_identity_pixel(rgb);
    if (!is_sdr) {
        rgb = tone_map_hdr_pixel_bt2390(rgb);
    }
    float3 srgb = float3(linear_to_srgb(rgb.r), linear_to_srgb(rgb.g), linear_to_srgb(rgb.b));
    dst_tex[coord] = float4(srgb, saturate(src.a));
}

[numthreads(256, 1, 1)]
void main_1d(uint3 dtid : SV_DispatchThreadID) {
    uint2 coord = uint2(dtid.x, dtid.y);
    bool in_bounds = coord.x < tex_width && coord.y < tex_height;
    if (!in_bounds) {
        return;
    }

    float4 src = src_tex[hdr_source_coord(coord)];
    float3 rgb = inverse_windows_sdr_boost(max(restore_screen_colors(src.rgb), 0.0));
    bool is_sdr = is_sdr_identity_pixel(rgb);
    if (!is_sdr) {
        rgb = tone_map_hdr_pixel_bt2390(rgb);
    }
    float3 srgb = float3(linear_to_srgb(rgb.r), linear_to_srgb(rgb.g), linear_to_srgb(rgb.b));
    dst_tex[coord] = float4(srgb, saturate(src.a));
}

// Plain F16 linear -> sRGB conversion (no HDR tonemapping)
void convert_f16_pixel(uint2 coord, uint w, uint h) {
    if (coord.x >= w || coord.y >= h) {
        return;
    }

    float4 src = src_tex[coord];
    float3 rgb = saturate(restore_screen_colors(src.rgb));
    float3 srgb = float3(linear_to_srgb(rgb.r), linear_to_srgb(rgb.g), linear_to_srgb(rgb.b));
    dst_tex[coord] = float4(srgb, saturate(src.a));
}

[numthreads(16, 16, 1)]
void main_f16(uint3 dtid : SV_DispatchThreadID) {
    convert_f16_pixel(dtid.xy, tex_width, tex_height);
}

[numthreads(256, 1, 1)]
void main_f16_1d(uint3 dtid : SV_DispatchThreadID) {
    convert_f16_pixel(uint2(dtid.x, dtid.y), tex_width, tex_height);
}
