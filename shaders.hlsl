cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 model;
    float4x4 view;
    float4x4 projection;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float3 position : POSITION, float4 color : COLOR)
{
    PSInput result;

    float4 newPosition = float4(position, 1.0f);

    newPosition = mul(model, newPosition);
    newPosition = mul(view, newPosition);
    newPosition = mul(projection, newPosition);

    result.position = newPosition;
    result.color = color;
    //result.color = color;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color; //input.color;
}