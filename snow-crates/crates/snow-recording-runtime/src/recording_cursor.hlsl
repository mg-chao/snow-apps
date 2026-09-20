Texture2D<float4> desktop : register(t0);
Texture2D<float4> cursorImage : register(t1);
RWTexture2D<float4> outputImage : register(u0);
cbuffer Parameters : register(b0) {
    int2 outputSize;
    int2 origin;
    int2 scaledSize;
    int2 shapeSize;
    int masked;
    int highlightRadius;
    int2 reserved;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)outputSize)) return;
    uint4 background = (uint4)round(saturate(desktop.Load(int3(id.xy, 0))) * 255.0);
    int2 position = int2(id.xy) - origin;
    if (masked == 2) {
        float2 delta = float2(position) + 0.5;
        uint alpha = (uint)round(shapeSize.y * saturate(highlightRadius + 0.5 - length(delta)));
        if (all(position >= -highlightRadius) && all(position < highlightRadius)) {
            uint3 color = uint3(scaledSize, shapeSize.x);
            uint3 factor = 255 * (255 - alpha) + alpha * color;
            background.rgb = (background.rgb * factor + 32512) / 65025;
        }
    } else if (all(position >= 0) && all(position < scaledSize)) {
        int2 source = position * shapeSize / scaledSize;
        uint4 pixel = (uint4)round(saturate(cursorImage.Load(int3(source, 0))) * 255.0);
        if (masked != 0 && (pixel.a == 0 || pixel.a == 255)) {
            background.rgb = (background.rgb & pixel.aaa) ^ pixel.rgb;
        } else {
            background.rgb = (pixel.rgb * pixel.a + background.rgb * (255 - pixel.a) + 127) / 255;
        }
    }
    outputImage[id.xy] = float4(background.rgb / 255.0, 1.0);
}
