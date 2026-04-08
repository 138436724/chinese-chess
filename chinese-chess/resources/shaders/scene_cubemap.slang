struct VSOutput
{
    float2 v_UV : TEXCOORD0;
    float4 position : SV_POSITION;
};

struct FSOutput
{
    float4 outFace0;
    float4 outFace1;
    float4 outFace2;
    float4 outFace3;
    float4 outFace4;
    float4 outFace5;
}

#define UX3D_MATH_PI 3.1415926535897932384626433832795
#define UX3D_MATH_INV_PI (1.0 / UX3D_MATH_PI)

[[vk::binding(0, 0)]]
Sampler2D uPanorama;

float3 uvToXYZ(int face, float2 uv)
{
    if (face == 0)
        return float3(1.f, uv.y, -uv.x);

    else if (face == 1)
        return float3(-1.f, uv.y, uv.x);

    else if (face == 2)
        return float3(+uv.x, -1.f, +uv.y);

    else if (face == 3)
        return float3(+uv.x, 1.f, -uv.y);

    else if (face == 4)
        return float3(+uv.x, uv.y, 1.f);

    else
    { // if(face == 5)
        return float3(-uv.x, +uv.y, -1.f);
    }
}

float2 dirToUV(float3 dir)
{
    return float2(0.5f + 0.5f * atan2(dir.z, dir.x) / UX3D_MATH_PI, 1.f - acos(dir.y) / UX3D_MATH_PI);
}

[shader("vertex")]
VSOutput vertMain(uint VertexID: SV_VertexID)
{
    float x = -1.0 + float((VertexID & 1) << 2);
    float y = -1.0 + float((VertexID & 2) << 1);

    VSOutput output;
    output.v_UV = float2((x + 1.0) * 0.5, (y + 1.0) * 0.5);
    output.position = float4(x, y, 0.0, 1.0);
    return output;
}

[shader("fragment")]
FSOutput fragMain(float2 inUV)
{
    FSOutput output;

    for (int face = 0; face < 6; face++)
    {
        float3 scan = uvToXYZ(face, inUV * 2.0 - 1.0);

        float3 direction = normalize(scan);

        float2 src = dirToUV(direction);

        float4 color = float4(uPanorama.Sample(src).rgb, 1.f);

        if (face == 0)
            output.outFace0 = color;
        else if (face == 1)
            output.outFace1 = color;
        else if (face == 2)
            output.outFace2 = color;
        else if (face == 3)
            output.outFace3 = color;
        else if (face == 4)
            output.outFace4 = color;
        else // if(face == 5)
            output.outFace5 = color;
    }

    return output;
}
