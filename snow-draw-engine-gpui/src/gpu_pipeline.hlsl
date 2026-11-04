cbuffer Parameters : register(b0) {
    float4 target;       // extent, global pixel origin
    float4 transform_matrix;       // local-to-target linear transform
    float4 translation;  // local-to-target translation, opacity, pattern
    float4 color;
    float4 sample_mapping; // target pixel to source UV
    float4 effect;       // operation, strength, support, opacity
    float4 world;        // pixel size in canvas units, canvas origin, spare
    float4 gap;          // excluded rectangle in target pixels
};
Texture2D<float4> source_image : register(t0);
Texture2D<float4> mask_image : register(t1);
Texture2D<float4> backdrop_image : register(t2);
SamplerState linear_sampler : register(s0);
SamplerState point_sampler : register(s1);

struct VertexOutput {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
};
VertexOutput mesh_vertex(float2 position : POSITION) {
    VertexOutput output;
    float2 pixel = float2(dot(position, transform_matrix.xz), dot(position, transform_matrix.yw)) + translation.xy;
    output.position = float4(pixel / target.xy * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.local = position;
    return output;
}
VertexOutput quad_vertex(uint index : SV_VertexID) {
    VertexOutput output;
    float2 uv = float2((index << 1) & 2, index & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.local = uv * target.xy;
    return output;
}
float4 mesh_pixel(VertexOutput input) : SV_Target {
    if (input.position.x >= gap.x && input.position.y >= gap.y && input.position.x < gap.z && input.position.y < gap.w) discard;
    if (translation.w < -0.5) {
        if (input.local.x < world.x || input.local.y < world.y || input.local.x >= world.z || input.local.y >= world.w) discard;
        float2 uv = input.local * sample_mapping.xy + sample_mapping.zw;
        float4 glyph = source_image.SampleLevel(linear_sampler, uv, 0);
        if (translation.w < -1.5) {
            float alpha = glyph.a * color.a * translation.z;
            return float4(color.rgb * alpha, alpha);
        }
        return glyph * translation.z;
    }
    float coverage = 1.0;
    if (translation.w > 0.5) {
        float a = input.local.x + input.local.y;
        float distance_a = abs(frac(a / 8.0 + 0.5) * 8.0 - 4.0);
        float edge_a = max(fwidth(a), 0.001);
        coverage = 1.0 - smoothstep(0.5 - edge_a * 0.5, 0.5 + edge_a * 0.5, distance_a);
        if (translation.w > 1.5) {
            float b = input.local.x - input.local.y;
            float distance_b = abs(frac(b / 8.0 + 0.5) * 8.0 - 4.0);
            float edge_b = max(fwidth(b), 0.001);
            coverage = max(coverage, 1.0 - smoothstep(0.5 - edge_b * 0.5, 0.5 + edge_b * 0.5, distance_b));
        }
    }
    float alpha = color.a * translation.z * coverage;
    return float4(color.rgb * alpha, alpha);
}
float4 straight(float4 pixel) {
    return float4(pixel.a > 0.0 ? pixel.rgb / pixel.a : 0.0, pixel.a);
}
float noise_hash(float2 noise_position) {
    return frac(sin(dot(noise_position, float2(127.1, 311.7)) + effect.y * 0.013) * 43758.5453);
}
float noise_value(float2 noise_position) {
    float2 cell = floor(noise_position), fraction = frac(noise_position);
    fraction = fraction * fraction * (3.0 - 2.0 * fraction);
    return lerp(lerp(noise_hash(cell), noise_hash(cell + float2(1, 0)), fraction.x),
                lerp(noise_hash(cell + float2(0, 1)), noise_hash(cell + 1), fraction.x), fraction.y);
}
float4 effect_pixel(VertexOutput input) : SV_Target {
    float2 pixel = input.position.xy;
    float2 uv = pixel * sample_mapping.xy + sample_mapping.zw;
    float4 sampled = source_image.SampleLevel(linear_sampler, uv, 0);
    uint operation = (uint)effect.x;
    if ((operation == 0 || operation == 1) && pixel.x >= gap.x && pixel.y >= gap.y && pixel.x < gap.z && pixel.y < gap.w) discard;
    if (operation == 0) return sampled * effect.w;
    if (operation == 1) {
        float4 destination = backdrop_image.Load(int3(pixel, 0));
        sampled *= effect.w;
        return float4(sampled.rgb * destination.rgb + sampled.rgb * (1.0 - destination.a) + destination.rgb * (1.0 - sampled.a),
                      sampled.a + destination.a * (1.0 - sampled.a));
    }
    if (operation == 2) {
        float amount = mask_image.Load(int3(pixel, 0)).a * effect.w;
        return lerp(backdrop_image.Load(int3(pixel, 0)), sampled, amount);
    }
    if (operation == 3 || operation == 4) {
        float sigma = max(effect.y, 0.01);
        int radius = (int)ceil(effect.z);
        float2 step_uv = operation == 3 ? float2(sample_mapping.x, 0) : float2(0, sample_mapping.y);
        float4 sum = 0.0;
        float weights = 0.0;
        [loop] for (int i = -radius; i <= radius; ++i) {
            float weight = exp(-0.5 * float(i * i) / (sigma * sigma));
            sum += source_image.SampleLevel(linear_sampler, uv + float(i) * step_uv, 0) * weight;
            weights += weight;
        }
        return sum / max(weights, 0.0001);
    }
    if (operation == 5) {
        float2 canvas = pixel * world.x + world.yz;
        float2 center = (floor(canvas / effect.z) + 0.5) * effect.z;
        float2 sample_pixel = floor((center - world.yz) / world.x) + 0.5;
        return source_image.SampleLevel(point_sampler, sample_pixel * sample_mapping.xy + sample_mapping.zw, 0);
    }
    float4 unassociated = straight(sampled);
    if (operation == 6) unassociated.rgb = dot(unassociated.rgb, float3(0.2126, 0.7152, 0.0722));
    if (operation == 7) unassociated.rgb = 1.0 - unassociated.rgb;
    if (operation == 8) unassociated.rgb = effect.y <= 0.5 ? unassociated.rgb * effect.y * 2.0 : lerp(unassociated.rgb, 1.0, effect.y * 2.0 - 1.0);
    if (operation == 9) {
        float2 step_uv = effect.z * sample_mapping.xy;
        float3 before = source_image.SampleLevel(linear_sampler, uv - step_uv, 0).rgb;
        float3 after = source_image.SampleLevel(linear_sampler, uv + step_uv, 0).rgb;
        unassociated.rgb = saturate(0.5 + effect.y * 10.0 * dot(after - before, float3(1.0/3.0, 1.0/3.0, 1.0/3.0)));
    }
    if (operation >= 6 && operation <= 9) return float4(saturate(unassociated.rgb) * unassociated.a, unassociated.a);
    if (operation == 10) {
        if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
        return float4(sampled.rgb * sampled.a, sampled.a) * effect.w;
    }
    if (operation == 11) {
        float alpha = (1.0 - mask_image.Load(int3(pixel, 0)).a) * color.a * effect.w;
        return float4(color.rgb * alpha, alpha);
    }
    if (operation == 12) {
        float2 canvas = pixel * world.x + world.yz;
        float2 noise = float2(noise_value(canvas * 0.045), noise_value(canvas * 0.045 + 137.0));
        noise = noise * (2.0/3.0) + float2(noise_value(canvas * 0.09), noise_value(canvas * 0.09 + 137.0)) / 3.0;
        return source_image.SampleLevel(linear_sampler, uv + (noise - 0.5) * effect.z * sample_mapping.xy, 0);
    }
    if (operation == 13) return sampled * mask_image.Load(int3(pixel, 0)).a * effect.w;
    if (operation == 14) { float alpha = sampled.a * color.a * effect.w; return float4(color.rgb * alpha, alpha); }
    if (operation == 15) {
        float2 delta = gap.zw-gap.xy;
        float amount = saturate(dot(pixel-gap.xy,delta)/max(dot(delta,delta),0.0001));
        float alpha = sampled.a*(1.0-mask_image.Load(int3(pixel,0)).a)*(1.0-backdrop_image.Load(int3(pixel,0)).a)*amount*0.3*color.a*effect.w;
        return float4(color.rgb*alpha,alpha);
    }
    if (operation == 16) return sampled*(1.0-mask_image.Load(int3(pixel,0)).a)*effect.w;
    if (operation == 17) {
        float2 coverage = saturate(min(pixel+0.5-gap.xy,gap.zw-(pixel-0.5)));
        return sampled*coverage.x*coverage.y*effect.w;
    }
    if (operation == 18) {
        return lerp(backdrop_image.Load(int3(pixel,0)), sampled, mask_image.Load(int3(pixel,0)).a);
    }
    return sampled;
}
